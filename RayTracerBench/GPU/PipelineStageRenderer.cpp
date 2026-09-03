#include "PipelineStageRenderer.hpp"

#include "CameraMath.hpp"
#include "../Export/EntityMesh.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;

	// RT_SHADERS_DIR is injected by CMakeLists.txt — same technique RasterRenderer.cpp/
	// ImageDisplayView.cpp already use for their own runtime-compiled shaders.
	std::string readShaderSource( const char* fileName )
	{
		std::string       path = std::string( RT_SHADERS_DIR ) + "/" + fileName;
		std::ifstream     file( path );
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}

	// KEEP IN SYNC with the identical struct in Shaders/Wireframe.metal.
	struct Uniforms
	{
		simd_float4x4 mvp;
	};

	// Same flat-material-color convention RasterRenderer.cpp's colorForMaterial() uses — kept as a
	// separate small copy rather than shared, since it's five lines and the two call sites have no
	// other overlap.
	simd_float3 colorForMaterial( const MaterialGPU& mat )
	{
		if ( mat.type == MAT_DIELECTRIC )
			return simd_make_float3( 1.0f, 1.0f, 1.0f );
		return mat.albedo;
	}

	// Diagram colors — deliberately distinct from the scene's own material colors so the
	// camera/image-plane/frustum/projection scaffolding always reads clearly against the wireframe.
	constexpr simd_float3 kAxisRightColor = { 1.0f, 0.25f, 0.25f };     // red
	constexpr simd_float3 kAxisUpColor = { 0.25f, 1.0f, 0.25f };        // green
	constexpr simd_float3 kAxisForwardColor = { 0.35f, 0.55f, 1.0f };   // blue
	constexpr simd_float3 kImagePlaneColor = { 0.2f, 0.9f, 0.9f };      // cyan — the picture plane
	constexpr simd_float3 kDeformedColor = { 1.0f, 1.0f, 1.0f };        // white — the projected/deformed outline
	constexpr simd_float3 kReferenceSquareColor = { 1.0f, 1.0f, 1.0f };
	constexpr simd_float3 kSightLineColor = { 0.85f, 0.35f, 0.95f };    // magenta — eye-to-object rays, matching the classic perspective-diagram convention (e.g. the reference the user linked)
	constexpr simd_float3 kObjectBoxColor = { 0.6f, 0.6f, 0.6f };       // dim gray — the "object" bounding box the sight lines target
	constexpr simd_float3 kReferencePlaneColor = { 0.5f, 0.5f, 0.4f };  // dim olive — schematic ground/reference plane
	constexpr simd_float3 kCameraGizmoColor = { 1.0f, 0.9f, 0.4f };     // pale yellow — the camera-body gizmo, distinct from the axis triad
	constexpr float       kContextWireframeDimFactor = 0.35f;          // dims the busy full-scene wireframe so the diagram overlay above reads clearly

	void appendLine( std::vector<LineVertex>& lines, simd_float3 a, simd_float3 b, simd_float3 color )
	{
		lines.push_back( LineVertex{ a, color } );
		lines.push_back( LineVertex{ b, color } );
	}

	// A small axis triad (right=red, up=green, forward=blue) at `origin`, sized relative to
	// `scale` so it stays visible regardless of the scene's own scale.
	void appendAxisTriad( std::vector<LineVertex>& lines, simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 forward, float scale )
	{
		appendLine( lines, origin, origin + right * scale, kAxisRightColor );
		appendLine( lines, origin, origin + up * scale, kAxisUpColor );
		appendLine( lines, origin, origin + forward * scale, kAxisForwardColor );
	}

	// A small wireframe camera "gizmo" (apex at `origin`, a small rectangle out along `forward`,
	// connected like a pyramid) — the same convention 3D tools use for camera icons, distinct from
	// the plain axis triad so "here's the camera" reads clearly on its own rather than just as three
	// colored lines. Sized relative to `size` so it stays visible regardless of the scene's scale.
	void appendCameraGizmo( std::vector<LineVertex>& lines, simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 forward, float size, simd_float3 color )
	{
		simd_float3 baseCenter = origin + forward * size;
		float       halfWidth = size * 0.6f;
		float       halfHeight = size * 0.4f;

		simd_float3 c00 = baseCenter - right * halfWidth - up * halfHeight;
		simd_float3 c10 = baseCenter + right * halfWidth - up * halfHeight;
		simd_float3 c11 = baseCenter + right * halfWidth + up * halfHeight;
		simd_float3 c01 = baseCenter - right * halfWidth + up * halfHeight;

		appendLine( lines, origin, c00, color );
		appendLine( lines, origin, c10, color );
		appendLine( lines, origin, c11, color );
		appendLine( lines, origin, c01, color );

		appendLine( lines, c00, c10, color );
		appendLine( lines, c10, c11, color );
		appendLine( lines, c11, c01, color );
		appendLine( lines, c01, c00, color );
	}

	// Applies a rigid (rotation+translation) transform to a point — no perspective divide, since a
	// view-only matrix never introduces one (w stays 1).
	simd_float3 transformPointRigid( const simd_float4x4& m, simd_float3 p )
	{
		simd_float4 r = simd_mul( m, simd_make_float4( p, 1.0f ) );
		return simd_make_float3( r.x, r.y, r.z );
	}

	// Applies a full projection matrix to a point and performs the perspective divide, yielding NDC.
	simd_float3 transformPointPerspective( const simd_float4x4& m, simd_float3 p )
	{
		simd_float4 r = simd_mul( m, simd_make_float4( p, 1.0f ) );
		return simd_make_float3( r.x / r.w, r.y / r.w, r.z / r.w );
	}

	// Builds a view+projection matrix for an "external observer" camera auto-framed to see `bounds`
	// from a fixed, arbitrary 3/4-elevated offset — used by the Projective- and Camera-Matrix
	// stages, which need to look *at* the scene's own camera from outside, not through it. Distance
	// is chosen so the whole framing sphere fits within the chosen vertical FOV (standard
	// fit-a-sphere-in-a-cone formula: distance = radius / sin(halfFov)), with a small margin.
	simd_float4x4 buildObserverViewProjection( const FramingBounds& bounds, float aspect )
	{
		constexpr float kVFovDegrees = 40.0f;
		constexpr float kMarginFactor = 1.25f;

		float radius = std::max( bounds.radius, 0.01f ) * kMarginFactor;
		float halfVFov = ( kVFovDegrees * kPi / 180.0f ) * 0.5f;
		float distance = radius / std::sin( halfVFov );

		simd_float3 dir = simd_normalize( simd_make_float3( 0.6f, 0.5f, 1.0f ) );
		simd_float3 lookFrom = bounds.center + dir * distance;

		simd_float3 back = dir; // lookFrom - lookAt, normalized — already `dir` by construction
		simd_float3 worldUp = simd_make_float3( 0.0f, 1.0f, 0.0f );
		simd_float3 right = simd_normalize( simd_cross( worldUp, back ) );
		simd_float3 up = simd_cross( back, right );

		float halfVFovTan = std::tan( halfVFov );
		float halfHFovTan = halfVFovTan * aspect;

		simd_float4x4 view = buildViewMatrix( lookFrom, right, up, back );
		simd_float4x4 proj = buildPerspectiveProjection( halfHFovTan, halfVFovTan, 0.05f, distance * 4.0f + radius * 4.0f );
		return simd_mul( proj, view );
	}

	// An axis-aligned box, used as the "object" the classic eye/sight-line/picture-plane diagram
	// (see the reference image the user linked) draws sight lines to — the same curated-proxy idea
	// buildSceneWireframe() already uses for spheres (a ring instead of a full tessellated mesh),
	// applied one level up: with ~490 entities, drawing sight lines to *every* vertex would be a
	// solid fan of thousands of rays, nothing like the reference's handful of clean lines. The box
	// covers every non-excluded entity's actual extent (center ± its own bounding radius, not just
	// its center point), so it's a true bound on "the object" rather than a looser center-only box.
	struct AxisAlignedBox
	{
		simd_float3 minCorner;
		simd_float3 maxCorner;
		bool        valid;
	};

	// Only entities within a generously widened version of the camera's *actual* field of view
	// contribute to the object box. Without this, an axis-aligned box over the whole non-excluded
	// scene (which spans a much wider area at ground level than the camera's ~20° vertical FOV
	// actually captures at this distance) produces *combinatorial* corners — e.g. one sphere's
	// extreme +X mixed with a completely different, distant sphere's extreme +Z — that don't
	// correspond to any real point the camera is looking toward. Some of those land almost exactly
	// edge-on to the camera (the sight-line-to-picture-plane ray nearly parallel to the plane
	// itself), which blows up projectOntoPlane()'s ray/plane intersection to an absurd distance —
	// confirmed numerically (one such corner projected to over 400 units away, versus the scene's
	// own ~20-unit scale) and visually (the long lines shooting off past the frame edge that
	// prompted this fix). Margin is 1.5x the actual half-FOV tangent, generous enough to keep
	// "roughly in view" entities while excluding ones the camera isn't really pointed at.
	bool isRoughlyInCameraView( const CameraGPU& cam, simd_float3 point, float halfHFovTan, float halfVFovTan )
	{
		constexpr float kFovMargin = 1.0f; // exactly the camera's own FOV, no extra slack — see the comment above

		simd_float3 direction = point - cam.origin;
		float       depthAlongView = -simd_dot( direction, cam.w ); // cam.w is "backward", so forward depth is -dot
		if ( depthAlongView <= 0.01f )
			return false; // behind the camera

		float rightFrac = simd_dot( direction, cam.u ) / ( depthAlongView * halfHFovTan );
		float upFrac = simd_dot( direction, cam.v ) / ( depthAlongView * halfVFovTan );
		return std::fabs( rightFrac ) <= kFovMargin && std::fabs( upFrac ) <= kFovMargin;
	}

	AxisAlignedBox computeObjectBox( const SceneDescription& scene )
	{
		const CameraGPU& cam = scene.camera;
		simd_float3       viewportCenter = cam.lowerLeftCorner + cam.horizontal * 0.5f + cam.vertical * 0.5f;
		float             focusDist = simd_dot( cam.origin - viewportCenter, cam.w );
		float             halfVFovTan = ( simd_length( cam.vertical ) * 0.5f ) / focusDist;
		float             halfHFovTan = ( simd_length( cam.horizontal ) * 0.5f ) / focusDist;

		AxisAlignedBox box{ simd_make_float3( 0.0f, 0.0f, 0.0f ), simd_make_float3( 0.0f, 0.0f, 0.0f ), false };

		for ( size_t e = 0; e < scene.transforms.size(); ++e )
		{
			float radius = entityBoundingRadius( scene.shapes[ e ] );
			if ( radius > kFramingSizeExcludeThreshold )
				continue;
			if ( !isRoughlyInCameraView( cam, scene.transforms[ e ].position, halfHFovTan, halfVFovTan ) )
				continue;

			simd_float3 rvec = simd_make_float3( radius, radius, radius );
			simd_float3 lo = scene.transforms[ e ].position - rvec;
			simd_float3 hi = scene.transforms[ e ].position + rvec;

			if ( !box.valid )
			{
				box.minCorner = lo;
				box.maxCorner = hi;
				box.valid = true;
			}
			else
			{
				box.minCorner = simd_min( box.minCorner, lo );
				box.maxCorner = simd_max( box.maxCorner, hi );
			}
		}

		return box;
	}

	// Standard box-corner numbering (bit0=X, bit1=Y, bit2=Z; 0=min, 1=max) and its 12 edges, shared
	// by both the real object box and its image-plane projection below so they're guaranteed to use
	// the exact same connectivity.
	void cornersOf( const AxisAlignedBox& box, simd_float3 outCorners[ 8 ] )
	{
		for ( int i = 0; i < 8; ++i )
		{
			outCorners[ i ] = simd_make_float3(
				( i & 1 ) ? box.maxCorner.x : box.minCorner.x,
				( i & 2 ) ? box.maxCorner.y : box.minCorner.y,
				( i & 4 ) ? box.maxCorner.z : box.minCorner.z );
		}
	}

	constexpr int kBoxEdges[ 12 ][ 2 ] = {
		{ 0, 1 }, { 0, 2 }, { 0, 4 }, { 1, 3 }, { 1, 5 }, { 2, 3 },
		{ 2, 6 }, { 3, 7 }, { 4, 5 }, { 4, 6 }, { 5, 7 }, { 6, 7 },
	};

	void appendBoxEdges( std::vector<LineVertex>& lines, const simd_float3 corners[ 8 ], simd_float3 color )
	{
		for ( const auto& edge : kBoxEdges )
			appendLine( lines, corners[ edge[ 0 ] ], corners[ edge[ 1 ] ], color );
	}
}

