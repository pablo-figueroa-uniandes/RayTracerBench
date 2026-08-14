# RayTracerBench — CPU vs GPU (Metal) Ray Tracing Comparison

## Context

The user wants a native macOS application, built in Xcode, that renders the same scene using both a CPU ray tracer and a GPU (Metal compute) ray tracer and compares their performance. It should be based on the algorithm and structure of two reference projects: the "Ray Tracing in One Weekend" book (RayTracing/raytracing.github.io, CC0) and its CUDA port (rogerallen/raytracinginoneweekendincuda, public domain). The user wants the app written in **C++ and Metal**, and has chosen a **pure-C++ architecture** (via Apple's `metal-cpp` + `metal-cpp-extensions`) over a SwiftUI/Objective-C++ approach, keeping Objective-C usage to the absolute minimum AppKit needs (ideally zero hand-written `.mm` files). The project directory is currently empty — this is a greenfield build.

Research into the two reference repos surfaced the key porting constraint that shapes this whole plan: the CUDA port kept C++ virtual dispatch (device-side `new`, real polymorphism) for its `hittable`/`material` hierarchy, but **Metal Shading Language forbids virtual functions, RTTI, and dynamic allocation in kernel code**. So unlike the CUDA port, the GPU (and, for a fair apples-to-apples comparison, the CPU) renderer here must use tagged structs + switch-based dispatch over flat arrays, not the book's polymorphic classes. To make the CPU/GPU comparison legitimate, both renderers will run the same shared algorithm source (a dual-compiled C++/MSL header) over the same scene data, rather than two independently-written implementations that merely produce similar-looking images.

## Architecture

**Pure C++ macOS app** using Apple's official header-only `metal-cpp` (C++ bindings for the Metal API) and `metal-cpp-extensions` (C++ bindings for AppKit/MetalKit/QuartzCore, used in Apple's own "Learn Metal with C++" sample series to build windows/views/widgets without touching Objective-C). GCD (`libdispatch`) is a plain C API, so main-thread marshaling from background render threads needs no Objective-C either. The one acknowledged risk area: `metal-cpp-extensions`' AppKit widget coverage (button target-action, popup buttons, text fields) is less battle-tested than the Metal bindings — Milestone 1 includes an early spike to confirm this works before committing further UI work to it. If a specific widget's callback wiring proves unworkable, the fallback is one small, isolated `.mm` shim for just that widget (documented as Plan B, not the default).

Scene display uses Metal itself rather than `NSImageView`: both the CPU (RGBA8 buffer, uploaded via `replaceRegion`) and GPU (already-a-texture) outputs are shown through two   `ImageDisplayView` — a `CAMetalLayer`-backed view with a tiny textured-quad blit shader.

## Shared core design (the key correctness decision)

