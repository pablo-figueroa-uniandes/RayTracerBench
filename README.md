# RayTracerBench

A native macOS app that renders the same scene with both a CPU ray tracer and a GPU (Metal
compute) ray tracer and compares their performance — render time, GPU-only time, estimated
rays/sec, and a labeled speedup ratio.

Written in **pure C++ and Metal** via Apple's header-only [metal-cpp](https://developer.apple.com/metal/cpp/)
and metal-cpp-extensions bindings, deliberately avoiding SwiftUI/Objective-C++. There are no
hand-written `.mm` files anywhere in this project.

## Building

- Xcode GUI: open `RayTracerBench.xcodeproj`, `Product > Run`.
- Command line: `xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build`

`RayTracerBench.xcodeproj` is generated from `CMakeLists.txt` via CMake's Xcode generator; see
`CLAUDE.md` if you need to regenerate it after adding source files.

## Testing

`RayTracerBenchTests` is a plain command-line C++ tool (no XCTest, no Objective-C):

```
xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBenchTests -configuration Debug build
<path-to-built-binary>/RayTracerBenchTests
```

It covers `Core/RayTraceCore.h`'s `hitSphere()` directly, and a deterministic CPU-vs-GPU pixel
parity check (same seed, both renderers, diffed with a tolerance that accounts for expected
chaotic floating-point branch divergence at low sample counts — see `CLAUDE.md`'s verification
notes for why a strict per-pixel bound doesn't hold).

## Architecture

`RayTraceCore.h`, `ShaderTypes.h`, and `Scene.hpp/.cpp` in `Core/` are shared, unmodified, between
the CPU renderer (`CPU/CPURenderer.hpp/.cpp`, plain C++17) and the GPU renderer
(`GPU/GPURenderer.hpp/.cpp` + `Shaders/Raytracer.metal`) — the same tagged-struct,
switch-dispatched ray tracing algorithm runs on both, so the CPU/GPU comparison is apples-to-apples
rather than two independently-written implementations that merely look similar. See `CLAUDE.md`
for the full architecture writeup.

## Attribution

This project is based on the algorithm and structure of two reference works — structural
precedent only, no source copied:

- **["Ray Tracing in One Weekend"](https://raytracing.github.io/)** by Peter Shirley, Trevor David
  Black, and Steve Hollasch. CC0 (public domain) — no attribution required, credited here as a
  courtesy.
- **[raytracinginoneweekendincuda](https://github.com/rogerallen/raytracinginoneweekendincuda)**,
  a CUDA port of the above by Roger Allen. Public domain — likewise credited as a courtesy, not a
  requirement.

## License

MIT — see `LICENSE`. Copyright Pablo Figueroa and Claude.

`ThirdParty/metal-cpp` and `ThirdParty/metal-cpp-extensions` are vendored from Apple's
`LearnMetalCPP.zip` sample bundle and carry their own Apache 2.0 licenses (see the `LICENSE.txt`
in each of those directories) — they are not covered by this project's MIT license.
