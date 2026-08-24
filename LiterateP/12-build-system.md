# Chapter 12: The Build System

**Abstract.** RayTracerBench is not built by hand-editing an Xcode project —
it is built by CMake, with the `.xcodeproj` itself treated as a generated
artifact that happens to be checked in for convenience. `CMakeLists.txt` is
short (136 lines) but carries real weight: it defines the three-target
layout (a framework-free static library, the app, and a headless test
binary), and it contains the one genuinely unusual build step in the whole
project — shelling out to the real Metal command-line compiler at build
time, because `Shaders/Raytracer.metal`'s verbatim `#include`s of the shared
core headers make it impossible to compile the way `Blit.metal` is compiled
(from an in-memory string at runtime). This chapter walks the file top to
bottom: the target layout, the `.metallib` custom command and the
`RT_RAYTRACER_METALLIB`/`RT_SHADERS_DIR` compile definitions it and its
sibling produce, the vendored `ThirdParty` include paths, the operational
rule for regenerating the Xcode project after a source change, and the
build/test commands this project actually uses (including the one that
looks like it should work and doesn't).

Files covered: `CMakeLists.txt`.

## §1. Three targets: a framework-free core library, the app, and a headless test binary

The whole file builds exactly three CMake targets, and the split between
them is not incidental — it is the same "framework-free vs. needs
AppKit/Metal" line that runs through the rest of the project's module
layout. The first is `RayTracerCore`, a static library:

```cmake
# CMakeLists.txt:20-29
# RayTraceCore.h and ShaderTypes.h are header-only and dual-compiled into
# Shaders/Raytracer.metal directly (not through this library) once the GPU
# renderer exists. CPURenderer.cpp is framework-free C++17 like Core/, so it
# lives in the same static library rather than a separate CMake target.
add_library(RayTracerCore STATIC
    RayTracerBench/Core/Scene.cpp
    RayTracerBench/CPU/CPURenderer.cpp
    RayTracerBench/Export/EntityMesh.cpp
    RayTracerBench/Export/SceneExporter.cpp
)

target_include_directories(RayTracerCore PUBLIC
    RayTracerBench/Core
    RayTracerBench/CPU
    RayTracerBench/Export
)
```

Every `.cpp` in this list is plain C++17 with no Apple-framework
dependency: `Scene.cpp` (Chapter 1) only touches `<simd/simd.h>` types and
the standard library's `std::mt19937`; `CPURenderer.cpp` (Chapter 4) is
explicitly framework-free by design; and `EntityMesh.cpp`/`SceneExporter.cpp`
(Chapters 6 and 7) build meshes and write glTF/OBJ text with nothing but
the standard library. `RayTraceCore.h` and `ShaderTypes.h` themselves never
appear in this target's source list at all — as the comment says, they are
header-only and get pulled into two separate compilations instead: this
library's `.cpp` files include them as plain C++, and `Shaders/Raytracer.metal`
(§2 below) includes the identical files again as MSL. `RayTracerCore` is a
library of *consumers* of that shared header pair, not a place where it is
compiled once and reused.

Note what is conspicuously absent from this list: `Export/ImageWriter.cpp`.
It builds meshes and writes 3D-format text like its siblings above, but it
also calls into CoreGraphics/ImageIO to rasterize a PNG (Chapter 7), and
`RayTracerCore` is deliberately kept framework-free so it never needs to
link against anything beyond the C++ standard library. `ImageWriter.cpp` is
compiled into the app target instead:

```cmake
# CMakeLists.txt:37-46
add_executable(RayTracerBench
    RayTracerBench/main.cpp
    RayTracerBench/App/AppDelegate.cpp
    RayTracerBench/App/AboutAlert.cpp
    RayTracerBench/App/ControlsPanel.cpp
    RayTracerBench/App/ResultsPanel.cpp
    RayTracerBench/App/ImageDisplayView.cpp
    RayTracerBench/GPU/GPURenderer.cpp
    RayTracerBench/Export/ImageWriter.cpp
)
```

`RayTracerBench` is where everything that actually needs a framework lives:
all of `App/` (AppKit, via `metal-cpp-extensions`), `GPU/GPURenderer.cpp`
(Metal), and `Export/ImageWriter.cpp` (CoreGraphics/ImageIO). It links
`RayTracerCore` in later (§5) to get the shared scene/CPU/export code, so
the app target is really "the framework-dependent half of the program,
plus the framework-free half linked in" rather than a self-contained list
of every source file the app needs.

The third target, `RayTracerBenchTests` (Chapter 11), reuses the same
split again — it links `RayTracerCore` for the scene/CPU code its tests
exercise, and separately compiles `GPU/GPURenderer.cpp` directly into
itself (rather than linking against `RayTracerBench`, which doesn't exist
as a library) so its parity tests can drive the real GPU renderer too:

```cmake
# CMakeLists.txt:113-119
add_executable(RayTracerBenchTests
    RayTracerBenchTests/main.cpp
    RayTracerBenchTests/RayTraceCoreTests.cpp
    RayTracerBenchTests/DeterministicParityTests.cpp
    RayTracerBenchTests/EntityMeshTests.cpp
    RayTracerBench/GPU/GPURenderer.cpp
)
```

Its own comment on this target states the same reasoning explicitly:

```cmake
# CMakeLists.txt:111-112
# RayTracerBenchTests: a plain command-line C++ tool (no XCTest, no Objective-C) — headless, so it
# only needs Metal + Foundation, not AppKit/metal-cpp-extensions/Cocoa.
```

## §2. The `.metallib` custom command: compiling Raytracer.metal to a real file on disk

`Shaders/Raytracer.metal` cannot be handed to Metal as an in-memory source
string the way `Shaders/Blit.metal` is (Chapter 3). Blit.metal is
self-contained — a small textured-quad vertex/fragment shader with no local
`#include`s — so `ImageDisplayView` can read its text off disk at runtime
and hand the raw string to `newLibrary(sourceString, ...)`. Raytracer.metal
is different: it `#include`s `Core/ShaderTypes.h` and `Core/RayTraceCore.h`
verbatim, using ordinary relative-path `#include` directives, and those
directives have nothing to resolve against when the "file" being compiled
is just a string sitting in memory with no location of its own. The build
system's answer is to make Raytracer.metal a real, separately compiled
artifact, using the actual command-line Metal toolchain:

```cmake
# CMakeLists.txt:61-83
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
```

This is a two-stage pipeline, mirroring exactly what Xcode's own "Metal
Compiler" build phase would do under the hood, but invoked as two plain
`xcrun` command lines instead: `xcrun -sdk macosx metal -c ... -o
Raytracer.air` compiles the `.metal` source (with its `#include`s now
resolving normally, since it's a real file on disk in its real directory)
down to Apple's intermediate representation (`.air`), and `xcrun -sdk
macosx metallib ...` links that `.air` file into the final `.metallib`
container `MTL::Device::newLibrary(filepath, &error)` can load. The
`DEPENDS` list is what makes this a correct incremental build rather than a
one-shot script: naming `Raytracer.metal` *and* the two headers it
`#include`s means CMake/Ninja/Xcode's build system will re-run this custom
command whenever any of the three changes, not just when the `.metal` file
itself is touched — exactly the dependency an `#include` relationship
requires, expressed the CMake way instead of relying on a compiler-generated
dependency file. `add_custom_target(RaytracerMetalLib ...)` gives the
custom command's single output file a named target that `RayTracerBench`
can depend on via `add_dependencies`, which is the standard CMake idiom for
making an executable target wait on a custom command that doesn't produce
one of its own directly-linked object files.

The comment's parenthetical — "still command-line `xcrun`, not an Xcode
'Metal Compiler' build phase" — is a deliberate consistency choice, not an
Xcode limitation: Xcode *does* have a built-in Metal Compiler phase that
could do this same job when the `.xcodeproj` is opened in the GUI, but
using it would mean the `.metallib` only gets built correctly when Xcode
generates that phase itself, which reintroduces exactly the kind of
GUI-project-template dependency §4 explains this project avoids. Shelling
out to `xcrun` from a `CMakeLists.txt` custom command works identically
whether the project is built from Xcode, from `xcodebuild` on the command
line, or (on another platform's generator) from Ninja or Make — it is the
one build step that has to reach outside CMake's own compiler abstractions,
and it does so in the same headless-friendly way as everything else here.

## §3. `RT_RAYTRACER_METALLIB` and `RT_SHADERS_DIR`: two different answers to "where do I find my shader"

The `.metallib` custom command produces a file at a path CMake controls
(`${CMAKE_BINARY_DIR}/Raytracer.metallib`), and `GPURenderer` needs to know
that exact path at runtime to call `newLibrary(filepath, &error)` on it
(Chapter 5). Rather than hard-coding a path or reading an environment
variable, the build injects it directly as a preprocessor definition on the
target that needs it:

```cmake
# CMakeLists.txt:85-87
target_compile_definitions(RayTracerBench PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
)
```

and the same definition is repeated for the test target, since
`RayTracerBenchTests` also constructs a real `GPURenderer` to drive its
parity tests:

```cmake
# CMakeLists.txt:126-128
target_compile_definitions(RayTracerBenchTests PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
)
```

`RT_SHADERS_DIR` solves a related but distinct problem, for the *other*
shader. `Blit.metal` (Chapter 3, and used by `ImageDisplayView`, Chapter
10) is compiled from an in-memory source string, which means something in
the app still has to read that string off disk first — and since
`RayTracerBench` is deliberately a plain executable rather than an app
bundle with resources copied alongside a compiled `default.metallib`, there
is no bundle-relative resource path to resolve against. The build injects
the shader source directory itself instead, so `ImageDisplayView` can
`fopen`/read `Blit.metal`'s text directly from the source tree at runtime:

```cmake
# CMakeLists.txt:53-59
# ImageDisplayView compiles Shaders/Blit.metal from source at runtime (newLibrary(), not
# newDefaultLibrary()) since this is deliberately a plain executable, not an app bundle with a
# compiled default.metallib resource — see ImageDisplayView.cpp's readShaderSource() comment.
# Blit.metal has no local #includes, so a raw source string is fine for it.
target_compile_definitions(RayTracerBench PRIVATE
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
)
```

The two definitions are answers to the same underlying question —
"where is my shader at runtime" — but they differ in exactly the way §2
explains the two `.metal` files differ: `RT_RAYTRACER_METALLIB` names a
*compiled artifact* CMake produced during the build, because Raytracer.metal
needed real on-disk `#include` resolution and thus needed to be compiled
ahead of time; `RT_SHADERS_DIR` names a *source directory* CMake never
touches, because Blit.metal is self-contained and can be compiled from a
string at the moment it's needed. Both are `${CMAKE_SOURCE_DIR}`- or
`${CMAKE_BINARY_DIR}`-rooted absolute paths baked in at configure/build
time, which is why they only ever make sense as compile definitions on a
specific build tree, not as something meaningful to a shipped, relocated
binary — consistent with this being a benchmark tool built and run in
place, not a distributed app bundle.

## §4. Regenerating the Xcode project from CMake, not editing it by hand

`RayTracerBench.xcodeproj` (the file actually opened by `Product > Run` and
invoked by `xcodebuild`) is not a hand-built Xcode project — it is CMake's
Xcode generator output, copied into the repository root:

```
cmake -S . -B <scratch-dir> -G Xcode
```

followed by copying the resulting `.xcodeproj` over the tracked one. This
is a direct consequence of how this project is developed: project creation
here happens headlessly, with no interactive Xcode session available to
click through "New Project" wizards, add target membership, or wire up
framework search paths by hand in a GUI inspector. CMake's Xcode generator
produces the exact same effect — an `.xcodeproj` with the right targets,
sources, include paths, compile definitions, and linked frameworks — purely
from this text file, which is something a headless session can actually
run.

The operational consequence is a rule, not just a fact about how the file
was first created: **`CMakeLists.txt` is the source of truth.** After
adding, removing, or moving a source file, or after changing an include
path, a compile definition, or a linked framework, the correct next step is
to regenerate the `.xcodeproj` from `CMakeLists.txt` again (the same
`cmake -S . -B <scratch-dir> -G Xcode` invocation, then re-copy the result
over the tracked copy) — never to open the tracked `.xcodeproj` and add the
change by hand in Xcode's file inspector or build-phase editor. A hand-edit
would work until the next regeneration silently discards it, and in the
meantime the tracked project would describe a build that `CMakeLists.txt`
itself doesn't produce, which is exactly the kind of drift a single
source-of-truth file exists to prevent. This is the sort of rule a
maintainer needs to be told once, in writing, and then never violate by
instinct — which is why it belongs in this chapter rather than left to be
rediscovered the first time a hand-edited build phase mysteriously
disappears.

## §5. The vendored `ThirdParty` include paths

`RayTracerCore`'s own include directories (`RayTracerBench/Core`,
`RayTracerBench/CPU`, `RayTracerBench/Export`, from §1) are marked
`PUBLIC`, which is what lets `RayTracerBench` reach `Core/Scene.hpp` and
friends by a bare `#include "Scene.hpp"` merely by linking against
`RayTracerCore` — CMake propagates `PUBLIC` include directories to anything
that links the library, without `RayTracerBench` having to repeat them
itself:

```cmake
# CMakeLists.txt:89
target_link_libraries(RayTracerBench PRIVATE RayTracerCore)
```

The vendored AppKit/Metal bindings need their own, separate include paths,
since they are headers-only and never compiled into a library of their
own. They are wired directly onto the app target:

```cmake
# CMakeLists.txt:48-51
target_include_directories(RayTracerBench PRIVATE
    ThirdParty/metal-cpp
    ThirdParty/metal-cpp-extensions
)
```

`ThirdParty/metal-cpp` is Apple's own header-only C++ binding to
Metal/Foundation (`<Metal/Metal.hpp>`, `<Foundation/Foundation.hpp>`, and so
on); `ThirdParty/metal-cpp-extensions` is the AppKit/QuartzCore extension
headers from the same `LearnMetalCPP.zip` sample bundle, plus this
project's own additions filling Apple's gaps (`NSControl.hpp`,
`NSButton.hpp`, `NSTextField.hpp`, `NSAlert.hpp`, `CAMetalLayer.hpp`,
`NSEvent.hpp`, all covered in Chapter 10). Both directories are `PRIVATE`
to `RayTracerBench` because nothing in `RayTracerCore` — the
framework-free half of the program — ever needs to see a Metal or AppKit
type.

`RayTracerBenchTests` needs a narrower slice of the same vendored tree.
Since it is headless (no AppKit at all, per its own comment in §1) but
still constructs a real `GPURenderer`, it only needs `metal-cpp`'s Metal
bindings, plus `RayTracerBench/GPU` so it can `#include "GPURenderer.hpp"`
directly (it compiles `GPURenderer.cpp` straight into itself rather than
linking against the app target, which isn't a library):

```cmake
# CMakeLists.txt:121-124
target_include_directories(RayTracerBenchTests PRIVATE
    ThirdParty/metal-cpp
    RayTracerBench/GPU
)
```

Note what's absent here relative to `RayTracerBench`'s include list:
`ThirdParty/metal-cpp-extensions` never appears for the test target, which
is a direct, load-bearing consequence of the "no XCTest, no Objective-C,
headless" design called out in §1 — a test binary that never touches
AppKit has no reason to see the AppKit extension headers at all.

## §6. Frameworks linked, and why ImageWriter's two extra ones only reach the app target

`RayTracerBench` links the frameworks the whole app needs across UI,
rendering, and export:

```cmake
# CMakeLists.txt:91-108
find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
# ImageWriter.cpp writes the scene-export preview PNG via CoreGraphics/ImageIO's plain C APIs —
# real system frameworks, not a vendored image library, and no Objective-C needed.
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)

target_link_libraries(RayTracerBench PRIVATE
    ${COCOA_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    ${IMAGEIO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)
```

`Cocoa`, `Metal`, `MetalKit`, `QuartzCore`, and `Foundation` cover the UI
layer (AppKit widgets, the `CAMetalLayer`-backed `ImageDisplayView`) and the
GPU renderer. `CoreGraphics` and `IMAGEIO_FRAMEWORK` are there for exactly
one reason, called out in the adjacent comment: `Export/ImageWriter.cpp`
(Chapter 7) rasterizes the scene-export preview PNG using
CoreGraphics/ImageIO's plain C `CGImageCreate`/`CGImageDestination*` APIs,
which is also precisely why `ImageWriter.cpp` was kept out of
`RayTracerCore` in §1 — linking those two frameworks into the
framework-free static library would have broken the one property that
library exists to have. `RayTracerBenchTests`, correspondingly, links only
the two frameworks it actually needs:

```cmake
# CMakeLists.txt:132-136
target_link_libraries(RayTracerBenchTests PRIVATE
    RayTracerCore
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)
```

`Metal` because it drives a real `GPURenderer`, `Foundation` because
`metal-cpp`'s `NS::` types depend on it — and nothing else, since it never
touches AppKit, CoreGraphics, ImageIO, or QuartzCore.

## §7. Build and test commands, and the one that doesn't work

The build command for the app is the ordinary `xcodebuild` invocation
against the generated (§4), checked-in `.xcodeproj`:

```
xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build
```

The equivalent command for the test target looks like it should be
`xcodebuild test -scheme RayTracerBenchTests`, and it is worth stating
precisely why that specific command fails, since it is the kind of thing
that looks like an environment problem the first time someone hits it and
is actually a structural fact about how this target is defined. From
`CLAUDE.md`:

> `xcodebuild test -scheme RayTracerBenchTests` does not work — confirmed
> by trying it, not assumed — since `RayTracerBenchTests` is a plain
> command-line C++ tool (no XCTest/ObjC), so its CMake-generated scheme has
> no Test action configured; `xcodebuild test` fails with "Scheme ... is
> not currently configured for the test action."

The root cause traces straight back to §1: `add_executable(RayTracerBenchTests
...)` in `CMakeLists.txt` declares an ordinary command-line executable
target, the same kind of target `RayTracerBench` itself is, with no XCTest
bundle, no `XCTestCase` subclasses, and nothing for CMake's Xcode generator
to wire into a scheme's Test action — there is no XCTest target for
`xcodebuild test` to even attempt to run. `RayTracerBenchTests/main.cpp`
(Chapter 11) is instead a plain `main()` that runs `RayTraceCoreTests.cpp`,
`DeterministicParityTests.cpp`, and `EntityMeshTests.cpp`'s checks directly
and reports pass/fail on its own, the same as any other C++ command-line
program would. The correct way to run it is therefore to build the scheme
as an ordinary product and then execute the resulting binary directly,
locating it first:

```
xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBenchTests -configuration Debug build
xcodebuild -showBuildSettings -scheme RayTracerBenchTests | grep TARGET_BUILD_DIR
```

and then invoking `<TARGET_BUILD_DIR>/RayTracerBenchTests` directly (or
running it from Xcode's GUI, which amounts to the same "build, then run the
plain executable" flow rather than any Test-action machinery). This is the
`add_dependencies(RayTracerBenchTests RaytracerMetalLib)` line (§2) doing
its job in the background during that build step — the test binary needs
`Raytracer.metallib` to exist before it runs, exactly as `RayTracerBench`
does, since it constructs a real `GPURenderer` against the same compiled
library (§3).

## Where this connects

- **Chapter 3** (`Shaders/Raytracer.metal`, `Shaders/Blit.metal`) is the
  pair of shader sources this file treats so differently: Raytracer.metal
  is the one whose verbatim `#include`s of `Core/ShaderTypes.h` and
  `Core/RayTraceCore.h` force the real `xcrun`/`.metallib` compile step in
  §2, while Blit.metal's lack of local `#include`s is exactly why it can
  stay a runtime source string under `RT_SHADERS_DIR` (§3) instead.
- **Chapter 5** (`GPU/GPURenderer.hpp/.cpp`) is the consumer of
  `RT_RAYTRACER_METALLIB`: its constructor calls
  `_pDevice->newLibrary(NS::String::string(RT_RAYTRACER_METALLIB, ...),
  &pError)` on exactly the path this file's custom command produces, never
  a source string, for the reason spelled out in both files' comments.
- **Chapter 7** (`Export/SceneExporter.hpp/.cpp`,
  `Export/ImageWriter.hpp/.cpp`) is why `RayTracerCore` and
  `RayTracerBench` split the way they do in §1 and §6: `ImageWriter.cpp`'s
  CoreGraphics/ImageIO dependency is the one thing in the whole `Export/`
  module that isn't framework-free, so it alone is compiled into and linked
  against the app target rather than the shared static library.
- **Chapter 11** (`RayTracerBenchTests/*`) is the target this file defines
  in §1 and links in §6, and whose actual checks (RayTraceCore unit tests,
  deterministic CPU/GPU parity, EntityMesh winding-order verification) are
  what running the binary produced by §7's build/locate/run sequence
  reports pass or fail on.
