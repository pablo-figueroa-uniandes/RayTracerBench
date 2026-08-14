# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Milestone 1's AppKit spike is done: `RayTracerBench.xcodeproj` builds and runs a plain `NS::Window` + `NS::Button` wired end-to-end in pure C++ (see `RayTracerBench/main.cpp`, `RayTracerBench/App/AppDelegate.{hpp,cpp}`). `Core/`, `CPU/`, `GPU/`, and the rest of the UI described below do not exist yet — `based-on-the-examples-streamed-ripple.md` remains the authoritative spec for that work. Keep this file in sync as more of the file tree lands.

`RayTracerBench.xcodeproj` is generated via CMake's Xcode generator (`cmake -S . -B <scratch-dir> -G Xcode`, then copy the resulting `.xcodeproj` to the repo root) rather than Xcode's GUI project templates, since project creation here happens headlessly. `CMakeLists.txt` is the source of truth for target/framework/include-path setup — regenerate the `.xcodeproj` from it after changing sources rather than hand-editing the project file, and re-copy over the tracked copy.

`ThirdParty/metal-cpp` and `ThirdParty/metal-cpp-extensions` are vendored from Apple's `LearnMetalCPP.zip` sample bundle (not the standalone `metal-cpp` zip) to keep the base library and AppKit/MetalKit extensions from Apple's own release pairing in sync. Apple's `metal-cpp-extensions` has no `NS::Control`/`NS::Button` wrapper — this is a confirmed, permanent gap (an Apple DTS engineer said so on the developer forums), not an oversight to work around differently. `ThirdParty/metal-cpp-extensions/AppKit/NSControl.hpp` and `NSButton.hpp` are project-local additions (clearly marked as such in their headers) that fill it using the exact same `Object::sendMessage`/`class_addMethod` idiom Apple's own headers use — extend that pattern for any other missing AppKit control rather than reaching for an `.mm` shim first.

## What this project is

RayTracerBench: a native macOS app (Xcode) that renders the same scene with both a CPU ray tracer and a GPU (Metal compute) ray tracer and compares their performance. It's based on the algorithm/structure of "Ray Tracing in One Weekend" (CC0) and its CUDA port by Roger Allen (public domain) — structural precedent only, no copied source.

Written in **pure C++ and Metal** via Apple's header-only `metal-cpp` + `metal-cpp-extensions` bindings, deliberately avoiding SwiftUI/Objective-C++. The goal is zero hand-written `.mm` files; if a specific AppKit widget's callback wiring proves unworkable through `metal-cpp-extensions`, the documented fallback (Plan B, not default) is one small isolated `.mm` shim for just that widget.

## Build and test commands

- Build: Xcode GUI (`Product > Run`), or `xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build`
- Test: `xcodebuild test -scheme RayTracerBenchTests` (or run the test executable directly — `RayTracerBenchTests` is a plain command-line C++ tool, not an XCTest/ObjC target)

## Architecture

### The core correctness constraint

Metal Shading Language forbids virtual functions, RTTI, and dynamic allocation in kernel code. Unlike the CUDA port (which keeps C++ virtual dispatch for `hittable`/`material`), both the GPU **and** CPU renderers here use tagged structs + switch-based dispatch over flat arrays — the same style — so the CPU/GPU comparison is apples-to-apples. This is achieved by having both renderers execute the *same shared algorithm source*, not two independently-written implementations that merely look similar.

### Shared core (`Core/`)

- `ShaderTypes.h` — plain structs shared byte-for-byte between C++ and MSL via `<simd/simd.h>` types (no hand-rolled vec3): `SphereGPU`, `MaterialGPU` (tagged enum: `MAT_LAMBERTIAN`/`MAT_METAL`/`MAT_DIELECTRIC`), `CameraGPU`, `RenderParams`.
- `RayTraceCore.h` — **dual-compiled**: `#include`d verbatim by both `Shaders/Raytracer.metal` and `CPU/CPURenderer.cpp`. Contains `hitSphere()`, tagged-switch `scatter()`, bounded-loop `rayColor()` (iterative, max depth ~50 — no recursion, so CPU and GPU stay structurally identical), `getRay()`, and a per-thread `pcgHash`/`randomFloat` RNG (replaces `curand`, used identically on both sides). A single `#ifdef __METAL_VERSION__` macro shim handles the one address-space-keyword difference between C++ and MSL. If shared compilation proves too fussy, the fallback is hand-duplicating the four functions with `// KEEP IN SYNC` comments at both ends — treat any divergence between the CPU and GPU copies as a bug.
- `Scene.hpp/.cpp` — `buildDefaultScene(seed, width, aspectRatio, spp, maxDepth)` builds the classic RTIOW demo scene once (via seeded `std::mt19937`) into a single `SceneDescription` that both renderers consume — there is no separate CPU/GPU scene representation.
- `Classic/` is an optional stretch goal only: a literal, polymorphic, book-verbatim port, explicitly excluded from timed comparisons since it doesn't share the tagged-struct code path with the GPU renderer.

### CPU renderer (`CPU/`)

Pure C++17, no Apple frameworks (kept framework-free deliberately). Threads via plain `std::thread` row-partitioning — not GCD — with a `CPUThreading::SingleThreaded/MultiThreaded` toggle. Default is MultiThreaded (realistic CPU perf); SingleThreaded is exposed explicitly so the UI's speedup number is never ambiguous about which CPU baseline it used.

### GPU renderer (`GPU/`)

Written directly against `metal-cpp` (`MTL::Device`, `MTL::Buffer`, `MTL::ComputePipelineState`, `MTL::Texture`). Device/queue/pipeline are created once at launch and cached, so first-render shader-compile latency never pollutes timed numbers. `Shaders/Raytracer.metal` is a `kernel void renderKernel(...)` dispatched with `dispatchThreads:threadsPerThreadgroup:` (8×8 threadgroups). Buffers use `MTL::ResourceStorageModeShared` (Apple Silicon unified memory — no explicit copy-back). Two timing numbers are surfaced: wall-clock and GPU-only (`GPUStartTime()`/`GPUEndTime()`), with GPU-only as the headline metric. A throwaway warm-up dispatch always precedes a timed run.

### UI (`App/`)

- `AppDelegate` + `main.cpp` bootstrap an `NS::ApplicationDelegate` and the main window — no Swift/Storyboard.
- Scene display uses Metal itself, not `NSImageView`: `ImageDisplayView` is a `CAMetalLayer`-backed view with a textured-quad blit shader (`Shaders/Blit.metal`), used once for the CPU output (RGBA8 buffer uploaded via `replaceRegion`) and once for the GPU output (already a texture).
- `ControlsPanel` exposes image-width, samples-per-pixel, max-depth, CPU-threading toggle, scene-seed field/randomize, and `Render CPU` / `Render GPU` / `Compare` buttons. `Compare` runs both renderers against one freshly-built shared `SceneDescription`.
- `ResultsPanel` shows render time, GPU-only time, estimated rays/sec (`width*height*spp/time`), and a labeled speedup ratio.
- Renders run on background `std::thread`s; completion marshals back to the main thread via `dispatch_async(dispatch_get_main_queue(), ...)` — GCD's plain C API, so no Objective-C is needed even for this.

## Verification approach

- **Eyeball correctness**: same shared scene seed on both renderers should produce the same sphere layout/materials/colors; per-pixel noise differences are expected since CPU/GPU RNG *streams* differ even though the RNG *function* is shared.
- **Deterministic parity**: a small fixed scene with a fixed hash-seed, run on both CPU and GPU, diffed directly (tolerance ~2/255 for floating-point execution-order differences) — this is only meaningful because `RayTraceCore.h` is shared verbatim. Covered by `RayTracerBenchTests/DeterministicParityTests.cpp`.
- **Benchmark validity**: always warm up the GPU once before timing; run each config 3× and report the min alongside the raw number; expose multiple spp presets (10/50/100/500). At small workloads GPU dispatch/readback overhead can dominate and CPU may look competitive — this is an expected artifact of the benchmark, not a bug, and should be noted rather than "fixed."

## Attribution requirements

Both reference repos (RayTracing/raytracing.github.io, rogerallen/raytracinginoneweekendincuda) are CC0/public-domain and don't require attribution, but a short courtesy note should still appear in `README.md` and `App/AboutAlert`, crediting both, plus an MIT license listing Pablo Figueroa and Claude as authors.
