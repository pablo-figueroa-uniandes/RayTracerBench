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
SceneDescription buildDefaultScene( unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth );
