# RayTracerBench, a Literate Program

This directory is a *literate programming* account of the RayTracerBench
source tree, in the sense Knuth meant the term: the program is presented as
an essay written for a human reader, in an order chosen for understanding
rather than the order a compiler needs, with the real source code woven
directly into the prose rather than summarized from a distance.

It is not a tangle/weave system — there is no tool here that regenerates
`RayTracerBench/` from these files, and these files are not compiled. What
carries over from the original idea is the *discipline*: every chapter below
picks a real, runnable slice of the program, quotes the actual code (with
`file:line` citations back to the source of truth), and explains not just
what each piece does but why it is written the way it is — the constraint,
the alternative that was rejected, or the bug that shaped the final form.
Where the "why" is already on record (in `CLAUDE.md`'s project-status notes,
in commit history, in the existing `Docs/RayTracerBench-Theory-and-Code`
narrative), these chapters cite it rather than re-deriving it from guesswork.

This is a *different* document from `Docs/RayTracerBench-Theory-and-Code`,
which is written for a reader who wants the ray-tracing theory first and the
engineering second. This one is organized module-by-module, follows the
code's own structure, and is meant to be read alongside the source files
open in an editor.

## Conventions used in every chapter

- Sections are numbered `§1`, `§2`, ... **local to each chapter** — think of
  each chapter as its own fascicle, not one continuously-numbered book.
- A section is prose first, code second: a paragraph motivating a design
  decision, followed by the fenced code excerpt that embodies it, cited as
  `path/to/file.ext:startLine-endLine`.
- Code is quoted verbatim from the real files. Nothing here is
  reconstructed or paraphrased into pseudocode; if a snippet is elided for
  length, that is marked explicitly (`/* ... */` with a note).
- Each chapter opens with a one-paragraph abstract and a short list of the
  files it covers, and closes with a "Where this connects" pointer to the
  neighboring chapters — the same way one section of a real program refers
  forward and backward to the sections that use or are used by it.
- These files are documentation only. They never modify anything under
  `RayTracerBench/` or `RayTracerBenchTests/`.

## Reading order

The chapters are ordered the way a reader building a mental model from
scratch would want them — data model first, then the algorithm shared by
both renderers, then each renderer, then the surrounding application, then
verification — rather than alphabetically or by directory.

| # | Chapter | Modules covered |
|---|---------|------------------|
| 1 | [The Core Data Model](01-core-data-model.md) | `Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp` |
| 2 | [The Shared Ray-Tracing Core](02-shared-raytrace-core.md) | `Core/RayTraceCore.h` |
| 3 | [The Metal Shaders](03-metal-shaders.md) | `Shaders/Raytracer.metal`, `Shaders/Blit.metal` |
| 4 | [The CPU Renderer](04-cpu-renderer.md) | `CPU/CPURenderer.hpp/.cpp` |
| 5 | [The GPU Renderer](05-gpu-renderer.md) | `GPU/GPURenderer.hpp/.cpp` |
| 6 | [Mesh Generation for Export](06-mesh-generation.md) | `Export/EntityMesh.hpp/.cpp` |
| 7 | [Scene Export: glTF, OBJ, and Preview Images](07-scene-export.md) | `Export/SceneExporter.hpp/.cpp`, `Export/ImageWriter.hpp/.cpp` |
| 8 | [The App Shell and Lifecycle](08-app-shell.md) | `main.cpp`, `App/AppDelegate.hpp/.cpp` |
| 9 | [UI Controls and Results](09-ui-controls.md) | `App/ControlsPanel.hpp/.cpp`, `App/ResultsPanel.hpp/.cpp`, `App/AboutAlert.hpp/.cpp` |
| 10 | [The Metal-Backed Image View, and Filling AppKit's Gaps](10-image-display-view.md) | `App/ImageDisplayView.hpp/.cpp`, plus the project-local `ThirdParty/metal-cpp-extensions` additions it depends on |
| 11 | [Tests: Parity, Geometry, and the Core Itself](11-tests.md) | `RayTracerBenchTests/*` |
| 12 | [The Build System](12-build-system.md) | `CMakeLists.txt` |

## What this program is, in one paragraph

RayTracerBench renders the same scene with a CPU ray tracer and a GPU
(Metal compute) ray tracer and compares them. The one hard constraint that
shapes nearly every file in this list is that Metal Shading Language
forbids virtual functions, RTTI, and dynamic allocation — so both renderers
share one address-space-parameterized, tagged-struct-and-switch core
(`Core/RayTraceCore.h`), compiled twice: once as plain C++ for the CPU path,
once as MSL for the GPU kernel. Chapter 2 is the heart of the whole
program; chapters 1, 3, 4, and 5 all exist to feed it data or execute it.
Chapters 6–10 are the surrounding application — exporting the scene to
standard 3D formats, and the AppKit UI, built without Objective-C++ by
filling gaps in Apple's `metal-cpp-extensions` bindings by hand. Chapter 11
is how all of the above was checked, including a real Monte-Carlo-variance
investigation that overturned an initial pixel-tolerance assumption.
