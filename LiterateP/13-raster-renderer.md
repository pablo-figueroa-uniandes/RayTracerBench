# Chapter 13: The Standard Graphics Pipeline — RasterRenderer and Shared Camera Math

**Abstract.** Chapters 4 and 5 covered the two ray tracers this app was built to compare. This
chapter covers a third render path, added after the fact at explicit user request: `RasterRenderer`
renders the *same* `SceneDescription` through Metal's standard graphics pipeline — a real
`MTL::RenderPipelineState` with a vertex/fragment shader pair and a depth buffer — instead of a
compute kernel. Structurally it looks like `GPURenderer` (Chapter 5): device/queue/pipeline cached
once at construction, a warm-up-then-timed draw per `render()` call. What's different is everything
downstream of "how do I get a camera matrix": rather than adding a second, parallel set of camera
parameters, `RasterRenderer`'s view/projection matrices are *derived algebraically* from the exact
same `CameraGPU` fields `RayTraceCore.h`'s `getRay()` already uses to cast rays — and that
derivation, once it turned out `PipelineStageRenderer` (Chapter 14) needed the same math for cameras
that don't come from a `CameraGPU` at all, was factored out into a small shared header,
`GPU/CameraMath.hpp`, rather than duplicated a second time.

Files covered: `GPU/RasterRenderer.hpp`, `GPU/RasterRenderer.cpp`, `GPU/CameraMath.hpp`,
`Shaders/Raster.metal`.

---

## §1. Why a third renderer, and what it deliberately does not try to be

Both existing renderers share one algorithm (`RayTraceCore.h`, Chapter 2) run two ways. A
rasterizer shares no code with that algorithm at all — it is a different rendering *strategy*, not
a third implementation of the same one — so this chapter's subject is a genuinely new module, not a
variation on Chapters 4–5's theme. The class comment states the contrast plainly:

```cpp
// GPU/RasterRenderer.hpp:19-25
// A standard graphics-pipeline (vertex/fragment + rasterizer + depth buffer) renderer for the same
// SceneDescription the CPU and GPU ray tracers consume — see CLAUDE.md's "Project status" for the
// full rationale. Unlike GPURenderer (a compute kernel), this drives a real MTL::RenderPipelineState
// over Shaders/Raster.metal, compiled from source at runtime exactly like ImageDisplayView's
// Blit.metal (it has no ShaderTypes.h/RayTraceCore.h #includes, so it doesn't need Raytracer.metal's
// xcrun/.metallib build step). Device/queue/pipeline/depth-stencil-state are created once in the
// constructor and cached, matching GPURenderer's warm-up-then-timed-dispatch methodology.
```

`Shaders/Raster.metal` (§6) has no local `#include`s — no `ShaderTypes.h`, no `RayTraceCore.h` —
so unlike `Raytracer.metal` it needs none of Chapter 12's `xcrun`/`.metallib` build-time compile
step. It is compiled from an in-memory source string at runtime, the same technique
`ImageDisplayView.cpp`'s `readShaderSource()` already uses for `Blit.metal` (Chapter 10), and for
exactly the same reason: no local `#include`s means no relative path a source string needs to
resolve.

Just as important as what this renderer does is what it is *not trying to do*: its shading is
deliberately basic (§6), and CLAUDE.md is explicit that this contrast with the physically-based
path tracer is the whole pedagogical point of the render path existing, not a gap to close later.

## §2. `RasterRenderResult`: a texture, two timings, and a triangle count instead of rays/sec

The public result type mirrors `GPURenderResult` (Chapter 5) almost field-for-field, with one
deliberate substitution:

```cpp
// GPU/RasterRenderer.hpp:10-17
struct RasterRenderResult
{
	// Not owned by the caller; valid until the next render() call on this RasterRenderer.
	MTL::Texture* pTexture;
	std::chrono::duration<double, std::milli> wallClockTime;
	double   gpuTimeMs; // CommandBuffer::GPUEndTime() - GPUStartTime(); the headline metric
	uint32_t triangleCount; // reported instead of rays/sec — rasterization has no "rays"
};
```

