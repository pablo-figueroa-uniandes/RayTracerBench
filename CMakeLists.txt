cmake_minimum_required(VERSION 3.20)
project(RayTracerBench LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "RayTracerBench targets macOS only (Metal + AppKit via metal-cpp).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

target_include_directories(RayTracerBench PRIVATE
    ThirdParty/metal-cpp
    ThirdParty/metal-cpp-extensions
)

# ImageDisplayView compiles Shaders/Blit.metal from source at runtime (newLibrary(), not
# newDefaultLibrary()) since this is deliberately a plain executable, not an app bundle with a
# compiled default.metallib resource — see ImageDisplayView.cpp's readShaderSource() comment.
# Blit.metal has no local #includes, so a raw source string is fine for it.
target_compile_definitions(RayTracerBench PRIVATE
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
)

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

target_link_libraries(RayTracerBench PRIVATE RayTracerCore)

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

# RayTracerBenchTests: a plain command-line C++ tool (no XCTest, no Objective-C) — headless, so it
# only needs Metal + Foundation, not AppKit/metal-cpp-extensions/Cocoa.
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

target_include_directories(RayTracerBenchTests PRIVATE
    ThirdParty/metal-cpp
    RayTracerBench/GPU
)

target_compile_definitions(RayTracerBenchTests PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
    # RasterRenderer.cpp reads Raster.metal's source from disk at runtime, like ImageDisplayView.cpp
    # does for Blit.metal — only RayTracerBench had this definition before RasterRendererTests needed it too.
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
)

add_dependencies(RayTracerBenchTests RaytracerMetalLib)

target_link_libraries(RayTracerBenchTests PRIVATE
    RayTracerCore
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)
