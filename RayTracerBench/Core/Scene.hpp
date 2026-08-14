#pragma once

#include "ShaderTypes.h"

#include <vector>

struct SceneDescription
{
	std::vector<SphereGPU>   spheres;
	std::vector<MaterialGPU> materials;
	CameraGPU                camera;
	RenderParams             params;
};

// Builds the classic "Ray Tracing in One Weekend" demo scene — a ground sphere, a randomized
// ~22x22 grid of small spheres, and three large feature spheres (glass / lambertian / metal) —
// deterministically for a given seed via a seeded std::mt19937. Both the CPU and GPU renderers
// consume this exact same SceneDescription, so identical seeds produce identical sphere
// layouts/materials on both (per-pixel noise still differs — see CLAUDE.md's verification notes).
//
// `floating`, when true, places the small randomized-field spheres at a random height in
// [radius, kMaxFloatHeight] (see Scene.cpp) instead of resting on the ground — kMaxFloatHeight was
// chosen and verified by rendering the fixed camera setup below and confirming nothing clips out
// of frame, not derived from an unverified formula. The three large feature spheres (glass /
// lambertian / metal) always rest on the ground regardless of `floating`.
SceneDescription buildDefaultScene( unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, bool floating = false );
