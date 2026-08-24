# Chapter 5: The GPU Renderer

**Abstract.** `GPURenderer` is the GPU half of the CPU/GPU comparison the whole
app exists to make. It is written directly against `metal-cpp` — raw
`MTL::Device`, `MTL::Buffer`, `MTL::ComputePipelineState`, `MTL::Texture` —
with no Objective-C++ and no MetalKit convenience layer. Its shape is set by
two goals that pull in slightly different directions: the expensive,
one-time costs (device setup, shader compilation, pipeline-state creation)
must happen exactly once, at construction, so they never leak into a timed
render; and every timed render must still be preceded by its own untimed
warm-up dispatch, because even a *cached* pipeline pays some first-dispatch
cost. This chapter walks the constructor, the buffer/texture setup, the
dispatch itself, and the two timing numbers (wall-clock and GPU-only) that
`GPURenderer::render()` hands back to the UI.

Files covered: `RayTracerBench/GPU/GPURenderer.hpp`,
`RayTracerBench/GPU/GPURenderer.cpp`.

## §1. Written directly against metal-cpp, no Objective-C++

`GPURenderer` includes exactly one Apple-side header, `<Metal/Metal.hpp>` —
`metal-cpp`'s C++ header-only binding to the Metal framework — and nothing
from AppKit or Foundation beyond what that header pulls in. Every type it
touches (`MTL::Device`, `MTL::Buffer`, `MTL::CommandQueue`,
`MTL::ComputePipelineState`, `MTL::ComputeCommandEncoder`, `MTL::Texture`,
`MTL::TextureDescriptor`) is a `metal-cpp` wrapper around the Objective-C
Metal API, called with ordinary C++ method syntax:

```cpp
// RayTracerBench/GPU/GPURenderer.hpp:42-51
MTL::Device*               _pDevice;
MTL::CommandQueue*         _pCommandQueue;
MTL::ComputePipelineState* _pPipelineState;

MTL::Buffer*  _pTransformBuffer;
MTL::Buffer*  _pShapeBuffer;
MTL::Buffer*  _pMaterialBuffer;
MTL::Texture* _pOutputTexture;
uint32_t      _textureWidth;
uint32_t      _textureHeight;
```

This is consistent with the project's stated architecture goal (see
`CLAUDE.md`, "What this project is"): zero hand-written `.mm` files, with an
isolated Objective-C++ shim as the documented fallback only if a specific
widget proves unworkable through `metal-cpp`/`metal-cpp-extensions`. The GPU
renderer never needed that fallback — everything it does (loading a
library, building a pipeline, allocating buffers, encoding a compute pass)
has a direct `metal-cpp` equivalent, unlike some of the AppKit widgets
covered in later chapters.

## §2. Device, queue, and pipeline: created once, cached for the object's lifetime

The constructor is the only place `GPURenderer` ever creates its
`MTL::Device`-derived, expensive-to-build state. The header spells out why
in its own class comment:

```cpp
// RayTracerBench/GPU/GPURenderer.hpp:18-21
// Written directly against metal-cpp (no MetalKit). Device/queue/pipeline are created once in the
// constructor and cached so shader-compile latency never pollutes a render's timing. Every
// render() call does its own untimed warm-up dispatch before the timed one, so a single call's
// gpuTimeMs is never inflated by cold-start effects.
```