// Radius of the smallest sphere centered at the shape's own local origin that contains it.
float entityBoundingRadius( const ShapeGPU& shape )
{
	if ( shape.type == SHAPE_SPHERE )
		return shape.radius;
	// Pyramid: farthest of the 5 local vertices (apex + 4 base corners) from the local origin.
	float apexDist = shape.height;
	float cornerDist = shape.baseHalfWidth * 1.41421356f; // sqrt(2)
	return std::max( apexDist, cornerDist );
}

// See the header comment: pyramids get their exact wireframe, spheres get a single equatorial ring.
std::vector<LineVertex> buildSceneWireframe( const SceneDescription& scene, bool excludeOversizedEntities )
{
	std::vector<LineVertex> lines;

	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		const TransformGPU& transform = scene.transforms[ e ];
		const ShapeGPU&      shape = scene.shapes[ e ];

		if ( excludeOversizedEntities && entityBoundingRadius( shape ) > kFramingSizeExcludeThreshold )
			continue;

		simd_float3 color = colorForMaterial( scene.materials[ shape.materialIndex ] );

		if ( shape.type == SHAPE_SPHERE )
		{
			for ( int i = 0; i < kSphereRingSegments; ++i )
			{
				float angleA = 2.0f * kPi * (float)i / (float)kSphereRingSegments;
				float angleB = 2.0f * kPi * (float)( i + 1 ) / (float)kSphereRingSegments;

				simd_float3 a = transform.position + shape.radius * ( std::cos( angleA ) * transform.right + std::sin( angleA ) * transform.forward );
				simd_float3 b = transform.position + shape.radius * ( std::cos( angleB ) * transform.right + std::sin( angleB ) * transform.forward );
				appendLine( lines, a, b, color );
			}
		}
		else
		{
			MeshData mesh = buildEntityMesh( transform, shape );
			for ( size_t i = 0; i + 2 < mesh.indices.size(); i += 3 )
			{
				simd_float3 a = mesh.positions[ mesh.indices[ i ] ];
				simd_float3 b = mesh.positions[ mesh.indices[ i + 1 ] ];
				simd_float3 c = mesh.positions[ mesh.indices[ i + 2 ] ];
				appendLine( lines, a, b, color );
				appendLine( lines, b, c, color );
				appendLine( lines, c, a, color );
			}
		}
	}

	return lines;
}

