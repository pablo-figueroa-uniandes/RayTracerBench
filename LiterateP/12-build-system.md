# Chapter 12: The Build System

**Abstract.** RayTracerBench is not built by hand-editing an Xcode project —
it is built by CMake, with the `.xcodeproj` itself treated as a generated
artifact that happens to be checked in for convenience. `CMakeLists.txt` is
short (158 lines) but carries real weight: it defines the three-target
layout (a framework-free static library, the app, and a headless test
binary), and it contains the one genuinely unusual build step in the whole
project — shelling out to the real Metal command-line compiler at build
time, because `Shaders/Raytracer.metal`'s verbatim `#include`s of the shared
core headers make it impossible to compile the way `Blit.metal` is compiled
(from an in-memory string at runtime). This chapter walks the file top to
bottom: the target layout, the `.metallib` custom command and the
`RT_RAYTRACER_METALLIB`/`RT_SHADERS_DIR` compile definitions it and its
sibling produce, the vendored `ThirdParty` include paths, the operational
rule for regenerating the Xcode project after a source change, the
build/test commands this project actually uses (including the one that
looks like it should work and doesn't), and a real build-system bug this
project's own history ran into — a `SYMROOT` override that looked correct
and silently wasn't — told in full as a cautionary case study in Xcode
generator internals.

Files covered: `CMakeLists.txt`.

## §1. Three targets: a framework-free core library, the app, and a headless test binary

The whole file builds exactly three CMake targets, and the split between
them is not incidental — it is the same "framework-free vs. needs
AppKit/Metal" line that runs through the rest of the project's module
layout. The first is `RayTracerCore`, a static library:

```cmake
# CMakeLists.txt:28-39
# RayTraceCore.h and ShaderTypes.h are header-only and dual-compiled into
# Shaders/Raytracer.metal directly (not through this library) once the GPU
# renderer exists. CPURenderer.cpp is framework-free C++17 like Core/, so it
# lives in the same static library rather than a separate CMake target.
add_library(RayTracerCore STATIC
    RayTracerBench/Core/Scene.cpp
    RayTracerBench/CPU/CPURenderer.cpp
    RayTracerBench/Export/Base64.cpp
    RayTracerBench/Export/EntityMesh.cpp
    RayTracerBench/Export/SceneExporter.cpp
    RayTracerBench/Export/SceneImporter.cpp
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
explicitly framework-free by design; and `EntityMesh.cpp`/`SceneExporter.cpp`/
`SceneImporter.cpp`/`Base64.cpp` (Chapter 7) build meshes and read/write
glTF/OBJ text and base64 data with nothing but the standard library and a
hand-rolled JSON parser. `RayTraceCore.h` and `ShaderTypes.h` themselves
never appear in this target's source list at all — as the comment says,
they are header-only and get pulled into two separate compilations
instead: this library's `.cpp` files include them as plain C++, and
`Shaders/Raytracer.metal` (§2 below) includes the identical files again as
MSL. `RayTracerCore` is a library of *consumers* of that shared header
pair, not a place where it is compiled once and reused.

Note what is conspicuously absent from this list: `Export/ImageWriter.cpp`.
It builds meshes and writes 3D-format text like its siblings above, but it
also calls into CoreGraphics/ImageIO to rasterize a PNG (Chapter 7), and
`RayTracerCore` is deliberately kept framework-free so it never needs to
link against anything beyond the C++ standard library. `ImageWriter.cpp` is
compiled into the app target instead, alongside `GPU/RasterRenderer.cpp`
(Chapter 13) and `GPU/PipelineStageRenderer.cpp` (Chapter 14) — both need
Metal, which the app target already links (§6), so they join
`GPU/GPURenderer.cpp` rather than moving into `RayTracerCore`:

```cmake
# CMakeLists.txt:47-59
add_executable(RayTracerBench
    RayTracerBench/main.cpp
    RayTracerBench/App/AppDelegate.cpp
    RayTracerBench/App/AboutAlert.cpp
    RayTracerBench/App/ControlsPanel.cpp
    RayTracerBench/App/ResultsPanel.cpp
    RayTracerBench/App/ImageDisplayView.cpp
    RayTracerBench/App/PipelineVisualizationWindow.cpp
    RayTracerBench/GPU/GPURenderer.cpp
    RayTracerBench/GPU/RasterRenderer.cpp
    RayTracerBench/GPU/PipelineStageRenderer.cpp
    RayTracerBench/Export/ImageWriter.cpp
)
```

`RayTracerBench` is where everything that actually needs a framework lives:
all of `App/` (AppKit, via `metal-cpp-extensions`), `GPU/*.cpp` (Metal),
and `Export/ImageWriter.cpp` (CoreGraphics/ImageIO). It links
`RayTracerCore` in later (§5) to get the shared scene/CPU/export code, so
the app target is really "the framework-dependent half of the program,
plus the framework-free half linked in" rather than a self-contained list
of every source file the app needs.

The third target, `RayTracerBenchTests` (Chapter 11), reuses the same
split again — it links `RayTracerCore` for the scene/CPU code its tests
exercise, and separately compiles `GPU/GPURenderer.cpp`, `GPU/RasterRenderer.cpp`,
and `GPU/PipelineStageRenderer.cpp` directly into itself (rather than
linking against `RayTracerBench`, which doesn't exist as a library) so its
parity, raster, and pipeline-stage tests can drive the real Metal-backed
renderers too:

```cmake
# CMakeLists.txt:126-137
add_executable(RayTracerBenchTests
    RayTracerBenchTests/main.cpp
    RayTracerBenchTests/RayTraceCoreTests.cpp
    RayTracerBenchTests/DeterministicParityTests.cpp
    RayTracerBenchTests/EntityMeshTests.cpp
    RayTracerBenchTests/SceneImportExportTests.cpp
    RayTracerBenchTests/RasterRendererTests.cpp
    RayTracerBenchTests/PipelineStageTests.cpp
    RayTracerBench/GPU/GPURenderer.cpp
    RayTracerBench/GPU/RasterRenderer.cpp
    RayTracerBench/GPU/PipelineStageRenderer.cpp
)
```

Its own comment on this target states the same reasoning explicitly:

```cmake
# CMakeLists.txt:124-125
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
# CMakeLists.txt:74-96
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
# CMakeLists.txt:98-100
target_compile_definitions(RayTracerBench PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
)
```

and the same definition is repeated for the test target, since
`RayTracerBenchTests` also constructs a real `GPURenderer` to drive its
parity tests:

```cmake
# CMakeLists.txt:144-149
target_compile_definitions(RayTracerBenchTests PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
    # RasterRenderer.cpp reads Raster.metal's source from disk at runtime, like ImageDisplayView.cpp
    # does for Blit.metal — only RayTracerBench had this definition before RasterRendererTests needed it too.
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
)
```

The test target's copy of this block picked up a second definition,
`RT_SHADERS_DIR`, once `RasterRendererTests.cpp` (Chapter 13, §7) started
constructing a real `RasterRenderer` — which, like `ImageDisplayView`,
reads its shader's source text off disk at runtime rather than from a
precompiled `.metallib`, so the test binary needs to know where the
`Shaders/` directory is too, for exactly the reason explained next.

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
# CMakeLists.txt:66-72
# ImageDisplayView compiles Shaders/Blit.metal from source at runtime (newLibrary(), not
# newDefaultLibrary()) since this is deliberately a plain executable, not an app bundle with a
# compiled default.metallib resource — see ImageDisplayView.cpp's readShaderSource() comment.
# Blit.metal has no local #includes, so a raw source string is fine for it.
target_compile_definitions(RayTracerBench PRIVATE
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
)
```

`Shaders/Raster.metal` (Chapter 13) follows Blit.metal's model, not
Raytracer.metal's — it has no local `#include`s either, so `RasterRenderer`
reads it from disk and compiles it from a source string at runtime,
reusing this same `RT_SHADERS_DIR` definition rather than needing a third
one of its own. `Shaders/Wireframe.metal` (Chapter 14) does too.

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
# CMakeLists.txt:102
target_link_libraries(RayTracerBench PRIVATE RayTracerCore)
```

The vendored AppKit/Metal bindings need their own, separate include paths,
since they are headers-only and never compiled into a library of their
own. They are wired directly onto the app target:

```cmake
# CMakeLists.txt:61-64
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
# CMakeLists.txt:139-142
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
# CMakeLists.txt:104-122
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
# CMakeLists.txt:153-157
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
(Chapter 11) is instead a plain `main()` that runs all six test files'
checks directly (`RayTraceCoreTests.cpp`, `DeterministicParityTests.cpp`,
`EntityMeshTests.cpp`, `SceneImportExportTests.cpp`, `RasterRendererTests.cpp`,
`PipelineStageTests.cpp`) and reports pass/fail on its own, the same as any
other C++ command-line program would. The correct way to run it is therefore to build the scheme
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

## §8. The `SYMROOT` saga: a build-system bug that looked fixed three times before it was

This project's own history includes a real build-system investigation
worth telling in full, in the Knuth sense of showing the bug that shaped
the final form rather than presenting the finished file as though it were
obvious from the start. An earlier version of `CMakeLists.txt` set a custom
`SYMROOT` — the intent, stated in that version's own comment, was to pin
build products to a fixed `<repo>/build` location regardless of where
`cmake -S . -B <dir> -G Xcode` happened to be invoked from, so that
regenerating the Xcode project into a scratch directory (this project's
headless generation workflow, §4) and then deleting that scratch directory
wouldn't leave the checked-in `.xcodeproj` pointing at a build folder that
no longer existed. What actually followed was three attempts, each one
looking like a fix until the next build proved otherwise:

1. **`CMAKE_XCODE_ATTRIBUTE_SYMROOT` alone.** This changed *where Xcode
   said* products would go, but not where CMake's Xcode generator had
   already computed several *other* absolute paths would be — the
   `.metallib` custom command's `OUTPUT`/`DEPENDS` paths (§2) and, more
   subtly, the linker flags connecting `RayTracerBench` to `RayTracerCore`.
2. **Also forcing `CMAKE_XCODE_ATTRIBUTE_CONFIGURATION_BUILD_DIR` to
   `"$(SYMROOT)/$(CONFIGURATION)"`.** This is the standard fix for
   "`SYMROOT` alone didn't move everything" in ordinary Xcode projects, and
   it worked — but only at the project level. CMake's Xcode generator
   *also* emits its own `CONFIGURATION_BUILD_DIR` value directly on each
   individual target, and a per-target build setting always wins over a
   project-level one in Xcode's build-setting resolution order, so each
   target's actual output silently kept going to the generator's own
   computed location regardless of the project-level override.
3. **`set_target_properties(... XCODE_ATTRIBUTE_CONFIGURATION_BUILD_DIR
   ...)` per target**, to fight the generator's per-target value directly.
   This one actually worked for each target's *own* output location — but
   uncovered the real, previously-hidden problem: the *linker* flags
   `RayTracerBench` uses to find `RayTracerCore`'s compiled static library
   are written by CMake's Xcode generator directly into `OTHER_LDFLAGS` as
   a literal absolute path string, computed from `CMAKE_BINARY_DIR` at
   *generation* time — not as an Xcode build-setting reference like
   `$(CONFIGURATION_BUILD_DIR)` that would follow any later override. Once
   each target's own product moved to the new, overridden location, the
   consuming target's hardcoded `OTHER_LDFLAGS` entry kept pointing at
   the *old*, generator-computed path, and the link failed outright with
   "no such file or directory" — confirmed by actually running the build,
   not inferred from reading the generated project file.

The eventual fix was to stop fighting the generator and instead work with
its actual, consistent default: Xcode's own default `SYMROOT` (unset) is
`$(PROJECT_DIR)/build`, a `build` subfolder nested under wherever the
generated `.xcodeproj` itself lives — which is exactly `CMAKE_BINARY_DIR`,
the same directory every other absolute path in this file (the
`.metallib` custom command's paths in §2, the `OTHER_LDFLAGS` entries that
broke attempt 3) is generated to already agree with. Once every override
was removed, every path in the generated project was internally consistent
again, because they had all been computed from the same
`CMAKE_BINARY_DIR` all along — the problem was never that CMake's paths
disagreed with each other, only that the `SYMROOT` overrides tried to
introduce a *second*, inconsistent notion of where products live
alongside the first. `CMakeLists.txt` records this as the operational rule
it actually is, not just a historical note:

```cmake
# CMakeLists.txt:12-26
# No custom SYMROOT override (an earlier version of this file had one, meant to "pin build products
# to <repo>/build regardless of where `cmake -B <dir>` is invoked from" — removed after it turned out
# to not actually be achievable). CMake's Xcode generator bakes several *other* absolute paths from
# CMAKE_BINARY_DIR as literal strings regardless of any SYMROOT override — not just the Raytracer.metallib
# custom-command paths below, but also, critically, inter-target link references: when RayTracerBench
# links RayTracerCore, the generator writes RayTracerCore's expected static-library path directly into
# OTHER_LDFLAGS as a plain linker argument (not a $(...)  build-setting reference), computed from
# CMAKE_BINARY_DIR at *generation* time — so overriding SYMROOT/CONFIGURATION_BUILD_DIR only moves
# where RayTracerCore's product actually gets *copied*, not where the consumer's hardcoded OTHER_LDFLAGS
# entry *looks for it*, breaking the link with "no such file or directory" (confirmed the hard way).
# Xcode's own default SYMROOT (unset here) is "$(PROJECT_DIR)/build" — a "build" subfolder nested
# under wherever the generated .xcodeproj itself lives (CMAKE_BINARY_DIR) — which is what every path
# in this file, including the ones below, is generated to agree with. The one hard requirement this
# leaves: `cmake -S . -B <dir> -G Xcode` must always regenerate into the *same*, persistent `<dir>`
# (never a one-off /tmp path) — see the "regenerating RayTracerBench.xcodeproj" note in CLAUDE.md.
```

That last sentence is the rule this whole investigation actually produced:
`cmake -S . -B .cmake-xcode -G Xcode` (§4) must always target the same
persistent, `.gitignore`d `.cmake-xcode/` directory, never a one-off
scratch path that gets deleted afterward — because the moment that
directory stops existing, every one of these baked-in absolute paths
(the `.metallib` command's output, `RayTracerCore`'s link path, the
`RT_RAYTRACER_METALLIB` compile definition itself) points at nothing, and
no `SYMROOT` override can be made to survive that in its place. This is
also, concretely, why a stray `build/` directory once accumulated in the
repository root and had to be cleaned up by hand: it was a leftover from
exactly this trial-and-error process, not a file this project's normal
workflow produces on its own.

---

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
- **Chapter 7** (`Export/SceneExporter.hpp/.cpp`, `Export/SceneImporter.hpp/.cpp`,
  `Export/Base64.hpp/.cpp`, `Export/ImageWriter.hpp/.cpp`) is why
  `RayTracerCore` and `RayTracerBench` split the way they do in §1 and §6:
  `ImageWriter.cpp`'s CoreGraphics/ImageIO dependency is the one thing in
  the whole `Export/` module that isn't framework-free, so it alone is
  compiled into and linked against the app target rather than the shared
  static library, while the round-trip import/export/base64 code all
  stays in `RayTracerCore`.
- **Chapter 11** (`RayTracerBenchTests/*`) is the target this file defines
  in §1 and links in §6, and whose actual checks (RayTraceCore unit tests,
  deterministic CPU/GPU parity, EntityMesh winding-order verification,
  scene import/export round-trips, raster smoke tests, pipeline-stage
  geometry-math checks) are what running the binary produced by §7's
  build/locate/run sequence reports pass or fail on.
- **Chapter 13** (`GPU/RasterRenderer.hpp/.cpp`, `GPU/CameraMath.hpp`,
  `Shaders/Raster.metal`) is why `RT_SHADERS_DIR` (§3) is shared between
  two consumers instead of one, and why `RasterRenderer.cpp` appears in
  both the app target's and the test target's source lists in §1.
- **Chapter 14** (`GPU/PipelineStageRenderer.hpp/.cpp`,
  `Shaders/Wireframe.metal`, `App/PipelineVisualizationWindow.hpp/.cpp`) is
  the other new consumer of the same pattern §1 and §3 describe for
  Chapter 13 — `PipelineStageRenderer.cpp` compiled into both the app and
  test targets, `Wireframe.metal` read from `RT_SHADERS_DIR` at runtime.
