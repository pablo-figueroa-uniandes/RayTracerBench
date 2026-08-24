# Chapter 4: The CPU Renderer

**Abstract.** This chapter covers `CPU/CPURenderer.hpp` and `CPU/CPURenderer.cpp` —
the plain-C++ half of RayTracerBench's dual-compile design. Chapter 2 showed
`Core/RayTraceCore.h` written so it could be compiled twice; Chapter 3 showed
one of those compiles, `Shaders/Raytracer.metal`, feeding it to the Metal
compiler as a GPU kernel. This chapter is the other compile: the same header,
`#include`d verbatim into an ordinary `.cpp` file, driven by a hand-rolled
`std::thread` row-partitioning scheme instead of a GPU dispatch grid. Nothing
in this file re-implements ray/sphere/pyramid intersection, material
scattering, or path-trace bouncing — all of that already exists in Chapter 2's
header. What's here is: turning a `SceneDescription` into a byte buffer,
deciding how to split the work across CPU cores, and timing the result.

**Files covered:** `RayTracerBench/CPU/CPURenderer.hpp` (28 lines),
`RayTracerBench/CPU/CPURenderer.cpp` (120 lines).

---

## §1. The other half of the dual compile

`Shaders/Raytracer.metal` opens with two `#include`s: `ShaderTypes.h` and
`RayTraceCore.h`, compiled as Metal Shading Language by `xcrun -sdk macosx
metal` (Chapter 3, Chapter 12). `CPURenderer.cpp` opens the same way, compiled
instead by whatever C++17 compiler Xcode invokes for the rest of the app:

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:1-6
#include "CPURenderer.hpp"

#include "../Core/RayTraceCore.h"

#include <algorithm>
#include <thread>
```

This single `#include "../Core/RayTraceCore.h"` line is the whole point of
Chapter 2's design. `RayTraceCore.h` doesn't have a CPU version and a GPU
version — it has one version, guarded by exactly one `#ifdef
__METAL_VERSION__` (the `RT_DEVICE` address-space shim), and each of the two
translation units that pull it in supplies the other half of the environment:
`Raytracer.metal` supplies `#include <metal_stdlib>` and MSL's `float3`/`sqrt`
built-ins; `CPURenderer.cpp` supplies `<simd/simd.h>` (via `ShaderTypes.h`)
and `<cmath>`'s `std::sqrt`/`std::pow`/`std::fabs`/`std::fmin`, pulled into
scope by `RayTraceCore.h`'s own `using std::fabs; using std::fmin; using
std::pow; using std::sqrt;` in its non-Metal branch. Neither file duplicates
`hitSphere()`, `hitPyramid()`, `hitEntity()`, `scatter()`, or `rayColor()` —
they call the identical inline functions, compiled twice by two different
compilers. `CPURenderer.cpp` is framework-free pure C++17 apart from that:
no AppKit, no CoreGraphics, no Metal headers — it doesn't even know a GPU
exists in the process. That's deliberate; per `CLAUDE.md`'s architecture
notes, the CPU renderer is kept framework-free so it stays a fair, unadorned
CPU baseline rather than something the GPU-side plumbing could bias.

## §2. The public interface: what `renderCPU()` takes and returns

Everything a caller needs is declared in the 28-line header. First, the
threading mode is a two-value enum, not a boolean or a thread-count
parameter:

```cpp
// RayTracerBench/CPU/CPURenderer.hpp:9-16
namespace CPUThreading
{
	enum Mode
	{
		SingleThreaded,
		MultiThreaded,
	};
}
```

Then the result type — a pixel buffer plus a duration, nothing else:

```cpp
// RayTracerBench/CPU/CPURenderer.hpp:18-22
struct CPURenderResult
{
	std::vector<uint8_t>                       pixels; // RGBA8, row-major, row 0 = top
	std::chrono::duration<double, std::milli>  renderTime;
};
```

And the function itself:

```cpp
// RayTracerBench/CPU/CPURenderer.hpp:24-28
// Renders `scene` on the CPU by calling the exact same Core/RayTraceCore.h functions the GPU
// renderer will later call from its kernel. Default is MultiThreaded (realistic CPU perf);
// SingleThreaded is exposed explicitly so a UI-facing speedup number is never ambiguous about
// which CPU baseline it was measured against.
CPURenderResult renderCPU( const SceneDescription& scene, CPUThreading::Mode mode = CPUThreading::MultiThreaded );
```

The signature is narrow on purpose: one `const SceneDescription&` in (the
same struct Chapter 1 showed `buildDefaultScene()` producing, with no
CPU-specific scene representation), one `CPURenderResult` out. There's no
callback, no output parameter, no reference to an `ImageDisplayView` or an
`NS::Alert` anywhere in this file — `renderCPU()` doesn't know or care who's
calling it. That's what lets three different call sites in later chapters
reuse it unchanged: `ControlsPanel`/`AppDelegate`'s `Render CPU` and
`Compare` buttons (Chapter 8/9) call it on a background `std::thread` and
marshal `pixels`/`renderTime` back to the UI via `dispatch_async`; and
`Export/SceneExporter.hpp`'s preview-PNG path (Chapter 7) calls the exact
same function on the exact same `SceneDescription` it just exported to
glTF/OBJ, feeding `result.pixels` straight into `ImageWriter`'s `writePNG()`
with no adapter code in between. The `pixels`/`renderTime` split also mirrors
what the GPU renderer's public interface returns (Chapter 5), which is what
lets `ResultsPanel` compute a speedup ratio without special-casing either
side.

## §3. Why the threading toggle is explicit, not automatic

It would be simpler for `renderCPU()` to always use
`std::thread::hardware_concurrency()` threads and drop the `mode` parameter
entirely. The header comment states directly why it doesn't: **"Default is
MultiThreaded (realistic CPU perf); SingleThreaded is exposed explicitly so a
UI-facing speedup number is never ambiguous about which CPU baseline it was
measured against"** (`CPURenderer.hpp:26-27`). A benchmark's headline number
is a ratio — GPU time divided by CPU time — and that ratio means something
different depending on whether the CPU side used one core or all of them. A
tool that silently always parallelizes the CPU path can still report an
honest number, but it can never report the *other* number (single-core vs.
GPU) without a code change, and a reader of the UI has no way to tell which
one they're looking at. `CLAUDE.md`'s CPU-renderer notes make the same point:
this toggle is surfaced in `ControlsPanel` precisely so the comparison is
never ambiguous about its own baseline. `SingleThreaded` isn't a debug
leftover or a fallback for some platform limitation — it's a first-class,
user-selectable mode whose entire reason to exist is to make the speedup
number legible.

## §4. Row-partitioning across `std::thread`s

The project note in `CLAUDE.md`'s CPU-renderer section is explicit that this
threads via **plain `std::thread` row-partitioning — not GCD** — the same
distinction drawn for why UI-completion marshaling uses `dispatch_async` while
the actual render work doesn't. `renderCPU()` is the function that makes that
choice concrete:

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:77-120
CPURenderResult renderCPU( const SceneDescription& scene, CPUThreading::Mode mode )
{
	const uint32_t width = scene.params.width;
	const uint32_t height = scene.params.height;

	CPURenderResult result;
	result.pixels.resize( static_cast<size_t>( width ) * height * 4 );

	const auto startTime = std::chrono::high_resolution_clock::now();

	if ( mode == CPUThreading::SingleThreaded || height == 0 )
	{
		renderRows( scene, result.pixels, 0, height );
	}
	else
	{
		unsigned threadCount = std::thread::hardware_concurrency();
		if ( threadCount == 0 )
			threadCount = 4;
		threadCount = std::min<unsigned>( threadCount, height );

		std::vector<std::thread> threads;
		threads.reserve( threadCount );

		const uint32_t rowsPerThread = ( height + threadCount - 1 ) / threadCount;

		for ( unsigned t = 0; t < threadCount; ++t )
		{
			const uint32_t rowStart = t * rowsPerThread;
			const uint32_t rowEnd = std::min( rowStart + rowsPerThread, height );
			if ( rowStart >= rowEnd )
				continue;
			threads.emplace_back( renderRows, std::cref( scene ), std::ref( result.pixels ), rowStart, rowEnd );
		}

		for ( std::thread& t : threads )
			t.join();
	}

	const auto endTime = std::chrono::high_resolution_clock::now();
	result.renderTime = endTime - startTime;

	return result;
}
```