`AppDelegate`'s existing `raysPerSecond()` helper (Chapter 8) divides a ray tracer's pixel count by
its time — a meaningless number for a rasterizer, which draws triangles, not rays. `triangleCount`
is what `ResultsPanel` reports instead (Chapter 9), computed in §7 as simply `_indexCount / 3`.

## §3. Construction: compiling Raster.metal, and a depth-stencil state neither ray tracer needs

The constructor's shape matches `GPURenderer`'s (Chapter 5, §2–§3) closely — load a shader, build a
pipeline state, create a queue — but a rasterizer needs one more piece of one-time state a compute
kernel has no use for: a depth-stencil state, since depth testing is how a rasterizer resolves which
triangle is in front at each pixel (the ray tracer gets this for free from `hitEntity()`'s own
closest-hit search, Chapter 2).

```cpp
// GPU/RasterRenderer.cpp:56-110
RasterRenderer::RasterRenderer( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
	, _pVertexBuffer( nullptr )
	, _pIndexBuffer( nullptr )
	, _indexCount( 0 )
	, _pColorTexture( nullptr )
	, _pDepthTexture( nullptr )
	, _textureWidth( 0 )
	, _textureHeight( 0 )
{
	using NS::StringEncoding::UTF8StringEncoding;

	std::string source = readShaderSource( "Raster.metal" );

	NS::Error*    pError = nullptr;
	MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
	if ( !pLibrary )
	{
		std::fprintf( stderr, "RasterRenderer: failed to compile Raster.metal: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	MTL::Function* pVertexFn = pLibrary->newFunction( NS::String::string( "rasterVertex", UTF8StringEncoding ) );
	MTL::Function* pFragmentFn = pLibrary->newFunction( NS::String::string( "rasterFragment", UTF8StringEncoding ) );

	MTL::RenderPipelineDescriptor* pPipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pPipelineDesc->setVertexFunction( pVertexFn );
	pPipelineDesc->setFragmentFunction( pFragmentFn );
	pPipelineDesc->colorAttachments()->object( 0 )->setPixelFormat( MTL::PixelFormatRGBA8Unorm );
	pPipelineDesc->setDepthAttachmentPixelFormat( MTL::PixelFormatDepth32Float );

	_pPipelineState = _pDevice->newRenderPipelineState( pPipelineDesc, &pError );
	if ( !_pPipelineState )
	{
		std::fprintf( stderr, "RasterRenderer: failed to create render pipeline state: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	pVertexFn->release();
	pFragmentFn->release();
	pPipelineDesc->release();
	pLibrary->release();

	MTL::DepthStencilDescriptor* pDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
	pDepthDesc->setDepthCompareFunction( MTL::CompareFunctionLess );
	pDepthDesc->setDepthWriteEnabled( true );
	_pDepthStencilState = _pDevice->newDepthStencilState( pDepthDesc );
	pDepthDesc->release();

	_pCommandQueue = _pDevice->newCommandQueue();
}
```

`MTL::RenderPipelineDescriptor` is `MTL::ComputePipelineState`'s counterpart for the graphics
pipeline: it names a vertex function and a fragment function (both pulled from the same compiled
`Raster.metal` library), and declares the pixel formats of every attachment the pipeline will write
— a color attachment (`RGBA8Unorm`, matching every other texture this app displays) and a depth
attachment (`Depth32Float`). The depth-stencil state built right after is a small, separate object:
`CompareFunctionLess` plus `depthWriteEnabled(true)` is the ordinary "nearer fragments win, and
update the depth buffer as you go" policy every standard rasterizer uses. Like `GPURenderer`, both
failure paths `abort()` rather than leave a half-constructed renderer around.

## §4. Geometry: one combined vertex/index buffer, built from `buildEntityMesh()`

`rebuildGeometryBuffers()` is the rasterizer's analogue of `GPURenderer::rebuildBuffers()` — rebuilt
fresh on every `render()` call, since a scene's geometry (and vertex count) changes between calls —
but where the ray tracer just uploads the ECS component arrays as-is, the rasterizer needs actual
triangle meshes to hand a `MTL::RenderPipelineState`, which `RayTraceCore.h` has no use for at all:

```cpp
// GPU/RasterRenderer.cpp:130-167
// Flattens every entity's world-space mesh (buildEntityMesh() — the same mesh builder
// SceneExporter.cpp's glTF/OBJ writers use) plus a flat per-entity material color into one combined
// vertex/index buffer pair, offsetting indices the same way SceneExporter.cpp's OBJ writer already
// does for its own file-global vertex indices.
void RasterRenderer::rebuildGeometryBuffers( const SceneDescription& scene )
{
	if ( _pVertexBuffer )
		_pVertexBuffer->release();
	if ( _pIndexBuffer )
		_pIndexBuffer->release();

	std::vector<RasterVertex> vertices;
	std::vector<uint32_t>     indices;

	const size_t entityCount = scene.transforms.size();
	for ( size_t e = 0; e < entityCount; ++e )
	{
		MeshData    mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
		simd_float3 color = colorForMaterial( scene.materials[ scene.shapes[ e ].materialIndex ] );

		uint32_t vertexOffset = (uint32_t)vertices.size();
		for ( size_t i = 0; i < mesh.positions.size(); ++i )
			vertices.push_back( RasterVertex{ mesh.positions[ i ], mesh.normals[ i ], color } );
		for ( uint32_t index : mesh.indices )
			indices.push_back( index + vertexOffset );
	}

	const size_t vertexBytes = vertices.size() * sizeof( RasterVertex );
	const size_t indexBytes = indices.size() * sizeof( uint32_t );

	_pVertexBuffer = _pDevice->newBuffer( vertexBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pVertexBuffer->contents(), vertices.data(), vertexBytes );

	_pIndexBuffer = _pDevice->newBuffer( indexBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pIndexBuffer->contents(), indices.data(), indexBytes );

	_indexCount = (uint32_t)indices.size();
}
```

`buildEntityMesh()` is Chapter 6's mesh-building system — the same function `SceneExporter.cpp`'s
glTF/OBJ writers call per entity (Chapter 7), reused here rather than reimplemented, since it
already produces exactly what a rasterizer needs: a world-space triangle mesh with per-vertex
positions and normals, tessellated spheres, and exact faceted pyramids. `colorForMaterial()` (a
five-line local function, quoted in §6 below) is the one piece this module *doesn't* share with
`SceneExporter.cpp` — that file's `gltfMaterialFor()`/`mtlFieldsFor()` produce full PBR/MTL field
sets a flat-Lambertian shader has no use for, so a smaller, separate mapping was written rather than
force-fitting the export format's richer material model through an unused subset.

Every entity's mesh is appended into one shared `vertices`/`indices` pair, with each entity's index
values offset by the running `vertexOffset` — the identical technique `SceneExporter.cpp`'s OBJ
writer already uses for its own file-global 1-based vertex indices (Chapter 7) — so the whole scene
becomes a single combined draw (§6) rather than one draw call per entity. At ~490 entities in the
default scene, this keeps the CPU-side draw-call overhead to exactly one call regardless of scene
size, at the cost of rebuilding the whole combined buffer on every `render()` — a trade this project
accepts explicitly, since a benchmark button click is not a hot loop.

Texture management (`rebuildTargetsIfNeeded()`) mirrors `GPURenderer::rebuildTextureIfNeeded()`
(Chapter 5, §4) with one addition — a second, depth texture, created `Private` rather than `Shared`
since (unlike the color texture) nothing ever reads it back on the CPU:

```cpp
// GPU/RasterRenderer.cpp:169-197
void RasterRenderer::rebuildTargetsIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pColorTexture && width == _textureWidth && height == _textureHeight )
		return;

	if ( _pColorTexture )
		_pColorTexture->release();
	if ( _pDepthTexture )
		_pDepthTexture->release();

	// Same RGBA8Unorm/Shared format GPURenderer's output texture uses, so ImageDisplayView's
	// displayTexture() needs no changes — plus TextureUsageRenderTarget, since this one is a color
	// attachment rather than a compute kernel's ShaderWrite target.
	MTL::TextureDescriptor* pColorDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pColorDesc->setStorageMode( MTL::StorageModeShared );
	pColorDesc->setUsage( MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead );
	_pColorTexture = _pDevice->newTexture( pColorDesc );

	// Depth never needs CPU readback, so Private (GPU-only) storage is fine here, unlike the color
	// texture above.
	MTL::TextureDescriptor* pDepthDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatDepth32Float, width, height, false );
	pDepthDesc->setStorageMode( MTL::StorageModePrivate );
	pDepthDesc->setUsage( MTL::TextureUsageRenderTarget );
	_pDepthTexture = _pDevice->newTexture( pDepthDesc );

	_textureWidth = width;
	_textureHeight = height;
}
```

