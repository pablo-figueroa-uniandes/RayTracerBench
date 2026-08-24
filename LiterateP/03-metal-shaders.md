# Chapter 3: The Metal Shaders

**Abstract.** This chapter covers the two `.metal` files in the program:
`Shaders/Raytracer.metal`, the GPU compute kernel that actually renders the
scene, and `Shaders/Blit.metal`, the small textured-quad shader that gets a
rendered image (from either renderer) onto screen. The two files could not
be more different in how they get compiled, and that difference is itself
the most instructive thing about them: one is compiled ahead of time from a
real file on disk because it `#include`s the shared core headers verbatim;
the other is compiled from an in-memory string at runtime because it has no
`#include`s to resolve. Blit.metal also carries a second, unrelated job —
the magnifying-glass loupe used to compare CPU and GPU output pixel-for-
pixel — which this chapter walks through in full since the shader math is
where that feature actually lives.

**Files covered:** `RayTracerBench/Shaders/Raytracer.metal`,
`RayTracerBench/Shaders/Blit.metal`.

## §1. Raytracer.metal is not a shader with logic in it — it's a thin wrapper around Chapter 2

The first thing to notice about `Raytracer.metal` is how little of it is
actually raytracing logic:

```cpp
// RayTracerBench/Shaders/Raytracer.metal:1-11
#include <metal_stdlib>
using namespace metal;

#include "../Core/ShaderTypes.h"
#include "../Core/RayTraceCore.h"

// One thread per pixel. entityCount/camera/params arrive via setBytes() (small, read-only,
// uniform across all threads) rather than being folded into RenderParams — transforms/shapes/
// materials stay separate `device` buffers (the ECS component arrays; see ShaderTypes.h/Scene.hpp)
// exactly like the CPU renderer's raw pointers, per RayTraceCore.h's RT_DEVICE design (see
// CLAUDE.md).
```

Everything that actually decides what color a pixel is — sphere and pyramid
intersection, material scattering, the bounded bounce loop — lives in
`Core/RayTraceCore.h` (Chapter 2), and everything that describes *what* is
being rendered — the transform/shape/material component arrays, the camera,
the render parameters — lives in `Core/ShaderTypes.h` (Chapter 1). This file
does not redeclare or reimplement any of it. It `#include`s both headers
verbatim, at lines 4–5, exactly the way `CPU/CPURenderer.cpp` does on the
C++ side (Chapter 4). That's the whole point of the dual-compile design
described in Chapter 2: the same source text becomes a Metal kernel here and
a plain C++ function there, so there is no second implementation to drift
out of sync with the first.

## §2. Why this file can't be compiled from a source string, and what failed when it was tried

`Blit.metal`, as we'll see in §4, is compiled from an in-memory string via
`newLibrary(sourceString, ...)`. `Raytracer.metal` cannot be, and the reason
is sitting right there in lines 4–5: `#include "../Core/ShaderTypes.h"` and
`#include "../Core/RayTraceCore.h"` are *relative* includes. A relative
`#include` is resolved against the location of the file containing it — but
when MSL source is handed to `newLibrary()` as a bare in-memory string,
there is no file location for the compiler to resolve anything against. The
string has no path; the `#include` has nothing to be relative *to*.

Per `CLAUDE.md`'s project-status notes, this is not a design that was
reasoned out in advance and avoided — it "failed loudly and immediately at
runtime the first time it was tried." Compiling `Raytracer.metal` in-memory
the way `Blit.metal` is compiled produces exactly the include-resolution
failure the paragraph above predicts, and it does so as soon as
`newLibrary()` is called, not as some later, subtler bug.

The fix is to give the file a real location to be `#include`d from, which
means giving up on runtime string compilation for this one shader and
compiling it ahead of time as an actual file on disk. `CMakeLists.txt`
(Chapter 12) does this with a build-time `add_custom_command` that shells
out to Apple's command-line Metal compiler, not Xcode's own "Metal Compiler"
build phase — kept deliberately as plain `xcrun` invocations, consistent
with this project's headless build style described elsewhere in `CLAUDE.md`:

