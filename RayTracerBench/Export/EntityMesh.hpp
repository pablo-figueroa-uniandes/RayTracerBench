#pragma once

#include "../Core/ShaderTypes.h"

#include <cstdint>
#include <vector>

// Sphere tessellation density (see EntityMesh.cpp's buildSphereMesh()) — exposed here, not kept
// file-local in EntityMesh.cpp, so SceneImporter.cpp can derive the exact same "is this a
// tessellated sphere?" vertex-count signature ((latN+1)*(lonN+1)) instead of duplicating a magic
// number that could silently drift out of sync with this if the density ever changes.
constexpr int kSphereLatSegments = 8;
constexpr int kSphereLonSegments = 12;

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
