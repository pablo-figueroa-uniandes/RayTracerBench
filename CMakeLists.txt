cmake_minimum_required(VERSION 3.20)
project(RayTracerBench LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "RayTracerBench targets macOS only (Metal + AppKit via metal-cpp).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RayTraceCore.h and ShaderTypes.h are header-only and dual-compiled into
# Shaders/Raytracer.metal directly (not through this library) once the GPU
# renderer exists. CPURenderer.cpp is framework-free C++17 like Core/, so it
# lives in the same static library rather than a separate CMake target.
add_library(RayTracerCore STATIC
    RayTracerBench/Core/Scene.cpp
    RayTracerBench/CPU/CPURenderer.cpp
)

target_include_directories(RayTracerCore PUBLIC
    RayTracerBench/Core
    RayTracerBench/CPU
)

add_executable(RayTracerBench
    RayTracerBench/main.cpp
    RayTracerBench/App/AppDelegate.cpp
    RayTracerBench/App/AboutAlert.cpp
    RayTracerBench/App/ControlsPanel.cpp
    RayTracerBench/App/ResultsPanel.cpp
    RayTracerBench/App/ImageDisplayView.cpp
    RayTracerBench/GPU/GPURenderer.cpp
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

target_link_libraries(RayTracerBench PRIVATE
    ${COCOA_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)

# RayTracerBenchTests: a plain command-line C++ tool (no XCTest, no Objective-C) — headless, so it
# only needs Metal + Foundation, not AppKit/metal-cpp-extensions/Cocoa.
add_executable(RayTracerBenchTests
    RayTracerBenchTests/main.cpp
    RayTracerBenchTests/RayTraceCoreTests.cpp
    RayTracerBenchTests/DeterministicParityTests.cpp
    RayTracerBench/GPU/GPURenderer.cpp
)

target_include_directories(RayTracerBenchTests PRIVATE
    ThirdParty/metal-cpp
    RayTracerBench/GPU
)

target_compile_definitions(RayTracerBenchTests PRIVATE
    RT_RAYTRACER_METALLIB="${RAYTRACER_METALLIB}"
)

add_dependencies(RayTracerBenchTests RaytracerMetalLib)

target_link_libraries(RayTracerBenchTests PRIVATE
    RayTracerCore
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)