- `Core/ShaderTypes.h` — plain structs shared byte-for-byte between C++ and MSL via `<simd/simd.h>` types (`simd_float3`, no hand-rolled `vec3`): `SphereGPU` (center, radius, materialIndex), `MaterialGPU` (enum tag `MAT_LAMBERTIAN/MAT_METAL/MAT_DIELECTRIC`, albedo, fuzz, ir), `CameraGPU` (origin, basis vectors, defocus params), `RenderParams` (width, height, samplesPerPixel, maxDepth, frameSeed).
- `Core/RayTraceCore.h` — **dual-compiled by both `Raytracer.metal` and `CPURenderer.cpp`** (`#include`d verbatim into each): `hitSphere()`, tagged-switch `scatter()`, bounded-loop `rayColor()` (iterative, max depth ~50 — no recursion, matching the CUDA port's GPU-stack workaround and keeping CPU/GPU structurally identical), `getRay()`, and a per-thread `pcgHash`/`randomFloat` RNG (no `curand` equivalent in MSL, so this hash-based generator replaces it on both sides). A one-line macro shim (`#ifdef __METAL_VERSION__`) handles the sole address-space-keyword difference. Fallback if shared compilation proves too fussy: hand-duplicate the four functions with `// KEEP IN SYNC` comments at both ends.
- `Core/Scene.hpp/.cpp` — `buildDefaultScene(seed, width, aspectRatio, spp, maxDepth)` builds the classic RTIOW demo scene (ground sphere + ~22×22 grid of randomized small spheres + 3 feature spheres: glass/lambertian/metal) once, via seeded `std::mt19937`, into a `SceneDescription` (`vector<SphereGPU>`, `vector<MaterialGPU>`, `CameraGPU`, `RenderParams`). Both renderers consume this exact same struct — no separate CPU/GPU scene representations.
- A literal, polymorphic, book-verbatim port (`Classic/`) is an **optional stretch goal only**, explicitly excluded from timed comparisons since it wouldn't run the same code path as the GPU.

## CPU renderer

`CPU/CPURenderer.hpp/.cpp` — pure C++17, no Apple frameworks. Per pixel/sample, calls the shared `getRay()`/`rayColor()`, accumulates, gamma-corrects (sqrt), writes RGBA8. Threading via plain `std::thread` row-partitioning (not GCD, to keep `Core/`+`CPU/` framework-free) with a `CPUThreading::SingleThreaded/MultiThreaded` toggle — **default MultiThreaded** ("realistic" CPU perf), with SingleThreaded exposed as an explicitly labeled alternative so the UI's speedup number is never ambiguous about which CPU baseline it used. Timing via `std::chrono::high_resolution_clock` around the whole render call.

## GPU renderer

`GPU/GPURenderer.hpp/.cpp` — written directly against `metal-cpp` (`MTL::Device`, `MTL::Buffer`, `MTL::ComputePipelineState`, `MTL::Texture`), device/queue/pipeline created once at launch and cached (first-render shader-compile latency must not pollute timed numbers). `Shaders/Raytracer.metal` is a `kernel void renderKernel(...)` taking `RenderParams`/`CameraGPU`/sphere+material buffers via `[[buffer(n)]]` and writing to a `texture2d<float, access::write>` via `[[thread_position_in_grid]]`; dispatched with `dispatchThreads:threadsPerThreadgroup:` (8×8 threadgroups, auto-clamped, no manual grid rounding needed unlike the CUDA port). Buffers use `MTL::ResourceStorageModeShared` (Apple Silicon unified memory, no explicit copy-back). Two timing numbers surfaced: wall-clock (`commit()`/`waitUntilCompleted()`) and GPU-only (`commandBuffer->GPUStartTime()`/`GPUEndTime()`), with GPU-only as the headline comparison metric. A throwaway warm-up dispatch precedes every timed run.

## UI

- `App/AppDelegate.hpp/.cpp` + `main.cpp` — `NS::ApplicationDelegate` bootstrap, creates the main window.
- `App/ControlsPanel.hpp/.cpp` — image-width, samples-per-pixel, max-depth, CPU-threading toggle, scene-seed field/randomize, and `Render CPU` / `Render GPU` / `Compare` buttons (`Compare` runs both against one freshly-built shared `SceneDescription`).
- `App/ImageDisplayView.hpp/.cpp` (×2, CPU and GPU) — textured-quad blit of the render output.
- `App/ResultsPanel.hpp/.cpp` — render time, GPU-only time, estimated rays/sec (`width*height*spp/time`), and a labeled speedup ratio (e.g. "GPU is 8.3× faster than CPU (multi-threaded)").
- `App/AboutAlert.hpp/.cpp` — `NS::Alert` with the attribution note below.
- Renders run on background `std::thread`s; completion marshals back to the main thread via `dispatch_async(dispatch_get_main_queue(), ...)` (plain C API, no ObjC needed).

## Attribution

Both reference repos are public-domain/CC0; no source is copied (the CUDA source isn't even compilable in this project — structural precedent only). Include a short courtesy note in `README.md` and `AboutAlert`: credits "Ray Tracing in One Weekend" (Peter Shirley et al., CC0, raytracing.github.io) and its CUDA port by Roger Allen (public domain), noting neither license requires attribution. Include a MIT license by Pablo Figueroa and Claude as authors

## File tree

```
RayTracerBench.xcodeproj

RayTracerBench/
  main.cpp
  App/
    AppDelegate.hpp / AppDelegate.cpp
    MainWindowController.hpp / MainWindowController.cpp
    ControlsPanel.hpp / ControlsPanel.cpp
    ResultsPanel.hpp / ResultsPanel.cpp
    ImageDisplayView.hpp / ImageDisplayView.cpp
    AboutAlert.hpp / AboutAlert.cpp
  Core/
    ShaderTypes.h
    RayTraceCore.h
    Scene.hpp / Scene.cpp
  CPU/
    CPURenderer.hpp / CPURenderer.cpp
  GPU/
    GPURenderer.hpp / GPURenderer.cpp
  Shaders/
    Raytracer.metal
    Blit.metal
  ThirdParty/
    metal-cpp/                # vendored, Apple official
    metal-cpp-extensions/     # vendored, Apple official
  Resources/
    Assets.xcassets, Info.plist
  Classic/ (optional stretch, book-fidelity virtual-dispatch reference, not used for timing)
  Scenes/  # directory for the scenes. Generate several examples of different scenes, of at least 20 spheres

RayTracerBenchTests/          # second target, plain C++ command-line tool (no XCTest/ObjC)
  main.cpp                    # tiny test runner or vendored single-header framework (e.g. doctest)
  RayTraceCoreTests.cpp       # hitSphere() unit tests
  DeterministicParityTests.cpp # fixed-seed CPU-vs-GPU pixel-diff (headless, no window needed)

README.md
```

## Build order

1. **Milestone 1 — Scaffold + CPU-only viewable image.** Create Xcode project starting from the Command Line Tool (C++) template (not the App template, to avoid Swift/Storyboard scaffolding); manually wire `NSApplication`/window via `metal-cpp-extensions` in `main.cpp`/`AppDelegate`; link Cocoa/Metal/QuartzCore frameworks. **Spike the AppKit widget/button target-action wiring first** to de-risk the pure-C++ UI approach before building further. Add `Core/`, `CPU/CPURenderer`, one button + two `ImageDisplayView`. Exit: pressing the button renders the recognizable RTIOW demo scene.
2. **Milestone 2 — Metal GPU renderer.** Add `Shaders/Raytracer.metal`, resolve any MSL-incompatibility in the shared header, add `GPU/GPURenderer` via metal-cpp, second button + second `ImageDisplayView`. Exit: GPU preview matches CPU preview's scene layout (RNG noise aside); manually verify the deterministic fixed-seed path produces near-identical output.
3. **Milestone 3 — UI wiring + timing.** Build out `ControlsPanel`/`ResultsPanel`, wire `Compare`, implement warm-up dispatch + GPU-only timing + CPU `std::chrono` timing + rays/sec estimate, move renders off the main thread. Exit: `Compare` shows both images and a labeled speedup ratio without freezing the UI.
4. **Milestone 4 — Polish.** `AboutAlert` + `README.md` attribution; `RayTracerBenchTests` target (`RayTraceCoreTests`, `DeterministicParityTests`); optional: bounce-inclusive ray counters, `Classic/` book-fidelity renderer, `MTLCounterSampleBuffer` calibrated GPU timestamps.

## Verification

- **Build:** Xcode GUI (`Product > Run`), or `xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build`; `xcodebuild test -scheme RayTracerBenchTests` (or run the test executable directly, since it's a plain command-line tool) for the parity/unit tests.
- **Correctness, two tiers:** (1) eyeball — same shared scene seed, both renderers should show the same sphere layout/materials/colors, with expected per-pixel noise differences since CPU/GPU RNG *streams* differ even though the RNG *function* is shared; (2) deterministic parity — small fixed scene, fixed hash-seed formula, run on both CPU and GPU, diff RGBA buffers directly (tolerance ~2/255 for FP execution-order differences) — a direct benefit of sharing `RayTraceCore.h` verbatim.
- **Benchmark validity:** one throwaway GPU warm-up dispatch before timing; run each config 3× and report min alongside the raw number; expose multiple spp presets (10/50/100/500) so time scaling with sample count is visible; note in the About/README that at small workloads GPU dispatch/readback overhead can dominate and CPU may look competitive — an expected artifact, not a bug.
