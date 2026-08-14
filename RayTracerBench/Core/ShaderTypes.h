#pragma once

// Plain, byte-for-byte-shared data passed between the CPU renderer and the
// GPU kernel's argument buffers. Included verbatim from both plain C++
// (CPURenderer, Scene, GPURenderer) and Raytracer.metal — <simd/simd.h>
// types keep layout identical on both sides.

#include <simd/simd.h>

enum MaterialType
{
	MAT_LAMBERTIAN = 0,
	MAT_METAL      = 1,
	MAT_DIELECTRIC = 2,
};

struct MaterialGPU
{
	int         type; // MaterialType
	simd_float3 albedo;
	float       fuzz; // metal roughness in [0,1]; unused for other types
	float       ir;   // dielectric index of refraction; unused for other types
};

struct SphereGPU
{
	simd_float3 center;
	float       radius;
	int         materialIndex; // index into the parallel materials array
};

struct CameraGPU
{
	simd_float3 origin;
	simd_float3 lowerLeftCorner;
	simd_float3 horizontal;
	simd_float3 vertical;
	simd_float3 u;
	simd_float3 v;
	simd_float3 w;
	float       lensRadius;
};

struct RenderParams
{
	uint32_t width;
	uint32_t height;
	uint32_t samplesPerPixel;
	uint32_t maxDepth;
	uint32_t frameSeed;
};