The shape of this is ordinary embarrassingly-parallel image work, but a few
details are worth calling out because they're the kind of edge case that only
shows up once: `height == 0` is folded into the `SingleThreaded` branch
(`renderRows(scene, result.pixels, 0, 0)` is a no-op loop, cheaper than
standing up a thread pool for zero rows) rather than being a separate guard.
`hardware_concurrency()` can return `0` on a platform that can't determine
the figure, so a `4`-thread fallback keeps the multithreaded path from
degenerating into zero threads and zero work. `threadCount` is then clamped
to `height` (`std::min<unsigned>(threadCount, height)`) so a tiny image —
say, a 3-row preview — never spins up more threads than there are rows to
give them; every thread is guaranteed at least one row. `rowsPerThread` uses
the `(height + threadCount - 1) / threadCount` ceiling-division idiom, so the
last thread's `rowEnd` is clamped with `std::min(rowStart + rowsPerThread,
height)` and can legitimately end up with a shorter band than the others (or,
via the `if (rowStart >= rowEnd) continue;` guard, no rows at all, though the
prior clamp against `height` makes that path unreachable in practice — it's
defensive against future changes to the partitioning arithmetic). Timing
wraps the whole dispatch-and-join block with `std::chrono::high_resolution_clock`,
so `renderTime` measures wall-clock time for the full render including thread
startup/teardown, not just the inner ray-tracing loop — the fair, literal
answer to "how long did this take."

Each worker thread is handed the scene by `std::cref` and the shared pixel
buffer by `std::ref`, both passed into `std::thread`'s constructor alongside
the free function `renderRows` and that thread's own `[rowStart, rowEnd)`
band. Because every thread writes only its own disjoint row range of
`result.pixels`, there's no shared mutable state to synchronize and no lock
anywhere in this file — the partitioning itself is the synchronization
strategy.

## §5. `renderRows()`: walking `transforms[]`/`shapes[]` in lockstep

The actual per-pixel work — the part that both the `SingleThreaded` and
`MultiThreaded` paths funnel into — lives in an anonymous-namespace helper,
`renderRows()`, which renders one contiguous band of rows into the shared
`pixels` buffer:

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:19-32
void renderRows( const SceneDescription& scene, std::vector<uint8_t>& pixels, uint32_t rowStart, uint32_t rowEnd )
{
	const uint32_t width = scene.params.width;
	const uint32_t height = scene.params.height;
	const uint32_t samplesPerPixel = scene.params.samplesPerPixel;
	const uint32_t maxDepth = scene.params.maxDepth;

	const TransformGPU* transforms = scene.transforms.data();
	const ShapeGPU*     shapes = scene.shapes.data();
	const uint32_t      entityCount = static_cast<uint32_t>( scene.transforms.size() );
	const MaterialGPU*  materials = scene.materials.data();
```

This is exactly Chapter 1's `SceneDescription`: `transforms`, `shapes`, and
`materials` are the three parallel ECS component arrays, and `.data()` here
is the plain-pointer side of the same `RT_DEVICE` design Chapter 2 described
— on the CPU these are ordinary `const TransformGPU*`/`const ShapeGPU*`/`const
MaterialGPU*`, with no address-space annotation needed at all, whereas
`Raytracer.metal`'s kernel arguments carry `device` because they arrive
through a Metal buffer binding (`Shaders/Raytracer.metal:13-16`). Nothing
else about the arrays differs. `entityCount` is just `scene.transforms.size()`
cast down to `uint32_t` — there is no separate sphere-count/pyramid-count
bookkeeping to keep straight, because spheres and pyramids already live
interleaved in the same two arrays, distinguished only by `ShapeGPU::type`.