```cmake
# CMakeLists.txt:61-87
# Raytracer.metal #includes Core/ShaderTypes.h and Core/RayTraceCore.h verbatim (the dual-compile
# design), so it CANNOT be compiled from a raw in-memory source string the way Blit.metal is —
# there's no file location for a relative #include to resolve against. Compile it to a real
# .metallib at build time instead (still command-line xcrun, not an Xcode "Metal Compiler" build
# phase, to stay consistent with this project being built headlessly), and load that compiled
# library at runtime via newLibrary(filepath, &error) rather than newLibrary(sourceString, ...).
set(RAYTRACER_METAL_SOURCE ${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders/Raytracer.metal)
set(RAYTRACER_METAL_AIR ${CMAKE_BINARY_DIR}/Raytracer.air)
set(RAYTRACER_METALLIB ${CMAKE_BINARY_DIR}/Raytracer.metallib)

add_custom_command(
    OUTPUT ${RAYTRACER_METALLIB}
    COMMAND xcrun -sdk macosx metal -c ${RAYTRACER_METAL_SOURCE} -o ${RAYTRACER_METAL_AIR}
    COMMAND xcrun -sdk macosx metallib ${RAYTRACER_METAL_AIR} -o ${RAYTRACER_METALLIB}
    DEPENDS
        ${RAYTRACER_METAL_SOURCE}
        ${CMAKE_SOURCE_DIR}/RayTracerBench/Core/ShaderTypes.h
        ${CMAKE_SOURCE_DIR}/RayTracerBench/Core/RayTraceCore.h
    COMMENT "Compiling Raytracer.metal -> Raytracer.metallib"
    VERBATIM
)
add_custom_target(RaytracerMetalLib DEPENDS ${RAYTRACER_METALLIB})
add_dependencies(RayTracerBench RaytracerMetalLib)

target_compile_definitions(RayTracerBench PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
)
```

The `DEPENDS` list is worth pausing on: it names not just
`Raytracer.metal` itself but `ShaderTypes.h` and `RayTraceCore.h` too, so
that editing either shared header (Chapters 1 and 2) correctly invalidates
and recompiles the `.metallib`, even though those headers are never listed
as sources of any CMake target in their own right.

The resulting `.metallib` path is injected into the build as the
`RT_RAYTRACER_METALLIB` compile definition, and `GPU/GPURenderer.cpp`
(Chapter 5) loads it as a real file path rather than a source string:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:20-25
// RT_RAYTRACER_METALLIB (injected by CMakeLists.txt) is a real compiled .metallib, not a
// source string like ImageDisplayView's Blit.metal — Raytracer.metal #includes Core/ShaderTypes.h
// and Core/RayTraceCore.h verbatim, and a raw source string has no file location for those
// relative #includes to resolve against. See CMakeLists.txt's comment for the build-time step.
NS::Error*    pError = nullptr;
MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( RT_RAYTRACER_METALLIB, UTF8StringEncoding ), &pError );
```

The overload being called here — `newLibrary(NS::String* filepath, NS::Error**)`
— is a different metal-cpp entry point than `Blit.metal`'s
`newLibrary(NS::String* source, MTL::CompileOptions*, NS::Error**)`; the
argument happens to be an `NS::String` in both cases, but in this call it's
a path Metal opens and parses as a precompiled library, not MSL text Metal
has to lex and compile. Chapter 5 covers the rest of `GPURenderer`'s
construction — pipeline state creation, command queue setup — that follows
this load.

## §3. The kernel signature: six buffers mirroring the CPU renderer's raw pointers

`renderKernel` is a `kernel void` function — a Metal compute kernel, one
invocation per grid position, with no return value; results are written
directly into a texture. Its argument list is a direct, buffer-by-buffer
translation of the same data `CPURenderer.cpp` walks with raw C++ pointers
(Chapter 4):

```cpp
// RayTracerBench/Shaders/Raytracer.metal:12-20
kernel void renderKernel(
	device const TransformGPU* transforms [[buffer( 0 )]],
	device const ShapeGPU*     shapes [[buffer( 1 )]],
	constant uint32_t&         entityCount [[buffer( 2 )]],
	device const MaterialGPU*  materials [[buffer( 3 )]],
	constant CameraGPU&        camera [[buffer( 4 )]],
	constant RenderParams&     params [[buffer( 5 )]],
	texture2d<float, access::write> outTexture [[texture( 0 )]],
	uint2                     gid [[thread_position_in_grid]] )
