# Session Summary — 2026-08-14

What started as a single planning document (`based-on-the-examples-streamed-ripple.md`) in an
otherwise empty repository became a fully working, tested, GitHub-hosted macOS app in one session:
**RayTracerBench**, a CPU-vs-GPU (Metal compute) ray tracing benchmark written in pure C++ and
Metal, with zero hand-written Objective-C.

Repository: **https://github.com/pablo-figueroa-uniandes/RayTracerBench**

## What got built, in order

1. **`/init`** — wrote the initial `CLAUDE.md` from the planning doc, since no code existed yet.
2. **Milestone 1 spike** — a plain `NS::Window` + `NS::Button`, wired end-to-end in pure C++ via
   Apple's `metal-cpp`/`metal-cpp-extensions` bindings. This was the project's riskiest assumption
   (can AppKit widget callbacks really be wired without Objective-C?), spiked first and confirmed
   working before anything else was built.
3. **`Core/`** — `ShaderTypes.h` (shared structs), `RayTraceCore.h` (the ray tracing algorithm,
   dual-compiled verbatim into both the CPU renderer and the GPU shader), `Scene.hpp/.cpp` (the
   classic "Ray Tracing in One Weekend" demo scene).
4. **`CPU/CPURenderer`** — multi-threaded CPU path tracer calling into the shared core.
5. **UI wiring** — `ImageDisplayView` (a `CAMetalLayer`-backed view with its own blit shader,
   deliberately not `NSImageView`) so the button's render actually appears on screen.
6. **Milestone 2 — GPU renderer** — `Shaders/Raytracer.metal` + `GPU/GPURenderer`, a real Metal
   compute kernel running the *same* shared `RayTraceCore.h`, not a separate reimplementation.
7. **Milestone 3** — `ControlsPanel` (width/spp/depth/seed fields, CPU-threading toggle, Render
   CPU / Render GPU / Compare buttons), `ResultsPanel` (times, rays/sec, speedup ratio), and moving
   every render off the main thread (`std::thread` + `dispatch_async` back to the main queue).
8. **Milestone 4** — an About panel with courtesy attribution, `README.md`, an MIT `LICENSE`, and a
   real `RayTracerBenchTests` command-line test target (unit tests for `hitSphere()`, plus a
   deterministic CPU/GPU pixel-parity test).
9. **`Docs/RayTracerBench-Theory-and-Code.pdf`** — a 17-page document explaining both the ray
   tracing theory (camera/ray model, intersection math, materials, Monte Carlo sampling, gamma
   correction, the RNG) and this codebase's specific implementation of it, with real code excerpts.
10. **Magnifying-glass loupe** — hovering either preview zooms the *same* region in both the CPU
    and GPU images simultaneously, so fine detail is directly comparable between the two renderers.
11. **Build-location fix** — the checked-in Xcode project originally built to a session-specific
    `/tmp` path (an artifact of how the project was generated); fixed to build to a stable,
    gitignored `build/` directory inside the repo instead.
12. **Pushed to GitHub** — public repo, full history, `main` branch tracked to `origin`.
13. **"Floating?" checkbox** — a real `NSButtonTypeSwitch` checkbox in `ControlsPanel` (AppKit
    manages its own checked state, so no click handler is needed). When checked,
    `Scene::buildDefaultScene()`'s new `floating` parameter randomizes the height of the small
    randomized-field spheres up to a rendered-and-eyeballed `kMaxFloatHeight`; the three large
    feature spheres (glass/lambertian/metal) always stay grounded, per explicit follow-up feedback
    after the first pass floated everything.
14. **Square pyramids + an ECS geometry refactor** — the scene's geometry moved from a
    sphere-only array to an ECS-style layout: `ShaderTypes.h` gained a `TransformGPU` (position +
    orthonormal orientation basis) and `ShapeGPU` (tagged sphere/pyramid dimensions + a
    material-index component reference) as separate component types, and `SceneDescription` stores
    them as parallel `transforms`/`shapes` arrays indexed by a plain entity index. `RayTraceCore.h`
    gained `hitEntity()`, a tagged-switch "collision system" dispatching to `hitSphere()` or the new
    `hitPyramid()`/`hitPyramidLocal()` — a square pyramid represented as 5 half-spaces (1 base + 4
    triangular sides) solved via the Kay-Kajiya slab method, with closed-form plane equations
    derived from the pyramid's own symmetry. Five pyramids were added to the demo scene at varied
    yaw/tilt orientations, each placed exactly flush on the ground regardless of orientation via a
    closed-form "lowest vertex" computation — no eyeballing needed, unlike `kMaxFloatHeight` above.
    Pyramids are lambertian/metal only (never dielectric), since the slab test doesn't resolve a ray
    originating inside the solid, which glass refraction would require. `RayTracerBenchTests` grew
    six new pyramid/entity-dispatch tests (15 total, all passing), and the existing CPU/GPU parity
    test kept passing unmodified — confirming the ECS restructuring changed neither renderer's
    actual output.
