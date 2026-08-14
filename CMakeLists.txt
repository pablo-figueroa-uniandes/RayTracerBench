cmake_minimum_required(VERSION 3.20)
project(RayTracerBench LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "RayTracerBench targets macOS only (Metal + AppKit via metal-cpp).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(RayTracerBench
    RayTracerBench/main.cpp
    RayTracerBench/App/AppDelegate.cpp
)

target_include_directories(RayTracerBench PRIVATE
    ThirdParty/metal-cpp
    ThirdParty/metal-cpp-extensions
)

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