```

Buffers 0, 1, and 3 — `transforms`, `shapes`, `materials` — are the ECS
component arrays from `Scene.hpp` (Chapter 1), each qualified `device
const`: potentially large, read-only, and indexed per-entity rather than
uniform across threads. This is precisely the one spot in the shared core
that needs the `RT_DEVICE` address-space macro described in Chapter 2 —
`hitEntity()` and `rayColor()` take these three as `device`-qualified
pointers, the only address-space keyword the dual-compiled core ever needs,
because CPU code has no equivalent concept and the macro compiles away to
nothing there.

Buffers 2, 4, and 5 — `entityCount`, `camera`, `params` — are `constant`
references instead of `device` pointers. The comment at lines 7–11 explains
the distinction directly: these three are small and identical for every
thread in the dispatch (there is one camera, one entity count, one set of
render parameters for the whole frame), so they're passed via `setBytes()`
on the host side and read as `constant` — Metal's address space for
uniform, read-only, broadcast data — rather than being folded into
`RenderParams` itself or treated as another per-entity `device` array.

`outTexture` is the kernel's actual output — a `texture2d<float,
access::write>`, written to exactly once per thread at the end of the
function — and `gid`, `[[thread_position_in_grid]]`, is Metal's built-in
per-thread grid coordinate, used both as the pixel being rendered and as
part of the per-pixel RNG seed.

## §4. What the kernel body does, briefly — and where the dispatch itself lives

The body is short precisely because it delegates almost everything to
Chapter 2's functions. After an early-out for threads past the image
bounds (line 22–23, needed because the dispatch grid is rounded up to a
multiple of the threadgroup size — see below), it hashes a per-pixel seed,
then loops `samplesPerPixel` times:

```cpp
// RayTracerBench/Shaders/Raytracer.metal:29-46
for ( uint32_t s = 0; s < params.samplesPerPixel; ++s )
{
	RandomFloatSample ru = randomFloat( seed );
	seed = ru.seed;
	RandomFloatSample rv = randomFloat( seed );
	seed = rv.seed;

	float u = ( float( gid.x ) + ru.value ) / float( params.width - 1 );
	float v = ( float( params.height - 1 - gid.y ) + rv.value ) / float( params.height - 1 );

	CameraRaySample cameraRay = getRay( camera, u, v, seed );
	seed = cameraRay.rngSeed;

	RayColorResult sample = rayColor( cameraRay.ray, transforms, shapes, entityCount, materials, params.maxDepth, seed );
	seed = sample.rngSeed;

	colorSum = colorSum + sample.color;
}
```

Every function called here — `randomFloat`, `getRay`, `rayColor` — is
declared in `RayTraceCore.h` and explained in Chapter 2; nothing about
their internals is specific to the GPU. The `seed = ru.seed` /
`seed = sample.rngSeed` threading visible on nearly every line is Chapter
2's value-in/value-out RNG convention showing up directly at the call site:
no function here mutates shared state, so the same loop, unchanged, is also
what `CPURenderer.cpp` runs per pixel.

After averaging the accumulated samples, the kernel applies gamma
correction and a clamp before writing the pixel:

```cpp
// RayTracerBench/Shaders/Raytracer.metal:48-52
simd_float3 averageColor = colorSum / float( params.samplesPerPixel );
simd_float3 gammaCorrected = sqrt( max( averageColor, 0.0f ) );
simd_float3 clamped = min( gammaCorrected, makeFloat3( 0.999f, 0.999f, 0.999f ) );