The color texture's format and `Shared` storage mode are chosen deliberately to match
`GPURenderer`'s own output texture (Chapter 5, §5) byte-for-byte, so `ImageDisplayView::displayTexture()`
(Chapter 10) can show either renderer's output through the exact same code path with no branching
on which renderer produced it.

## §5. Camera math: derived from `CameraGPU`, not duplicated

This is the section that gives the chapter its second half its name. A rasterizer needs a
model-view-projection matrix; the ray tracer's `CameraGPU` (Chapter 1) has never needed one — it
stores a viewport corner and two edge vectors for `getRay()` to build a per-pixel ray from. Rather
than add a parallel `viewMatrix`/`projectionMatrix` pair to `ShaderTypes.h` (risking the two
renderers' cameras ever drifting apart), the actual matrix is derived algebraically from fields
`CameraGPU` already has:

```cpp
// GPU/CameraMath.hpp:50-67
// Derives the exact perspective view-projection matrix for the scene's own CameraGPU — see
// RasterRenderer.cpp's original design note for the full derivation (focus distance recovered from
// the precomputed viewport corner/extent vectors, half-FOV tangents from viewport extent /
// focusDist, `u`/`v`/`w` already a right-handed right/up/backward basis matching Metal's -Z-forward
// view space). Kept here, not duplicated, so every consumer of "the scene's real camera" — the
// raster renderer and the pipeline-stage visualizer alike — is locked to the exact same matrix.
inline simd_float4x4 buildSceneViewProjectionMatrix( const CameraGPU& cam )
{
	simd_float3 viewportCenter = cam.lowerLeftCorner + cam.horizontal * 0.5f + cam.vertical * 0.5f;
	float       focusDist = simd_dot( cam.origin - viewportCenter, cam.w );

	float halfVFovTan = ( simd_length( cam.vertical ) * 0.5f ) / focusDist;
	float halfHFovTan = ( simd_length( cam.horizontal ) * 0.5f ) / focusDist;

	simd_float4x4 view = buildViewMatrix( cam.origin, cam.u, cam.v, cam.w );
	simd_float4x4 proj = buildPerspectiveProjection( halfHFovTan, halfVFovTan, kSceneNearPlane, kSceneFarPlane );
	return simd_mul( proj, view );
}
```

Two facts about `CameraGPU`'s existing fields make this possible without any new state. First,
`u`/`v`/`w` (built once in `Scene.cpp`'s `makeCamera()`, Chapter 1) are already exactly a
right-handed right/up/backward basis — `w = normalize(lookFrom - lookAt)`, the same convention
Metal's own -Z-forward view space uses — so the view matrix is just that basis's transpose applied
to `point - origin`:

```cpp
// GPU/CameraMath.hpp:13-26
// Builds a right-handed view matrix from an orthonormal right/up/back basis at `origin` — `back`
// is the "away from the view direction" axis (e.g. CameraGPU::w, or lookFrom-lookAt normalized),
// matching Metal's -Z-forward view space convention. view * point = the basis's transpose applied
// to (point - origin), i.e. project (point - origin) onto each basis axis.
inline simd_float4x4 buildViewMatrix( simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 back )
{
	simd_float4x4 view;
	view.columns[ 0 ] = simd_make_float4( right.x, up.x, back.x, 0.0f );
	view.columns[ 1 ] = simd_make_float4( right.y, up.y, back.y, 0.0f );
	view.columns[ 2 ] = simd_make_float4( right.z, up.z, back.z, 0.0f );
	view.columns[ 3 ] = simd_make_float4(
		-simd_dot( right, origin ), -simd_dot( up, origin ), -simd_dot( back, origin ), 1.0f );
	return view;
}
```

Second, the vertical/horizontal field of view and focus distance — which `CameraGPU` also never
stores directly — are fully recoverable from the *precomputed* viewport corner/extent vectors
`lowerLeftCorner`/`horizontal`/`vertical` that `getRay()` (Chapter 2) already uses to build rays:
the focus distance is the signed distance from the camera to the viewport rectangle's own center,
measured along `w`, and each axis's half-FOV tangent is that axis's viewport extent divided by that
same focus distance. Once those two tangents are known, a standard perspective projection matrix
follows, mapping Metal's right-handed view space to its own `[0,1]`-depth NDC convention (near maps
to NDC `z=0`, far to `z=1`):

```cpp
// GPU/CameraMath.hpp:28-40
inline simd_float4x4 buildPerspectiveProjection( float halfHFovTan, float halfVFovTan, float nearPlane, float farPlane )
{
	simd_float4x4 proj{};
	proj.columns[ 0 ] = simd_make_float4( 1.0f / halfHFovTan, 0.0f, 0.0f, 0.0f );
	proj.columns[ 1 ] = simd_make_float4( 0.0f, 1.0f / halfVFovTan, 0.0f, 0.0f );
	proj.columns[ 2 ] = simd_make_float4( 0.0f, 0.0f, farPlane / ( nearPlane - farPlane ), -1.0f );
	proj.columns[ 3 ] = simd_make_float4( 0.0f, 0.0f, ( nearPlane * farPlane ) / ( nearPlane - farPlane ), 0.0f );
	return proj;
}
```

The near/far planes themselves are fixed constants, sized off the one fact this codebase already
knows about the default scene's own scale — the ground sphere's radius-1000 extent, the same figure
Chapter 7's export-corruption story cites:

```cpp
// GPU/CameraMath.hpp:42-48
// Scene extends roughly ±1000 units around the origin (the ground sphere has radius 1000 — see
// CLAUDE.md's export-bug story for that exact figure); these give generous margin on both sides
// without meaningful depth-precision loss at this scene's scale. Shared by RasterRenderer (the
// "Viewport Matrix" pipeline stage) and PipelineStageRenderer's "Orthographic Matrix" stage, which
// must use the exact same projection to be a faithful intermediate step of the same pipeline.
constexpr float kSceneNearPlane = 0.05f;
constexpr float kSceneFarPlane = 5000.0f;
```

`CameraMath.hpp` did not start out this general. It began as a single, file-local function inside
`RasterRenderer.cpp`; Chapter 14's `PipelineStageRenderer` needed the identical view/projection
math for cameras that don't come from a `CameraGPU` at all — "external observer" viewpoints that
look *at* the scene's own camera from outside, not through it — so the view-matrix and
projection-matrix primitives were lifted out into this small, header-only file (both call sites are
tiny, host-side C++, never Metal-compiled, so there is no dual-compile concern the way
`RayTraceCore.h` has), and `RasterRenderer.cpp` was rewritten to call the shared
`buildSceneViewProjectionMatrix()` instead of keeping its own copy — a pure refactor, verified by
`RasterRendererTests` (Chapter 11) continuing to pass unmodified.

## §6. Raster.metal: a fixed light, an ambient term, and nothing more

`drawOnce()` (§7) binds this shader pair and issues one draw call for the whole combined mesh from
§4. The shader itself is short enough to quote in full:

```metal
// Shaders/Raster.metal:15-61
struct RasterVertex
{
	float3 position;
	float3 normal;
	float3 color;
};

struct Uniforms
{
	float4x4 mvp;
};

struct RasterizerData
{
	float4 position [[position]];
	float3 worldNormal;
	float3 color;
};

vertex RasterizerData rasterVertex( uint vertexID [[vertex_id]],
	constant RasterVertex* vertices [[buffer( 0 )]],
	constant Uniforms& uniforms [[buffer( 1 )]] )
{
	RasterVertex in = vertices[ vertexID ];

	RasterizerData out;
	out.position = uniforms.mvp * float4( in.position, 1.0 );
	out.worldNormal = in.normal;
	out.color = in.color;
	return out;
}

// One fixed directional light (a "sun" from above/front-right) plus a flat ambient term — a
// standard, non-physical rasterization lighting model, not an attempt to match the ray tracer's
// light transport.
fragment float4 rasterFragment( RasterizerData in [[stage_in]] )
{
	constexpr float3 kLightDir = float3( 0.4082483, 0.8164966, 0.4082483 ); // normalize((1,2,1))
	constexpr float  kAmbient = 0.2;

	float3 n = normalize( in.worldNormal );
	float  diffuse = max( dot( n, kLightDir ), 0.0 );
	float3 shaded = in.color * ( kAmbient + ( 1.0 - kAmbient ) * diffuse );
	return float4( shaded, 1.0 );
}
```

`rasterVertex` does exactly one piece of real work — transform an already-world-space position
(§4's `buildEntityMesh()` output) by the single `mvp` matrix §5 derives — and passes the world-space
normal and flat per-entity color straight through. `rasterFragment` is one Lambertian term (`N·L`
against a fixed light direction) plus a flat ambient floor, with no shadow test, no reflection, and
no global illumination. `colorForMaterial()`, the small function §4 calls to produce that flat
color, lives in `RasterRenderer.cpp` rather than the shader itself:

```cpp
// GPU/RasterRenderer.cpp:42-52
// A flat display color per material — separate from SceneExporter.cpp's gltfMaterialFor()/
// mtlFieldsFor(), which need full PBR/MTL field sets this rasterizer's flat-Lambertian shading
// has no use for. MAT_DIELECTRIC is approximated the same way the exporters already document
// (a clear surface, not modeled) — near-white rather than its near-black albedo (which is only
// meaningful for refraction, not a flat display color).
simd_float3 colorForMaterial( const MaterialGPU& mat )
{
	if ( mat.type == MAT_DIELECTRIC )
		return simd_make_float3( 1.0f, 1.0f, 1.0f );
	return mat.albedo;
}
```

`RasterVertex`/`Uniforms` are hand-duplicated between this file and `RasterRenderer.cpp` rather
than shared through a header, each side marked `KEEP IN SYNC` — the same precedent `Blit.metal`'s
`MagnifierUniforms` already established for a source-string-compiled shader with no natural shared
header (Chapter 10).

## §7. Drawing: no back-face culling, one indexed draw call, warm-up then timed

`drawOnce()` is where the pipeline state, depth-stencil state, and this scene's geometry actually
get bound and drawn:

```cpp
// GPU/RasterRenderer.cpp:202-240
double RasterRenderer::drawOnce( const SceneDescription& scene )
{
	Uniforms uniforms{ buildSceneViewProjectionMatrix( scene.camera ) };

	MTL::RenderPassDescriptor* pPassDesc = MTL::RenderPassDescriptor::renderPassDescriptor();

	MTL::RenderPassColorAttachmentDescriptor* pColorAttachment = pPassDesc->colorAttachments()->object( 0 );
	pColorAttachment->setTexture( _pColorTexture );
	pColorAttachment->setLoadAction( MTL::LoadActionClear );
	pColorAttachment->setStoreAction( MTL::StoreActionStore );
	pColorAttachment->setClearColor( MTL::ClearColor::Make( 0.0, 0.0, 0.0, 1.0 ) );

	MTL::RenderPassDepthAttachmentDescriptor* pDepthAttachment = pPassDesc->depthAttachment();
	pDepthAttachment->setTexture( _pDepthTexture );
	pDepthAttachment->setLoadAction( MTL::LoadActionClear );
	pDepthAttachment->setStoreAction( MTL::StoreActionDontCare );
	pDepthAttachment->setClearDepth( 1.0 );

	MTL::CommandBuffer*        pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::RenderCommandEncoder* pEncoder = pCommandBuffer->renderCommandEncoder( pPassDesc );

	pEncoder->setRenderPipelineState( _pPipelineState );
	pEncoder->setDepthStencilState( _pDepthStencilState );
	// No back-face culling: EntityMesh.cpp's winding order was only ever verified against "the
	// normal points away from the shape's interior" (see RayTracerBenchTests/EntityMeshTests.cpp),
	// never against a specific front-face handedness convention, so culling risks silently dropping
	// correct triangles for negligible benefit at this scene's size.
	pEncoder->setCullMode( MTL::CullModeNone );
	pEncoder->setViewport( MTL::Viewport{ 0.0, 0.0, (double)_textureWidth, (double)_textureHeight, 0.0, 1.0 } );
	pEncoder->setVertexBuffer( _pVertexBuffer, 0, 0 );
	pEncoder->setVertexBytes( &uniforms, sizeof( Uniforms ), 1 );
	pEncoder->drawIndexedPrimitives( MTL::PrimitiveTypeTriangle, _indexCount, MTL::IndexTypeUInt32, _pIndexBuffer, 0 );
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}
```

`MTL::CullModeNone` is a deliberate choice, not an oversight: `buildEntityMesh()`'s winding order
(Chapter 6) was hand-derived and verified only against "the cross-product normal points away from
the shape's interior" — a purely geometric property, independent of which way a viewer happens to
be looking — never against a specific clockwise/counter-clockwise front-face convention. Enabling
culling on an unverified handedness assumption risks silently dropping correct triangles for a
negligible performance benefit at this scene's size, so the safer default was kept. One
`drawIndexedPrimitives` call covers the whole scene's combined geometry from §4 — the entire reason
that buffer was built as one flat array rather than 490 separate small ones.

`render()`, the public entry point, follows §7's own warm-up-then-timed pattern identically to
`GPURenderer::render()` (Chapter 5, §7):

```cpp
// GPU/RasterRenderer.cpp:244-261
RasterRenderResult RasterRenderer::render( const SceneDescription& scene )
{
	rebuildGeometryBuffers( scene );
	rebuildTargetsIfNeeded( scene.params.width, scene.params.height );

	drawOnce( scene ); // untimed warm-up — see header comment

	const auto   startTime = std::chrono::high_resolution_clock::now();
	const double gpuTimeMs = drawOnce( scene );
	const auto   endTime = std::chrono::high_resolution_clock::now();

	RasterRenderResult result;
	result.pTexture = _pColorTexture;
	result.wallClockTime = endTime - startTime;
	result.gpuTimeMs = gpuTimeMs;
	result.triangleCount = _indexCount / 3;
	return result;
}
```

The one addition beyond `GPURenderer::render()`'s shape is the last line: `triangleCount` is simply
`_indexCount / 3`, reported instead of a rays/sec figure that would be meaningless for this render
path (§2).

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) defines `CameraGPU` and builds the
  one instance every renderer — this one included — consumes; §5's whole derivation depends on
  `makeCamera()`'s `u`/`v`/`w` basis and precomputed viewport vectors already being exactly what
  they are.
- **Chapter 5** (`GPU/GPURenderer.hpp/.cpp`) is the structural template this renderer's
  constructor/destructor/timing methodology follows throughout (§3, §7), and the renderer whose
  output-texture format/storage-mode this one deliberately matches (§4) so `ImageDisplayView` never
  has to branch on which renderer produced a texture.
- **Chapter 6** (`Export/EntityMesh.hpp/.cpp`) supplies the actual triangle meshes §4 flattens into
  one combined buffer — the same mesh-building system the glTF/OBJ exporters use.
- **Chapter 7** (`Export/SceneExporter.hpp/.cpp`) is where the `vertexOffset`-per-entity indexing
  technique §4 reuses was first written, for the OBJ writer's own file-global vertex indices, and
  where the export-bug story that gives `kSceneNearPlane`/`kSceneFarPlane` their scale comes from.
- **Chapter 9** (`App/ControlsPanel.hpp/.cpp`, `App/ResultsPanel.hpp/.cpp`) is where the `Render
  Raster` button and `Compare` construct/invoke a `RasterRenderer` on a background thread, and where
  `triangleCount` ends up displayed instead of a rays/sec figure.
- **Chapter 11** (`RayTracerBenchTests/RasterRendererTests.cpp`) is the smoke-test coverage for this
  module — deliberately not a pixel-parity test against the ray tracer, since a rasterized image is
  *expected* to look different (no shadows/GI/reflection).
- **Chapter 14** (`GPU/PipelineStageRenderer.hpp/.cpp`) is `CameraMath.hpp`'s other consumer — the
  reason §5's shared header exists at all instead of staying a private detail of this file — and its
  "Viewport Matrix" stage is exactly this renderer's own output, reused rather than reimplemented.