FramingBounds fitFramingSphere( const std::vector<simd_float3>& points )
{
	if ( points.empty() )
		return FramingBounds{ simd_make_float3( 0.0f, 0.0f, 0.0f ), 1.0f };

	simd_float3 sum = simd_make_float3( 0.0f, 0.0f, 0.0f );
	for ( simd_float3 p : points )
		sum += p;
	simd_float3 center = sum / (float)points.size();

	float radius = 0.0f;
	for ( simd_float3 p : points )
		radius = std::max( radius, simd_length( p - center ) );

	return FramingBounds{ center, radius };
}

std::vector<simd_float3> collectFramingPoints( const SceneDescription& scene, const std::vector<simd_float3>& extraPoints )
{
	std::vector<simd_float3> points;
	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		if ( entityBoundingRadius( scene.shapes[ e ] ) <= kFramingSizeExcludeThreshold )
			points.push_back( scene.transforms[ e ].position );
	}
	points.insert( points.end(), extraPoints.begin(), extraPoints.end() );
	return points;
}

simd_float3 projectOntoPlane( simd_float3 origin, simd_float3 point, simd_float3 planePoint, simd_float3 planeNormal )
{
	simd_float3 direction = point - origin;
	float       denom = simd_dot( direction, planeNormal );
	float       t = simd_dot( planePoint - origin, planeNormal ) / denom;
	return origin + t * direction;
}