The body of the row/column loop is where those arrays actually get used, and
it reads almost line-for-line like `renderKernel`'s per-thread body in
`Raytracer.metal:25-52` — same seed derivation, same jittered-sample loop,
same call into `getRay()` and `rayColor()`, just iterated explicitly over `i`
and `j` instead of arriving as a `[[thread_position_in_grid]]`:

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:33-71
for ( uint32_t j = rowStart; j < rowEnd; ++j )
{
	for ( uint32_t i = 0; i < width; ++i )
	{
		// Distinct per-pixel seed derivation; only randomFloat()/pcgHash() themselves need
		// to match the GPU renderer bit-for-bit, not this seeding formula (see CLAUDE.md's
		// "eyeball correctness" note — per-pixel noise legitimately differs CPU vs GPU).
		uint32_t seed = pcgHash( scene.params.frameSeed ^ ( j * 9781u + i * 6271u + 1u ) );

		simd_float3 colorSum = makeFloat3( 0.0f, 0.0f, 0.0f );

		for ( uint32_t s = 0; s < samplesPerPixel; ++s )
		{
			RandomFloatSample ru = randomFloat( seed );
			seed = ru.seed;
			RandomFloatSample rv = randomFloat( seed );
			seed = rv.seed;

			float u = ( static_cast<float>( i ) + ru.value ) / static_cast<float>( width - 1 );
			float v = ( static_cast<float>( height - 1 - j ) + rv.value ) / static_cast<float>( height - 1 );

			CameraRaySample cameraRay = getRay( scene.camera, u, v, seed );
			seed = cameraRay.rngSeed;

			RayColorResult sample = rayColor( cameraRay.ray, transforms, shapes, entityCount, materials, maxDepth, seed );
			seed = sample.rngSeed;

			colorSum = colorSum + sample.color;
		}

		simd_float3 averageColor = colorSum / static_cast<float>( samplesPerPixel );

		const size_t pixelIndex = ( static_cast<size_t>( j ) * width + i ) * 4;
		pixels[ pixelIndex + 0 ] = toByte( averageColor.x );
		pixels[ pixelIndex + 1 ] = toByte( averageColor.y );
		pixels[ pixelIndex + 2 ] = toByte( averageColor.z );
		pixels[ pixelIndex + 3 ] = 255;
	}
}
```

`rayColor(cameraRay.ray, transforms, shapes, entityCount, materials, maxDepth,
seed)` on line 57 is the entire "collision system plus shading" pipeline
Chapter 2 documented in one call: internally, `rayColor()` (`Core/RayTraceCore.h:467-513`)
loops `i` from `0` to `entityCount`, calling `hitEntity(transforms[i],
shapes[i], r, 0.001f, closestSoFar)` per entity to find the closest hit — the
tagged dispatch to `hitSphere()` or `hitPyramid()` that stands in for a
forbidden virtual `Hittable::hit()` call — and then hands the winning hit
record to `scatter()` for tagged-switch material shading, bouncing up to
`maxDepth` times. `renderRows()` itself never calls `hitEntity()` or
`scatter()` directly, and doesn't need to know how many shape types exist or
how materials are resolved; it only walks pixels and samples, and defers the
entire "what does this ray hit and what color does it come back as" question
to Chapter 2's shared function. That's the payoff of Chapter 2's design
appearing concretely here: adding a third primitive type someday would mean
touching `RayTraceCore.h`'s `hitEntity()` switch once, not this file.

The per-pixel RNG seed on line 40 is derived from `scene.params.frameSeed`
mixed with the pixel coordinates via `pcgHash()` — but the exact mixing
formula (`j * 9781u + i * 6271u + 1u`) is local to this loop, not part of the
shared core, and the comment says so directly: only `randomFloat()`/
`pcgHash()` themselves need to match the GPU renderer bit-for-bit; this
seeding arithmetic doesn't, which is exactly why CPU and GPU renders of the
same scene produce visibly different per-pixel noise while still agreeing on
geometry, materials, and overall lighting — `CLAUDE.md`'s "eyeball
correctness" verification note this comment points at.

## §6. Byte conversion: `toByte()`

The last piece is the free function that turns a linear, possibly-out-of-range
float color channel into a displayable `uint8_t`:

```cpp
// RayTracerBench/CPU/CPURenderer.cpp:10-17
// Gamma-2 correction (sqrt) plus the book's [0,0.999] clamp before quantizing to a byte, so a
// component of exactly 1.0 rounds down to 255 rather than overflowing to 0.
uint8_t toByte( float c )
{
	float gammaCorrected = std::sqrt( std::max( 0.0f, c ) );
	float clamped = std::min( gammaCorrected, 0.999f );
	return static_cast<uint8_t>( 256.0f * clamped );
}
```

This is the CPU-side twin of the last three lines of `renderKernel()` in
`Raytracer.metal:49-52` (`sqrt`/`max`/`min`/`clamped` there operate on a whole
`simd_float3` at once via MSL's vector overloads and write straight into a
`texture2d`, whereas `toByte()` here operates one channel at a time and
writes into a plain `RGBA8` byte array) — same gamma-2 approximation
(`sqrt`, standing in for the more expensive `pow(c, 1/2.2)`), same
`[0, 0.999]` clamp before quantizing so an exact `1.0` input rounds down to
`255` instead of wrapping to `0` at `256.0f * 1.0f`. `toByte()` is the one
place in this file where the CPU path's `RGBA8` output format and the GPU
path's `MTL::Texture` output format visibly diverge — not because the
tone-mapping math differs, but because `ImageDisplayView` (Chapter 10)
consumes the two renderers' results through different upload paths
(`replaceRegion` for the CPU buffer, direct texture write for the GPU
kernel), and each renderer produces its native format for its own path
rather than either one converting to match the other.

## §7. What isn't here

It's worth noting what `CPURenderer.cpp` deliberately does *not* do, since
the omissions are as much a design statement as the code that's present.
There's no threading library beyond `<thread>` itself — no thread pool, no
work queue, no GCD dispatch — because the workload here is a single,
one-shot, statically-known partition (N rows into T threads, joined once),
which doesn't need a general-purpose pool. There's no scene construction —
`buildDefaultScene()` (Chapter 1) already ran by the time a
`SceneDescription` reaches `renderCPU()`. And there's no UI awareness at all:
no knowledge of `ControlsPanel`'s settings fields, no `NS::Alert`, no file
I/O. That narrowness is what makes the same function usable, unmodified, from
three different call sites across the rest of the app.

## Where this connects

- **Chapter 2 (The Shared Ray-Tracing Core)** is the header this file
  compiles as its second target — every intersection, scatter, and RNG
  function `renderRows()` calls (`hitEntity()`, `scatter()`, `rayColor()`,
  `getRay()`, `pcgHash()`/`randomFloat()`) is defined there once and shared
  verbatim with the GPU kernel, not reimplemented here.
- **Chapter 1 (The Core Data Model)** defines the `SceneDescription` this
  file only ever reads from (`scene.transforms`, `scene.shapes`,
  `scene.materials`, `scene.camera`, `scene.params`) — `CPURenderer.cpp`
  never constructs or mutates a scene, only consumes one.
- **Chapter 3 (The Metal Shaders)** is the GPU side of the same dual-compile
  design: `Raytracer.metal`'s `renderKernel()` and this file's `renderRows()`
  run the same per-pixel algorithm (seed derivation aside) over the same
  `RayTraceCore.h`, just dispatched across a Metal thread grid instead of
  `std::thread` row bands.
- **Chapter 8 (The App Shell and Lifecycle)** shows `AppDelegate` calling
  `renderCPU()` on a background `std::thread` for the `Render CPU` and
  `Compare` actions, then marshaling `CPURenderResult` back to the main
  thread via `dispatch_async` to update `ImageDisplayView`/`ResultsPanel`.
- **Chapter 7 (Scene Export)** shows `Export/SceneExporter.hpp`'s
  preview-PNG path calling this exact same `renderCPU()` against the scene
  it just exported to glTF/OBJ, then handing `result.pixels` to
  `Export/ImageWriter.hpp`'s `writePNG()` — the second, independent call site
  that this file's narrow, UI-agnostic interface makes possible without any
  export-specific code inside `CPURenderer.cpp` itself.