outTexture.write( float4( clamped, 1.0 ), gid );
```

`sqrt` here is gamma-2 correction (the standard RTIOW convention), and the
`0.999f` clamp keeps the written color strictly below 1.0. `makeFloat3()` is
the same helper from `RayTraceCore.h` mentioned in Chapter 2's note that
`simd_float3(x, y, z)` function-call construction doesn't compile as plain
C++, even though it's fine in MSL — used here for consistency with the
CPU-side call sites, even though this file, being pure MSL, could have used
`float3(x, y, z)` directly.

The comment at line 7 that opens the file also notes this kernel is "one
thread per pixel," dispatched 8×8 per threadgroup:

```cpp
// RayTracerBench/GPU/GPURenderer.cpp:130-132
const MTL::Size threadsPerThreadgroup( 8, 8, 1 );
...
pEncoder->dispatchThreads( threadsPerGrid, threadsPerThreadgroup );
```

That call — computing `threadsPerGrid`, encoding the compute command,
binding the six buffers to buffer indices 0–5 in the exact order this
kernel signature expects, and reading back the timed result — is
`GPURenderer`'s job and belongs to Chapter 5. What matters here is just
that the kernel's bounds check at lines 22–23 (`if ( gid.x >= params.width
|| gid.y >= params.height ) return;`) exists *because* an 8×8 threadgroup
grid, dispatched over an arbitrary image width and height, is rounded up to
the next multiple of 8 in each dimension — some threads in the last row or
column of threadgroups fall outside the actual image and must do nothing.

## §5. Blit.metal: simple enough to need no file at all

`Blit.metal` has a completely different job: it doesn't compute any pixel
colors from scratch, it draws an already-rendered texture onto the screen.
Its header comment states its two jobs plainly:

```cpp
// RayTracerBench/Shaders/Blit.metal:1-11
// Fullscreen textured-quad blit: draws ImageDisplayView's source texture (the CPU renderer's
// RGBA8 output today; a GPU-rendered texture directly, once that exists) onto the view's
// CAMetalLayer drawable. Two triangles, no vertex buffer — positions/texCoords are baked into
// constant arrays indexed by vertex_id.
//
// Also implements the magnifying-glass loupe: when active, fragments inside a circular region
// around a lens center sample the source texture from a smaller, de-magnified region instead of
// 1:1, producing a zoomed inset. MagnifierUniforms is compiled from a raw in-memory source string
// (see ImageDisplayView.cpp's readShaderSource() comment — unlike Raytracer.metal, this file has
// no local #includes, so that's fine), so its layout is hand-duplicated in ImageDisplayView.cpp
// rather than shared via a header — KEEP IN SYNC if either side changes.
```

That last clause of the comment is the direct counterpart of §2's
discussion: `Blit.metal` has no `#include "../Core/..."` anywhere in it —
its only include is the standard `<metal_stdlib>` — so there is nothing a
missing file location could break. That's what makes it safe to compile
from an in-memory string, and `ImageDisplayView.cpp` (Chapter 10) does
exactly that:

```cpp
// RayTracerBench/App/ImageDisplayView.cpp:16-29
// RT_SHADERS_DIR is injected by CMakeLists.txt as the absolute path to RayTracerBench/Shaders.
// Loading .metal source from disk at runtime (rather than a compiled-in default.metallib) is a
// deliberate consequence of this project deliberately not being an app bundle (see CLAUDE.md's
// "Command Line Tool template, not App template" decision) — Apple's own LearnMetalCPP samples
// use the equivalent technique of compiling MSL from a source string via newLibrary() rather
// than newDefaultLibrary(), just with the string embedded instead of read from a sibling file.
std::string readShaderSource( const char* fileName )
{
	std::string   path = std::string( RT_SHADERS_DIR ) + "/" + fileName;
	std::ifstream file( path );
	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}
```

```cpp
// RayTracerBench/App/ImageDisplayView.cpp:57-60
std::string source = readShaderSource( "Blit.metal" );

NS::Error* pError = nullptr;
MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
```

Note the two-step split: the file is still read from disk at the path
`RT_SHADERS_DIR` (a plain `CMakeLists.txt`-injected compile definition
pointing at `RayTracerBench/Shaders`, per `CMakeLists.txt:57-59`) so that
editing `Blit.metal` doesn't require a rebuild of the host executable — but
once the bytes are in hand, they're handed to Metal as a source *string*,
not a source *path*. `newLibrary()` compiles that string at that moment,
during `ImageDisplayView`'s constructor, rather than reading a precompiled
`.metallib`. This is the `newLibrary(source, options, &error)` overload,
distinct from the `newLibrary(filepath, &error)` overload `GPURenderer`
uses for `Raytracer.metal` in §2. `Blit.metal`'s own vertex data needs no
buffer either — the two triangles making up the fullscreen quad are baked
directly into `constant` arrays and indexed by `vertex_id`:

```cpp
// RayTracerBench/Shaders/Blit.metal:22-32
constant float2 kQuadPositions[ 6 ] = {
	float2( -1.0, -1.0 ), float2( 1.0, -1.0 ), float2( -1.0, 1.0 ),
	float2( -1.0, 1.0 ), float2( 1.0, -1.0 ), float2( 1.0, 1.0 )
};

// texCoord.y=0 at the top row: matches ImageDisplayView's source texture, which is uploaded via
// replaceRegion() directly from a row-major, row-0-is-top RGBA8 buffer (see CPURenderer.hpp).
constant float2 kQuadTexCoords[ 6 ] = {
	float2( 0.0, 1.0 ), float2( 1.0, 1.0 ), float2( 0.0, 0.0 ),
	float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 1.0, 0.0 )
};
```

`blitVertex` (lines 46–52) does nothing but pick out one of these six
position/texCoord pairs by `vertex_id` and pass it through to the rasterizer
as `RasterizerData`; there is no transform, no camera, no projection — the
quad already spans clip space `[-1, 1]` on both axes.

## §6. The magnifying-glass loupe: circular, aspect-corrected, de-magnifying sample

`blitFragment` is where the loupe effect actually lives. Its uniform
struct, `MagnifierUniforms`, is deliberately not shared via a header with
`Core/ShaderTypes.h` — since `Blit.metal` is compiled from a source string
at runtime rather than alongside a build-time include path, there's no
convenient shared compile step for a small file like this one, so its
layout is hand-duplicated on the host side in `ImageDisplayView.cpp`
instead, with an explicit `KEEP IN SYNC` comment marking both copies:

```cpp
// RayTracerBench/Shaders/Blit.metal:34-43
// KEEP IN SYNC with the identical struct in ImageDisplayView.cpp.
struct MagnifierUniforms
{
	float centerU;
	float centerV;
	float viewAspect; // width / height, so the lens reads as a circle rather than an ellipse
	float radius;     // normalized, as a fraction of view height
	float zoom;
	int   active;     // 0 = disabled; draw the source texture unmodified
};
```

The fragment shader itself branches on `magnifier.active`, falling through
to a plain 1:1 texture sample when the loupe isn't in use:

```cpp
// RayTracerBench/Shaders/Blit.metal:56-86
fragment float4 blitFragment( RasterizerData in [[stage_in]],
	texture2d<float, access::sample> sourceTexture [[texture( 0 )]],
	constant MagnifierUniforms& magnifier [[buffer( 0 )]] )
{
	constexpr sampler linearSampler( coord::normalized, address::clamp_to_edge, filter::linear );

	if ( magnifier.active != 0 )
	{
		float2 center = float2( magnifier.centerU, magnifier.centerV );
		float2 delta = in.texCoord - center;
		// Correct for aspect ratio: 1 unit of U spans `width` pixels but 1 unit of V spans
		// `height` pixels, so scale U by viewAspect to compare both in "V-equivalent" units —
		// otherwise the lens would read as an ellipse on a non-square view.
		float2 deltaScreen = float2( delta.x * magnifier.viewAspect, delta.y );
		float  dist = length( deltaScreen );

		if ( dist < magnifier.radius )
		{
			float2 zoomedUV = center + delta / magnifier.zoom;
			float4 color = sourceTexture.sample( linearSampler, zoomedUV );

			// Thin white border ring so the lens's edge reads clearly against any image content.
			float ringWidth = magnifier.radius * 0.035;
			float ring = smoothstep( magnifier.radius - ringWidth, magnifier.radius, dist );
			color.rgb = mix( color.rgb, float3( 1.0, 1.0, 1.0 ), ring * 0.85 );
			return color;
		}
	}

	return sourceTexture.sample( linearSampler, in.texCoord );
}
```

Walking through the math: `delta` is the current fragment's texture
coordinate offset from the lens center, in normalized `[0,1]` UV units.
Because UV space treats U and V as running over the same `[0,1]` range
regardless of the view's actual pixel width and height, a fixed-radius
circle in raw UV distance would look like an ellipse on any non-square
view — one unit of U movement covers `width` pixels, one unit of V movement
covers `height` pixels. `deltaScreen` fixes this by scaling `delta.x` by
`viewAspect` (`width / height`, computed once in `ImageDisplayView`'s
constructor and stored in the uniforms), converting both axes into a common
"V-equivalent" unit before measuring `dist = length(deltaScreen)`. Only
then does comparing `dist` against `magnifier.radius` describe an actual
circle on screen.