// Compiles Wireframe.metal from source and builds the (depth-test-free — this is a wireframe
// diagram, not a solid-occlusion render, so overlapping lines are expected and fine to see through)
// render pipeline state. Aborts on failure, matching RasterRenderer/GPURenderer's contract.
PipelineStageRenderer::PipelineStageRenderer( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
	, _pProjectiveTexture( nullptr )
	, _pCameraTexture( nullptr )
	, _pOrthographicTexture( nullptr )
{
	using NS::StringEncoding::UTF8StringEncoding;

	std::string source = readShaderSource( "Wireframe.metal" );

	NS::Error*    pError = nullptr;
	MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
	if ( !pLibrary )
	{
		std::fprintf( stderr, "PipelineStageRenderer: failed to compile Wireframe.metal: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	MTL::Function* pVertexFn = pLibrary->newFunction( NS::String::string( "wireframeVertex", UTF8StringEncoding ) );
	MTL::Function* pFragmentFn = pLibrary->newFunction( NS::String::string( "wireframeFragment", UTF8StringEncoding ) );

	MTL::RenderPipelineDescriptor* pPipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pPipelineDesc->setVertexFunction( pVertexFn );
	pPipelineDesc->setFragmentFunction( pFragmentFn );
	pPipelineDesc->colorAttachments()->object( 0 )->setPixelFormat( MTL::PixelFormatRGBA8Unorm );

	_pPipelineState = _pDevice->newRenderPipelineState( pPipelineDesc, &pError );
	if ( !_pPipelineState )
	{
		std::fprintf( stderr, "PipelineStageRenderer: failed to create render pipeline state: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	pVertexFn->release();
	pFragmentFn->release();
	pPipelineDesc->release();
	pLibrary->release();

	_pCommandQueue = _pDevice->newCommandQueue();
}

PipelineStageRenderer::~PipelineStageRenderer()
{
	if ( _pOrthographicTexture )
		_pOrthographicTexture->release();
	if ( _pCameraTexture )
		_pCameraTexture->release();
	if ( _pProjectiveTexture )
		_pProjectiveTexture->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pDevice->release();
}

void PipelineStageRenderer::rebuildTargetIfNeeded( MTL::Texture** ppTexture, uint32_t width, uint32_t height )
{
	if ( *ppTexture && ( *ppTexture )->width() == width && ( *ppTexture )->height() == height )
		return;

	if ( *ppTexture )
		( *ppTexture )->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead );
	*ppTexture = _pDevice->newTexture( pDesc );
}

// Encodes and draws `lines` as a non-indexed line list into `pTexture`, no depth test.
double PipelineStageRenderer::drawLines( MTL::Texture* pTexture, const std::vector<LineVertex>& lines, simd_float4x4 mvp, uint32_t width, uint32_t height )
{
	Uniforms uniforms{ mvp };

	MTL::Buffer* pVertexBuffer = nullptr;
	if ( !lines.empty() )
	{
		const size_t bytes = lines.size() * sizeof( LineVertex );
		pVertexBuffer = _pDevice->newBuffer( bytes, MTL::ResourceStorageModeShared );
		std::memcpy( pVertexBuffer->contents(), lines.data(), bytes );
	}

	MTL::RenderPassDescriptor*                pPassDesc = MTL::RenderPassDescriptor::renderPassDescriptor();
	MTL::RenderPassColorAttachmentDescriptor* pColorAttachment = pPassDesc->colorAttachments()->object( 0 );
	pColorAttachment->setTexture( pTexture );
	pColorAttachment->setLoadAction( MTL::LoadActionClear );
	pColorAttachment->setStoreAction( MTL::StoreActionStore );
	pColorAttachment->setClearColor( MTL::ClearColor::Make( 0.05, 0.05, 0.08, 1.0 ) );

	MTL::CommandBuffer*        pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::RenderCommandEncoder* pEncoder = pCommandBuffer->renderCommandEncoder( pPassDesc );

	pEncoder->setRenderPipelineState( _pPipelineState );
	pEncoder->setViewport( MTL::Viewport{ 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 } );
	if ( pVertexBuffer )
	{
		pEncoder->setVertexBuffer( pVertexBuffer, 0, 0 );
		pEncoder->setVertexBytes( &uniforms, sizeof( Uniforms ), 1 );
		pEncoder->drawPrimitives( MTL::PrimitiveTypeLine, (NS::UInteger)0, (NS::UInteger)lines.size() );
	}
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	if ( pVertexBuffer )
		pVertexBuffer->release();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}

// Renders a classic eye/sight-line/picture-plane perspective diagram (the same composition as the
// reference the user linked: an eye, straight sight lines to the object, a picture plane the lines
// pass through with the resulting flattened shape traced on it, and a ground/reference plane the
// object rests on) for this scene's real camera and geometry. The scene's own full wireframe is
// still drawn (dimmed) as context, but the sight lines and the "deformed" shape target a curated
// bounding-box proxy for the object rather than every individual vertex — with ~490 entities, rays
// to every vertex would be a solid fan, nothing like the reference's handful of legible lines (the
// same reasoning buildSceneWireframe() already applies to spheres, one level up).
PipelineStageResult PipelineStageRenderer::renderProjectiveStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pProjectiveTexture, width, height );

	const CameraGPU& cam = scene.camera;
	simd_float3       c00 = cam.lowerLeftCorner;
	simd_float3       c10 = cam.lowerLeftCorner + cam.horizontal;
	simd_float3       c01 = cam.lowerLeftCorner + cam.vertical;
	simd_float3       c11 = cam.lowerLeftCorner + cam.horizontal + cam.vertical;

	// Context wireframe, dimmed so the diagram overlay below reads clearly against it. Excludes the
	// scene's own oversized ground sphere — see buildSceneWireframe()'s header comment for why it's
	// pure visual noise here (a schematic reference plane below stands in for "the ground" instead).
	std::vector<LineVertex> lines = buildSceneWireframe( scene, /*excludeOversizedEntities=*/true );
	for ( LineVertex& v : lines )
		v.color = v.color * kContextWireframeDimFactor;

	std::vector<simd_float3> framingPoints = collectFramingPoints( scene, { cam.origin, c00, c10, c01, c11 } );
	FramingBounds            bounds = fitFramingSphere( framingPoints );

	AxisAlignedBox objectBox = computeObjectBox( scene );
	if ( !objectBox.valid ) // degenerate scene (every entity excluded, or empty) — fall back to a small unit box at the framing center
	{
		objectBox.minCorner = bounds.center - simd_make_float3( 1.0f, 1.0f, 1.0f );
		objectBox.maxCorner = bounds.center + simd_make_float3( 1.0f, 1.0f, 1.0f );
	}
	simd_float3 corners[ 8 ];
	cornersOf( objectBox, corners );

	// Schematic ground/reference plane at the object box's lowest Y, spanning a margin beyond its
	// footprint — replaces the actual (visually confusing, per the note above) ground sphere.
	{
		float       groundY = objectBox.minCorner.y;
		simd_float3 size = objectBox.maxCorner - objectBox.minCorner;
		float       halfX = size.x * 0.75f + 1.0f;
		float       halfZ = size.z * 0.75f + 1.0f;
		simd_float3 centerXZ = simd_make_float3(
			( objectBox.minCorner.x + objectBox.maxCorner.x ) * 0.5f, groundY, ( objectBox.minCorner.z + objectBox.maxCorner.z ) * 0.5f );
		simd_float3 g00 = centerXZ + simd_make_float3( -halfX, 0.0f, -halfZ );
		simd_float3 g10 = centerXZ + simd_make_float3( halfX, 0.0f, -halfZ );
		simd_float3 g11 = centerXZ + simd_make_float3( halfX, 0.0f, halfZ );
		simd_float3 g01 = centerXZ + simd_make_float3( -halfX, 0.0f, halfZ );
		appendLine( lines, g00, g10, kReferencePlaneColor );
		appendLine( lines, g10, g11, kReferencePlaneColor );
		appendLine( lines, g11, g01, kReferencePlaneColor );
		appendLine( lines, g01, g00, kReferencePlaneColor );
	}

	// The object bounding box itself — what the sight lines below are aimed at.
	appendBoxEdges( lines, corners, kObjectBoxColor );

	// Image-plane rectangle (the "picture plane" the reference diagram calls PP).
	appendLine( lines, c00, c10, kImagePlaneColor );
	appendLine( lines, c10, c11, kImagePlaneColor );
	appendLine( lines, c11, c01, kImagePlaneColor );
	appendLine( lines, c01, c00, kImagePlaneColor );

	// Camera icon.
	appendAxisTriad( lines, cam.origin, cam.u, cam.v, -cam.w, bounds.radius * 0.08f );

	// Sight lines: one continuous ray per box corner, from the eye all the way to the real 3D
	// point (passing through the picture plane partway) — the defining feature of the reference
	// diagram, and the piece this stage was missing before: frustum edges to the *plane's own*
	// corners (the old approach) don't correspond to the object at all.
	for ( simd_float3 corner : corners )
		appendLine( lines, cam.origin, corner, kSightLineColor );

	// The "deformed" shape: the box's own corners, ray-cast through the picture plane, redrawn with
	// the box's own edge connectivity — the flattened outline traced on the plane, directly
	// analogous to the reference image's projected horse silhouette. Even with the FOV-scoped box
	// above, an axis-aligned box's corners are still *combinatorial* (e.g. one entity's extreme +X
	// mixed with a different entity's extreme +Z), so a corner can still occasionally sit at a
	// shallow, near-edge-on angle to the camera — this is a defensive backstop, not the primary
	// fix: skip (rather than draw) any edge whose ray/plane intersection parameter is out of a sane
	// range, so an unlucky corner can't paint a line shooting off to an absurd distance.
	simd_float3 projectedCorners[ 8 ];
	bool        cornerProjectionValid[ 8 ];
	for ( int i = 0; i < 8; ++i )
	{
		simd_float3 direction = corners[ i ] - cam.origin;
		float       denom = simd_dot( direction, cam.w );
		float       t = simd_dot( c00 - cam.origin, cam.w ) / denom;
		cornerProjectionValid[ i ] = ( t > 0.0f && t < 3.0f );
		projectedCorners[ i ] = cam.origin + t * direction;
	}
	for ( const auto& edge : kBoxEdges )
		if ( cornerProjectionValid[ edge[ 0 ] ] && cornerProjectionValid[ edge[ 1 ] ] )
			appendLine( lines, projectedCorners[ edge[ 0 ] ], projectedCorners[ edge[ 1 ] ], kDeformedColor );

	simd_float4x4 mvp = buildObserverViewProjection( bounds, (float)width / (float)height );
	drawLines( _pProjectiveTexture, lines, mvp, width, height );

	return PipelineStageResult{ _pProjectiveTexture };
}

PipelineStageResult PipelineStageRenderer::renderCameraStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pCameraTexture, width, height );

	const CameraGPU& cam = scene.camera;
	simd_float4x4    view = buildViewMatrix( cam.origin, cam.u, cam.v, cam.w );

	// Same exclusion as the Projective-Matrix stage — see buildSceneWireframe()'s header comment.
	std::vector<LineVertex> worldLines = buildSceneWireframe( scene, /*excludeOversizedEntities=*/true );
	std::vector<LineVertex> lines;
	lines.reserve( worldLines.size() );
	for ( const LineVertex& v : worldLines )
		lines.push_back( LineVertex{ transformPointRigid( view, v.position ), v.color } );

	std::vector<simd_float3> framingPoints = collectFramingPoints( scene, { cam.origin } );
	std::vector<simd_float3> transformedFramingPoints;
	transformedFramingPoints.reserve( framingPoints.size() );
	for ( simd_float3 p : framingPoints )
		transformedFramingPoints.push_back( transformPointRigid( view, p ) );
	FramingBounds bounds = fitFramingSphere( transformedFramingPoints );

	// The camera is now at the origin looking down -Z with +Y up, by construction of `view`. Both
	// the bare coordinate axes (so "this is (0,0,0)" is explicit) and a small camera-body gizmo
	// (so "this is the camera, facing this way" reads as more than just three colored lines) are
	// drawn there.
	simd_float3 originPoint = simd_make_float3( 0.0f, 0.0f, 0.0f );
	simd_float3 unitRight = simd_make_float3( 1.0f, 0.0f, 0.0f );
	simd_float3 unitUp = simd_make_float3( 0.0f, 1.0f, 0.0f );
	simd_float3 unitForward = simd_make_float3( 0.0f, 0.0f, -1.0f );
	appendAxisTriad( lines, originPoint, unitRight, unitUp, unitForward, bounds.radius * 0.08f );
	appendCameraGizmo( lines, originPoint, unitRight, unitUp, unitForward, bounds.radius * 0.15f, kCameraGizmoColor );

	simd_float4x4 mvp = buildObserverViewProjection( bounds, (float)width / (float)height );
	drawLines( _pCameraTexture, lines, mvp, width, height );

	return PipelineStageResult{ _pCameraTexture };
}

