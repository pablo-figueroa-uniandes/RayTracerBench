#pragma once

#include "../Core/Scene.hpp"

#include <string>

// Result of a load attempt: `ok` plus a human-readable summary (a success message naming what was
// reconstructed, or an error), and — only when `ok` — the reconstructed `scene`.
struct SceneImportResult
{
	bool              ok;
	std::string       message;
	SceneDescription  scene; // valid only when ok
};

// Reconstructs objects/positions/materials from a .gltf/.obj this app's own SceneExporter.hpp
// previously wrote — the inverse of exportSceneAsGLTF()/exportSceneAsOBJ(). Camera and render
// params (width/aspectRatio/samplesPerPixel/maxDepth/seed) are NOT part of what gets exported (see
// SceneExporter.hpp's header comment — geometry only), so the caller supplies them here exactly
// like buildDefaultScene()'s equivalent parameters; only the entity/material component arrays come
// from the file.
//
// Reconstruction is exact (up to the exported file's own text/float precision) for entities this
// app generated, by exploiting the fact that buildEntityMesh() always produces one of exactly two
// recognizable shapes: a 117-vertex shared-vertex UV sphere (see EntityMesh.cpp's
// kSphereLatSegments/kSphereLonSegments) or an 18-vertex unshared-vertex faceted pyramid — see
// SceneImporter.cpp's reconstructSphere()/reconstructPyramid() for the geometry this decodes back
// out of each. A mesh matching neither vertex-count signature (i.e. not something this app wrote)
// is reported as an error rather than silently guessed at.
SceneImportResult importSceneFromGLTF( const std::string& path, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, unsigned seed );

// Same contract as importSceneFromGLTF(), reading a <path>.obj + its companion .mtl (found via the
// .obj's own "mtllib" line, matching how exportSceneAsOBJ() names it).
SceneImportResult importSceneFromOBJ( const std::string& path, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, unsigned seed );
