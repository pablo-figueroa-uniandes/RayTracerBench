#pragma once

#include "ShaderTypes.h"

#include <vector>

// ECS-style layout: an entity is simply an array index — transforms[i]/shapes[i] are that
// entity's Transform and Shape components (see ShaderTypes.h). There is no separate list per
// shape type; spheres and pyramids are interleaved in the same two parallel arrays and
// distinguished only by ShapeGPU::type, exactly like RayTraceCore.h's hitEntity() dispatches on it.
// Each entity's material is itself a component reference (ShapeGPU::materialIndex) into the
// separate `materials` component array, so many entities can share one material record.
struct SceneDescription
{
	std::vector<TransformGPU> transforms;
	std::vector<ShapeGPU>     shapes;
	std::vector<MaterialGPU>  materials;
	CameraGPU                 camera;
	RenderParams              params;
};

// Builds the classic "Ray Tracing in One Weekend" demo scene — a ground sphere, a randomized
// ~22x22 grid of small spheres, three large feature spheres (glass / lambertian / metal), and a
// handful of square pyramids at varied 3D orientations — deterministically for a given seed via a
// seeded std::mt19937. Both the CPU and GPU renderers consume this exact same SceneDescription, so
// identical seeds produce identical entity layouts/materials on both (per-pixel noise still
// differs — see CLAUDE.md's verification notes).
//
// `floating`, when true, places the small randomized-field spheres at a random height in
// [radius, kMaxFloatHeight] (see Scene.cpp) instead of resting on the ground — kMaxFloatHeight was
// chosen and verified by rendering the fixed camera setup below and confirming nothing clips out
// of frame, not derived from an unverified formula. The three large feature spheres (glass /
// lambertian / metal) and the pyramids always rest on the ground regardless of `floating`.
SceneDescription buildDefaultScene( unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, bool floating = false );