Inside that circle, `zoomedUV = center + delta / magnifier.zoom` is the
de-magnification step: rather than sampling `in.texCoord` directly, the
shader samples a point pulled *toward* the lens center by a factor of
`zoom` — with `zoom = 4.0` (the default, per `ImageDisplayView.cpp:41`'s
`_magnifier{ 0.5f, 0.5f, 1.0f, 0.18f, 4.0f, 0 }` initializer), a fragment
one full lens-radius away from center samples a source texel only a
quarter of a lens-radius away from center. That shrinkage is what makes the
region inside the lens look like a 4x-zoomed inset of the texture around
`center`, rather than the same image repeated at a different position.

The `ring` calculation is a cosmetic finishing touch, not part of the
zoom math: `smoothstep(radius - ringWidth, radius, dist)` produces a value
that ramps from 0 to 1 only in a thin band just inside the lens's edge
(`ringWidth` is 3.5% of the radius), and `mix(color.rgb, white, ring *
0.85)` blends that band toward white — a border thin and subtle enough not
to obscure the zoomed content, but visible enough that the lens's
boundary reads clearly against any image behind it.

None of this — `centerU`/`centerV`, `active` — is computed by the shader
itself; those come from `MagnifierUniforms` values the host side sets each
frame from live mouse position, which is `ImageDisplayView`'s job
(Chapter 10) via its `NS::EventMaskMouseMoved` local event monitor. What's
covered here is only the shader-side geometry: given a center and a state,
how the fragment shader decides which texel to actually draw.

## §7. How the lens math was checked, not just written

`CLAUDE.md`'s project-status notes describe this feature as "verified three
ways rather than assumed," which is worth restating here since it applies
directly to the shader code just walked through:

1. **An offscreen shader test with a known 2px marker**, confirming exact
   4x magnification with no positional drift — i.e., feeding
   `blitFragment` a source texture with a marker at a known location and
   confirming the zoomed output places that marker at exactly the position
   the `zoomedUV` formula above predicts, at exactly 4x scale.
2. **A coordinate-math test using production-matching window/view frames**,
   confirming that `NS::View::convertPoint()` (the window-to-view-local
   coordinate conversion done on the host side before the lens center ever
   reaches this shader) plus the AppKit-Y-up-to-texture-V-down flip compose
   correctly — this is a host-side concern, covered fully in Chapter 10,
   but it's the reason `centerU`/`centerV` can be trusted to mean what
   `blitFragment` assumes they mean.
3. **An end-to-end test through the real scene/CPURenderer/Blit.metal
   pipeline**, confirming the lens zooms into actual, recognizable,
   correctly-positioned scene detail — not just a synthetic test pattern.

The point of listing all three is that the shader math in §6 — the
aspect-corrected circular distance test, the de-magnified sample offset —
was checked against a known ground truth at each of three different
levels (shader-only, coordinate-math-only, full pipeline) rather than
written once and trusted by inspection.

## Where this connects

- **Chapter 2** (`Core/RayTraceCore.h`) is what `Raytracer.metal`
  `#include`s verbatim at line 5, and is the reason this file exists in its
  current thin-wrapper form: every ray/hit/scatter/RNG function called from
  `renderKernel`'s sample loop is defined and explained there, not here.
- **Chapter 5** (`GPU/GPURenderer.hpp/.cpp`) is the host-side counterpart
  to both shaders in this chapter: it's what loads `Raytracer.metal`'s
  compiled `.metallib` via `newLibrary(filepath, ...)`, builds the compute
  pipeline state, binds the six buffers this chapter's §3 describes to
  buffer indices 0–5, and issues the 8×8 `dispatchThreads` call this
  chapter only gestures at.
- **Chapter 10** (`App/ImageDisplayView.hpp/.cpp`) is the host-side
  counterpart to `Blit.metal`: it compiles the shader from an in-memory
  string read from `RT_SHADERS_DIR`, builds the render pipeline state,
  defines the host-side twin of `MagnifierUniforms`, and drives the lens
  center via mouse tracking every frame — the piece of the loupe feature
  this chapter deliberately left to that chapter to cover in full.
