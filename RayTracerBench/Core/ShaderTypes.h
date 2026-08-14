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

// Geometry is organized ECS-style: an entity is just an array index into SceneDescription's
// parallel `transforms`/`shapes` component arrays (see Scene.hpp) rather than a polymorphic
// "Sphere : Hittable" object — the same tagged-struct idea MaterialGPU/MaterialType already uses
// for materials, generalized to shapes. See CLAUDE.md for the full rationale.

enum ShapeType
{
	SHAPE_SPHERE  = 0,
	SHAPE_PYRAMID = 1,
};

// Transform component, shared by every entity regardless of shape. `position` is the entity's
// local origin (a sphere's center; a pyramid's base-square center). `right`/`up`/`forward` are an
// orthonormal orientation basis — the identity basis for spheres (rotationally symmetric, so
// orientation is meaningless for them) and an arbitrary rotation for pyramids, whose `up` axis
// points from base to apex.
struct TransformGPU
{
	simd_float3 position;
	simd_float3 right;
	simd_float3 up;
	simd_float3 forward;
};

// Shape component: a tagged union of per-primitive geometry parameters plus a reference to this
// entity's Material component. Adding a new primitive means adding one more SHAPE_* tag and one
// more field group here, dispatched by a switch (see RayTraceCore.h's hitEntity()) rather than a
// new virtual base class — virtual dispatch is forbidden in MSL kernel code.
struct ShapeGPU
{
	int   type;          // ShapeType
	float radius;        // sphere: radius. pyramid: unused.
	float baseHalfWidth; // pyramid: half the base square's side length. sphere: unused.
	float height;        // pyramid: apex height above the base. sphere: unused.
	int   materialIndex; // index into SceneDescription::materials
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