The device itself is handed in from outside (retained, not owned from
scratch) and stored for the object's whole lifetime, alongside the command
queue and pipeline state built from it:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:9-17
GPURenderer::GPURenderer( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
	, _pTransformBuffer( nullptr )
	, _pShapeBuffer( nullptr )
	, _pMaterialBuffer( nullptr )
	, _pOutputTexture( nullptr )
	, _textureWidth( 0 )
	, _textureHeight( 0 )
{
```

The buffers and texture start out null and are not built here at all — they
are rebuilt per-scene inside `render()` (§4), because their contents (and
even their sizes) depend on the `SceneDescription` passed to each render
call, whereas the device/queue/pipeline triad depends on nothing but the
GPU hardware and the compiled shader, both fixed for the process's whole
run. This is the same "cache what's expensive and scene-independent,
rebuild what's cheap and scene-dependent" split that shows up again in
`rebuildBuffers()`'s own comment (§4).

## §3. Loading Raytracer.metallib as a real file, not a source string

Immediately after storing the device, the constructor loads the compiled
shader library:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:18-47
using NS::StringEncoding::UTF8StringEncoding;

// RT_RAYTRACER_METALLIB (injected by CMakeLists.txt) is a real compiled .metallib, not a
// source string like ImageDisplayView's Blit.metal — Raytracer.metal #includes Core/ShaderTypes.h
// and Core/RayTraceCore.h verbatim, and a raw source string has no file location for those
// relative #includes to resolve against. See CMakeLists.txt's comment for the build-time step.
NS::Error*    pError = nullptr;
MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( RT_RAYTRACER_METALLIB, UTF8StringEncoding ), &pError );
if ( !pLibrary )
{
	std::fprintf( stderr, "GPURenderer: failed to load Raytracer.metallib: %s\n",
		pError ? pError->localizedDescription()->utf8String() : "unknown error" );
	std::abort();
}

MTL::Function* pKernelFn = pLibrary->newFunction( NS::String::string( "renderKernel", UTF8StringEncoding ) );

_pPipelineState = _pDevice->newComputePipelineState( pKernelFn, &pError );
if ( !_pPipelineState )
{
	std::fprintf( stderr, "GPURenderer: failed to create compute pipeline state: %s\n",
		pError ? pError->localizedDescription()->utf8String() : "unknown error" );
	std::abort();
}

pKernelFn->release();
pLibrary->release();

_pCommandQueue = _pDevice->newCommandQueue();
```

`newLibrary( filepath, &pError )` here is doing something categorically
different from the sibling call in `ImageDisplayView`'s blit setup, which
loads `Blit.metal` as an in-memory source string via `newLibrary(
sourceString, ... )`. The comment names the reason directly: `Raytracer.metal`
`#include`s `Core/ShaderTypes.h` and `Core/RayTraceCore.h` verbatim (the
dual-compiled algorithm core that Chapter 2 covers), and those are relative
`#include`s that only resolve against a real file location on disk — a
source string handed to `newLibrary` has no such location. Chapter 3 covers
`Raytracer.metal` itself and why it takes this path instead of Blit.metal's;
Chapter 12 covers the `CMakeLists.txt` `add_custom_command` that actually
shells out to `xcrun -sdk macosx metal` / `metallib` at build time and
injects the resulting path as the `RT_RAYTRACER_METALLIB` compile
definition consumed above. Both failure paths here (`newLibrary` and
`newComputePipelineState`) `abort()` rather than leaving a half-usable
renderer around — a deliberate fail-fast choice matching the header
comment's "Aborts on failure ... rather than leaving a half-usable
renderer."

Both the destructor and this constructor make retain/release symmetry
explicit rather than relying on any RAII wrapper: `pKernelFn` and `pLibrary`
are transient (only needed to build the pipeline state) and are released
immediately after use, while `_pDevice`, `_pCommandQueue`, and
`_pPipelineState` are held for the object's lifetime and released in the
destructor:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:49-63
GPURenderer::~GPURenderer()
{
	if ( _pOutputTexture )
		_pOutputTexture->release();
	if ( _pMaterialBuffer )
		_pMaterialBuffer->release();
	if ( _pShapeBuffer )
		_pShapeBuffer->release();
	if ( _pTransformBuffer )
		_pTransformBuffer->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pDevice->release();
}
```

## §4. Per-scene buffers: transforms, shapes, materials, mirroring the CPU renderer's raw pointers

`rebuildBuffers()` re-creates and re-uploads the three ECS component arrays
every time `render()` is called, on the theory that a handful of `Shared`
buffer allocations is cheap next to the one-time cost cached in §2:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:65-89
void GPURenderer::rebuildBuffers( const SceneDescription& scene )
{
	// Recreated on every render() call rather than capacity-cached: device/queue/pipeline are the
	// expensive, cache-worthy resources (shader compile latency); a few Shared-mode buffer
	// allocations per on-demand render is negligible in comparison.
	if ( _pTransformBuffer )
		_pTransformBuffer->release();
	if ( _pShapeBuffer )
		_pShapeBuffer->release();
	if ( _pMaterialBuffer )
		_pMaterialBuffer->release();

	const size_t transformBytes = scene.transforms.size() * sizeof( TransformGPU );
	const size_t shapeBytes = scene.shapes.size() * sizeof( ShapeGPU );
	const size_t materialBytes = scene.materials.size() * sizeof( MaterialGPU );

	_pTransformBuffer = _pDevice->newBuffer( transformBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pTransformBuffer->contents(), scene.transforms.data(), transformBytes );

	_pShapeBuffer = _pDevice->newBuffer( shapeBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pShapeBuffer->contents(), scene.shapes.data(), shapeBytes );

	_pMaterialBuffer = _pDevice->newBuffer( materialBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pMaterialBuffer->contents(), scene.materials.data(), materialBytes );
}
```

These three buffers are the GPU-side counterpart of exactly the raw
pointers `CPURenderer.cpp` pulls out of the same `SceneDescription` before
its own per-row loop (Chapter 4):

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:27-30
const TransformGPU* transforms = scene.transforms.data();
const ShapeGPU*     shapes = scene.shapes.data();
const uint32_t      entityCount = static_cast<uint32_t>( scene.transforms.size() );
const MaterialGPU*  materials = scene.materials.data();
```

On the CPU side those are plain pointers into `std::vector` storage passed
straight into `rayColor()`. On the GPU side the same three arrays have to
cross into `device`-address-space buffers a compute kernel can read, which
is exactly what `rebuildBuffers()`'s `newBuffer`+`memcpy` pairs do — one
`MTL::Buffer` per component array, byte-for-byte the same `TransformGPU`/
`ShapeGPU`/`MaterialGPU` layouts `ShaderTypes.h` defines (Chapter 1), so no
translation step sits between the CPU's `SceneDescription` and the buffers
the kernel actually reads.

`rebuildTextureIfNeeded()` handles the fourth resource, the output texture,
with a different caching policy than the buffers above — it *is* cached
across calls, and only rebuilt when the requested width/height actually
change (the same "reuse resource, replace ImageDisplayView's contents"
economy `ImageDisplayView::updatePixels()` applies to its own source
texture, covered in Chapter 10):

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:91-110
void GPURenderer::rebuildTextureIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pOutputTexture && width == _textureWidth && height == _textureHeight )
		return;

	if ( _pOutputTexture )
		_pOutputTexture->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	// Shared (not Private): ImageDisplayView blits it directly on Apple Silicon's unified memory,
	// and the deterministic-parity check reads it back on the CPU via getBytes() — both need it
	// to not be Private.
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageShaderWrite | MTL::TextureUsageShaderRead );

	_pOutputTexture = _pDevice->newTexture( pDesc );
	_textureWidth = width;
	_textureHeight = height;
}
```

## §5. `MTL::ResourceStorageModeShared`: unified memory, no explicit copy-back

Every allocation `GPURenderer` makes — the three component buffers in §4
and the output texture just above — is created in `Shared` storage mode,
never `Private`. On Apple Silicon's unified memory architecture, `Shared`
means the CPU and GPU see the same physical memory, so there is no explicit
copy-back step after the kernel writes its results: the app reads
`_pOutputTexture`'s contents (via `ImageDisplayView`'s blit, or via
`getBytes()` in a parity check) the moment the command buffer signals
completion, with no `MTL::BlitCommandEncoder` synchronizing a private GPU
buffer back to host-visible memory in between. The texture descriptor's
comment makes the two consumers that specifically require `Shared` (rather
than `Private`) explicit: `ImageDisplayView` blits the texture directly, and
the deterministic-parity tests (Chapter 11) read it back on the CPU via
`getBytes()` — both need direct CPU visibility into what the GPU wrote.

## §6. Dispatching the kernel: buffers 0–5 and an 8×8 threadgroup

`dispatchOnce()` is where a single compute pass over the whole image
actually gets encoded, bound, and dispatched:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:112-139
// Encodes and dispatches one compute pass over the whole image, waits for completion, and returns
// the GPU-only elapsed time in milliseconds (from the command buffer's own timestamps).
double GPURenderer::dispatchOnce( const SceneDescription& scene )
{
	uint32_t entityCount = static_cast<uint32_t>( scene.transforms.size() );

	MTL::CommandBuffer*         pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::ComputeCommandEncoder* pEncoder = pCommandBuffer->computeCommandEncoder();

	pEncoder->setComputePipelineState( _pPipelineState );
	pEncoder->setBuffer( _pTransformBuffer, 0, 0 );
	pEncoder->setBuffer( _pShapeBuffer, 0, 1 );
	pEncoder->setBytes( &entityCount, sizeof( entityCount ), 2 );
	pEncoder->setBuffer( _pMaterialBuffer, 0, 3 );
	pEncoder->setBytes( &scene.camera, sizeof( CameraGPU ), 4 );
	pEncoder->setBytes( &scene.params, sizeof( RenderParams ), 5 );
	pEncoder->setTexture( _pOutputTexture, 0 );

	const MTL::Size threadsPerThreadgroup( 8, 8, 1 );
	const MTL::Size threadsPerGrid( scene.params.width, scene.params.height, 1 );
	pEncoder->dispatchThreads( threadsPerGrid, threadsPerThreadgroup );
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}
```

The six `setBuffer`/`setBytes` calls at indices 0–5 line up argument for
argument with `renderKernel`'s own parameter list in `Shaders/Raytracer.metal`
(Chapter 3):

```
// RayTracerBench/Shaders/Raytracer.metal:12-18
kernel void renderKernel(
	device const TransformGPU* transforms [[buffer( 0 )]],
	device const ShapeGPU*     shapes [[buffer( 1 )]],
	constant uint32_t&         entityCount [[buffer( 2 )]],
	device const MaterialGPU*  materials [[buffer( 3 )]],
	constant CameraGPU&        camera [[buffer( 4 )]],
	constant RenderParams&     params [[buffer( 5 )]],
```

Buffers 0, 1, and 3 are the `MTL::Buffer*` objects `rebuildBuffers()` built
in §4, bound with `setBuffer`. `entityCount`, `camera`, and `params` are
each small, fixed-size values rather than persistent allocations, so they
go in via `setBytes` (index 2, 4, 5) instead — Metal copies the bytes
inline for the encoder rather than requiring a backing `MTL::Buffer` the
caller has to manage. `entityCount` mirrors the same `scene.transforms.size()`
count the CPU renderer computes locally in §4's quoted snippet; `camera`
and `params` are `scene.camera`/`scene.params` verbatim, the same
`CameraGPU`/`RenderParams` structs `Scene.hpp` builds once per scene
(Chapter 1). The output texture is bound separately via `setTexture`, since
it is the kernel's write target rather than one of its `device`/`constant`
read-only arguments.

The dispatch itself uses `dispatchThreads:threadsPerThreadgroup:` with an
8×8 threadgroup and a grid sized exactly to the image (`width` × `height` ×
1) — one GPU thread per output pixel, tiled into 8×8 groups, with no manual
padding or bounds math needed in the kernel because `dispatchThreads`
(rather than the older `dispatchThreadgroups:threadsPerThreadgroup:`)
handles a grid size that isn't an exact multiple of the threadgroup size
correctly on its own.

After `commit()` and a blocking `waitUntilCompleted()`, the function
returns `GPUEndTime() - GPUStartTime()` in milliseconds — the command
buffer's own hardware-reported timestamps for this one dispatch, which is
what makes it possible to time *just* the kernel's execution, isolated from
CPU-side encoding overhead, queue latency, or the `waitUntilCompleted()`
call itself.

## §7. Two timing numbers, and the warm-up dispatch that keeps them honest

`render()` is the public entry point, and it calls `dispatchOnce()` twice —
once thrown away, once timed:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:141-159
// Rebuilds this scene's buffers/texture, does one untimed warm-up dispatch, then one timed
// dispatch; returns the output texture plus wall-clock and GPU-only timings.
GPURenderResult GPURenderer::render( const SceneDescription& scene )
{
	rebuildBuffers( scene );
	rebuildTextureIfNeeded( scene.params.width, scene.params.height );

	dispatchOnce( scene ); // untimed warm-up — see header comment

	const auto startTime = std::chrono::high_resolution_clock::now();
	const double gpuTimeMs = dispatchOnce( scene );
	const auto endTime = std::chrono::high_resolution_clock::now();

	GPURenderResult result;
	result.pTexture = _pOutputTexture;
	result.wallClockTime = endTime - startTime;
	result.gpuTimeMs = gpuTimeMs;
	return result;
}
```

Note that the pipeline state, command queue, and device are already
cached from construction (§2) by the time `render()` runs at all — so the
warm-up dispatch here is not compensating for shader-compile latency (that
cost was already paid once, at app launch, and never repeats). It exists
because even a fully-cached pipeline still has some GPU/driver first-dispatch
cost on a freshly rebuilt set of buffers and a freshly (re)bound texture,
and `CLAUDE.md`'s "Benchmark validity" section is explicit that a warm-up
dispatch should always precede a timed run, and that each configuration
should really be run multiple times with the minimum reported — this
per-`render()`-call warm-up is the mechanism inside `GPURenderer` that
keeps a single call's `gpuTimeMs` from being skewed by that first-dispatch
effect, independent of whatever repeat/min-of-3 policy the caller layers on
top.

`GPURenderResult` carries both numbers out to the caller, with the class
comment already flagging which one matters more:

```cpp
// RayTracerBench/GPU/GPURenderer.hpp:10-16
struct GPURenderResult
{
	// Not owned by the caller; valid until the next render() call on this GPURenderer.
	MTL::Texture* pTexture;
	std::chrono::duration<double, std::milli> wallClockTime;
	double gpuTimeMs; // CommandBuffer::GPUEndTime() - GPUStartTime(); the headline metric
};
```

`wallClockTime` is measured with `std::chrono::high_resolution_clock`
around the single timed `dispatchOnce()` call — it includes command-buffer
encoding, `commit()`, and the CPU-side wait for `waitUntilCompleted()` to
return, so it is closer to what a user experiences (the interval between
"render started" and "results are back") but can be inflated by CPU-side
scheduling noise unrelated to the GPU's own work. `gpuTimeMs`, by contrast,
comes straight from the command buffer's own `GPUStartTime()`/`GPUEndTime()`
hardware timestamps (§6) and reflects only the time the GPU itself spent
executing the kernel — which is why it, not `wallClockTime`, is documented
as "the headline metric" both here and in `ResultsPanel`'s presentation of
it (Chapter 9).

## §8. What is *not* here: the private-implementation macros

Unlike `App/ImageDisplayView.cpp`, `GPURenderer.cpp` defines none of
Metal/QuartzCore's `*_PRIVATE_IMPLEMENTATION` macros — it only `#include`s
`GPURenderer.hpp`, which pulls in `<Metal/Metal.hpp>` without instantiating
the private implementation tables those macros switch on. That's
deliberate, not an oversight: each such macro may be defined in only the
one `.cpp` that first needs the private, non-header-only bodies for its
corresponding framework's classes, and this project puts
`MTL_PRIVATE_IMPLEMENTATION`/`CA_PRIVATE_IMPLEMENTATION` in
`ImageDisplayView.cpp` (with `NS_PRIVATE_IMPLEMENTATION` separately in
`main.cpp`, next to the `AppKit/AppKit.hpp` include it belongs with).
Chapter 10 covers that placement rule and the linker errors
(undefined `Private::Class`/`Private::Selector` symbols) that motivate it
in full; `GPURenderer.cpp` simply relies on whichever translation unit
already defined `MTL_PRIVATE_IMPLEMENTATION` for the whole link, the same
way any other `.cpp` in the app that merely *uses* `MTL::` types does.

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) defines the
  `TransformGPU`/`ShapeGPU`/`MaterialGPU`/`CameraGPU`/`RenderParams` structs
  that `rebuildBuffers()` copies byte-for-byte into GPU buffers, and builds
  the `SceneDescription` that `GPURenderer::render()` takes as its only
  input.
- **Chapter 2** (`Core/RayTraceCore.h`) is the algorithm `renderKernel`
  actually runs per pixel — `GPURenderer` never touches ray-tracing logic
  itself, only the plumbing (buffers, dispatch, timing) around a kernel
  whose body is this shared, dual-compiled core.
- **Chapter 3** (`Shaders/Raytracer.metal`, `Shaders/Blit.metal`) is the
  `.metal` source `renderKernel` lives in, including why it must be
  compiled to a real `.metallib` rather than loaded from an in-memory
  string the way `Blit.metal` is (§3 above).
- **Chapter 4** (`CPU/CPURenderer.hpp/.cpp`) runs the same algorithm over
  the same `transforms`/`shapes`/`entityCount`/`materials` arrays as raw
  C++ pointers instead of Metal buffers (§4 above) — the two renderers are
  the two arms of the comparison this whole app exists to make.
- **Chapter 12** (`CMakeLists.txt`) is where `RT_RAYTRACER_METALLIB` is
  actually produced, via the `xcrun -sdk macosx metal`/`metallib`
  `add_custom_command` referenced in §3.
- **Chapters 8–9** (`App/AppDelegate.hpp/.cpp`, `App/ControlsPanel.hpp/.cpp`,
  `App/ResultsPanel.hpp/.cpp`) are where `Render GPU` and `Compare` actually
  construct/invoke a `GPURenderer` on a background thread and where
  `GPURenderResult::gpuTimeMs`/`wallClockTime` end up displayed as the
  GPU-only and wall-clock numbers in `ResultsPanel`.