PipelineStageResult PipelineStageRenderer::renderOrthographicStage( const SceneDescription& scene, uint32_t width, uint32_t height )
{
	rebuildTargetIfNeeded( &_pOrthographicTexture, width, height );

	simd_float4x4 vp = buildSceneViewProjectionMatrix( scene.camera );

	std::vector<LineVertex> worldLines = buildSceneWireframe( scene );
	std::vector<LineVertex> lines;
	lines.reserve( worldLines.size() + 8 );
	for ( const LineVertex& v : worldLines )
		lines.push_back( LineVertex{ transformPointPerspective( vp, v.position ), v.color } );

	// Reference square at exactly the [-1,1] bounds the user asked to see explicitly.
	simd_float3 bl = simd_make_float3( -1.0f, -1.0f, 0.0f );
	simd_float3 br = simd_make_float3( 1.0f, -1.0f, 0.0f );
	simd_float3 tr = simd_make_float3( 1.0f, 1.0f, 0.0f );
	simd_float3 tl = simd_make_float3( -1.0f, 1.0f, 0.0f );
	appendLine( lines, bl, br, kReferenceSquareColor );
	appendLine( lines, br, tr, kReferenceSquareColor );
	appendLine( lines, tr, tl, kReferenceSquareColor );
	appendLine( lines, tl, bl, kReferenceSquareColor );

	simd_float4x4 identity = simd_matrix( simd_make_float4( 1, 0, 0, 0 ), simd_make_float4( 0, 1, 0, 0 ),
		simd_make_float4( 0, 0, 1, 0 ), simd_make_float4( 0, 0, 0, 1 ) );
	drawLines( _pOrthographicTexture, lines, identity, width, height );

	return PipelineStageResult{ _pOrthographicTexture };
}
