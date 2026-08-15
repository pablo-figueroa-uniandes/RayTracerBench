#pragma once

#include "../Core/ShaderTypes.h"

#include <cstdint>
#include <vector>

// A world-space triangle mesh: parallel positions/normals arrays plus a triangle-list index
// buffer (3 indices per face), in the layout both the glTF and OBJ/MTL exporters expect.
struct MeshData
{
	std::vector<simd_float3> positions;
	std::vector<simd_float3> normals;
	std::vector<uint32_t>    indices;
};

// Builds a world-space mesh for one entity's Transform+Shape components, dispatching on the Shape
// component's tag — the mesh-export analogue of RayTraceCore.h's hitEntity() intersection system:
// same ECS dispatch-by-tag idea (see CLAUDE.md), a different "system" operating over the same
// components. Spheres are tessellated (no native sphere primitive in glTF/OBJ); pyramids are
// exact, faceted geometry (flat per-triangle normals — see buildPyramidMesh's winding-order note).
MeshData buildEntityMesh( TransformGPU transform, ShapeGPU shape );
