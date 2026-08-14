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
    RayTracerBench/App/ImageDisplayView.cpp
)

target_include_directories(RayTracerBench PRIVATE
    ThirdParty/metal-cpp
    ThirdParty/metal-cpp-extensions
)

# ImageDisplayView compiles Shaders/Blit.metal from source at runtime (newLibrary(), not
# newDefaultLibrary()) since this is deliberately a plain executable, not an app bundle with a
# compiled default.metallib resource — see ImageDisplayView.cpp's readShaderSource() comment.
target_compile_definitions(RayTracerBench PRIVATE
    RT_SHADERS_DIR="${CMAKE_SOURCE_DIR}/RayTracerBench/Shaders"
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