15. **Scene export (glTF / OBJ+MTL)** — new "Save glTF"/"Save OBJ" buttons in `ControlsPanel`
    export the *current scene's geometry* (spheres tessellated into a UV mesh, pyramids exact and
    faceted) to `<the running executable's own directory>/SavedScenes/`, via a new `Export/`
    module reusing the ECS Transform+Shape components as a third "system" (`buildEntityMesh()`)
    alongside `hitEntity()` and `scatter()`. Filenames encode seed/width/Floating? plus a
    timestamp — deliberately not samples-per-pixel/max-depth, which don't affect the exported
    geometry. `executableDirectory()` resolves the real binary location via `_NSGetExecutablePath`
    rather than `argv[0]`/`getcwd()`. Dielectric materials are approximated the same way in both
    formats (clear/glossy/partly-transparent), since neither format's core spec models true glass.
16. **Same-named preview PNG alongside every 3D export** — each glTF/OBJ save now also renders
    the scene at the current settings (via the existing CPU renderer) and writes it as a real PNG,
    same stem, same `SavedScenes/` directory, via a new `Export/ImageWriter.hpp/.cpp` wrapping
    CoreGraphics/ImageIO's plain C APIs (no vendored image library, no Objective-C). Adding a full
    render meant `saveScene()` could no longer stay synchronous on the main thread as originally
    designed — it now runs on a background thread like the other render actions, with the result
    alert marshaled back via `dispatch_async`.

## Notable technical decisions

- **One algorithm, two compilers.** `RayTraceCore.h` is `#include`d verbatim by both
  `CPU/CPURenderer.cpp` and `Shaders/Raytracer.metal`, so the CPU/GPU comparison is apples-to-apples
  rather than two implementations that merely look similar. Every function passes/returns plain
  values instead of out-parameters, so only *one* MSL-specific address-space keyword (`device`) is
  ever needed, on exactly two parameters. Validated directly against the real Metal shader compiler
  before any renderer code was built on top of it — compiled clean, zero warnings, on the first try.
- **Filling real gaps in Apple's own bindings.** `metal-cpp-extensions` turned out to be missing
  `NS::Button`, `NS::Control`, `NS::TextField`, `NS::Alert`, `CA::MetalLayer`, and `NS::Event`
  entirely — each confirmed missing (in one case, an Apple DTS engineer's own forum post confirming
  it's a permanent gap) rather than assumed, and each filled the same way Apple's own headers do it
  internally (thin C++ wrappers over `objc_msgSend`). One of these was an actual bug in Apple's
  vendored code (`NS::View::init()` sent a message to the wrong object) — caught by reproducing the
  crash first, not just inferred from reading the header.
- **Winding orders, verified not assumed.** The scene exporter's sphere-mesh triangle winding was
  hand-derived and looked plausible, but a standalone check (does each face's cross-product normal
  point away from the sphere's center?) found it was actually backwards — 174 of 192 triangles
  faced inward. Fixed, then re-verified programmatically (kept as a permanent test in
  `EntityMeshTests.cpp`) rather than trusted by inspection a second time.
- **The CPU/GPU parity investigation.** A strict per-pixel tolerance didn't hold at low sample
  counts (~9% of channels differed by more than expected). Investigated rather than dismissed:
  differences were exactly zero on every pixel that didn't hit geometry, and shrank as sample count
  rose — the signature of chaotic Monte Carlo branch-divergence from sub-ULP CPU/GPU floating-point
  differences, not a bug. The committed test asserts on mismatch rate at a high sample count instead
  of a strict bound at a low one.
- **Empirical bound over untested math.** For the Floating? feature's max height, an exact
  per-object analytical bound was derived by hand from the camera's `u`/`v`/`w` basis vectors, but a
  hand-checked numeric example didn't verify cleanly — so it was abandoned in favor of a simpler
  global constant, chosen by actually rendering candidate heights and confirming by eye that nothing
  left the frame, consistent with this session's preference throughout for verifying against a real
  render rather than trusting derived formulas.
- **Verification without a screen.** This session ran with no display/accessibility access (no
  `screencapture`, no `osascript` automation). Every claim was still verified for real rather than
  assumed: offscreen GPU readback tests, a synthetic `NSEvent` posted through the app's actual
  `NSApplication.sendEvent:` path (which incidentally also captured real mouse movement during one
  test, correctly tracked), and temporary self-trigger hooks added, exercised, and reverted before
  each commit — confirmed clean via `git diff` every time.

## Repository layout

```
RayTracerBench/
  Core/        — shared, dual-compiled ray tracing algorithm + scene construction
  CPU/         — multi-threaded CPU renderer
  GPU/         — Metal compute renderer
  Shaders/     — Raytracer.metal (the shared algorithm), Blit.metal (display + magnifier lens)
  App/         — pure C++ AppKit UI (window, controls, results, image previews, About)
RayTracerBenchTests/  — plain command-line test executable (no XCTest)
ThirdParty/    — vendored metal-cpp / metal-cpp-extensions, plus this project's own
                 extensions to them (NSButton, NSTextField, NSAlert, CAMetalLayer, NSEvent, ...)
Docs/          — this file, plus the theory-and-code PDF
CLAUDE.md      — living architecture notes for future work on this repo
```

## Building and running

```
xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build
build/Debug/RayTracerBench
```

Tests: same pattern with `-scheme RayTracerBenchTests`, then run `build/Debug/RayTracerBenchTests`
directly (`xcodebuild test` doesn't apply — it's a plain executable, not an XCTest bundle).
