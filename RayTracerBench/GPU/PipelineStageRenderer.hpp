#pragma once

#include "../Core/Scene.hpp"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <vector>

// One colored line-segment endpoint — KEEP IN SYNC with the identical struct in
// Shaders/Wireframe.metal. Every consecutive pair forms one line (MTL::PrimitiveTypeLine).
struct LineVertex
{
	simd_float3 position;
	simd_float3 color;
};

// Sphere wireframes are a single equatorial-ring proxy, not their full tessellated mesh — see
// buildSceneWireframe()'s comment for why.
constexpr int kSphereRingSegments = 32;

// Entities whose own local bounding radius exceeds this are excluded from framing-bounds fitting
// (see fitFramingSphere()) but still drawn in the wireframe itself. Without this, an "external
// observer" camera trying to fit the whole scene — including the ground sphere's radius-1000 extent
// against a ~22-unit field of radius-0.2 spheres — would zoom out so far that every "interesting"
// object collapses to a few pixels: the exact illusion this session's Unity-import investigation
// diagnosed (see CLAUDE.md), now deliberately avoided here instead of reproduced.
constexpr float kFramingSizeExcludeThreshold = 50.0f;

// Radius of the smallest sphere centered at the shape's own local origin that contains it: exactly
// `radius` for a sphere, or the farthest of a pyramid's 5 local vertices (apex + 4 base corners)
// from that origin otherwise.
float entityBoundingRadius( const ShapeGPU& shape );

// Builds a legible wireframe "line soup" of the scene in world space. Reuses
// Export/EntityMesh.hpp's buildEntityMesh() (already world-space, already used by the raster
// renderer and the glTF/OBJ exporter) for pyramids — only 5 entities, already low-poly (18
// verts/6 tris), so their exact triangle-edge wireframe is cheap and legible. Spheres are NOT drawn
// via their full tessellated mesh: at ~490 entities that would be hundreds of thousands of line
// segments, an unreadable tangle. Instead each sphere gets one kSphereRingSegments-segment
// equatorial ring at its actual center/radius (spheres always use the identity orientation basis —
// see Scene.cpp's identityTransformAt() — so "equatorial" is exactly the entity's own right/forward
// plane). Colored per-entity via the same flat-material-color convention RasterRenderer.cpp uses.
//
// `excludeOversizedEntities`, when true, skips any entity whose entityBoundingRadius() exceeds
// kFramingSizeExcludeThreshold — used by the Projective/Camera-Matrix stages, where drawing the
// scene's actual radius-1000 ground sphere produces a giant, scene-scale-dominating ring that
// reads as visual noise rather than "ground" (its top touches world Y=0, right where the rest of
// the scene sits, so it sweeps directly across the diagram). Default false preserves this
// function's original "draw everything" behavior for the Orthographic-Matrix stage and for tests.
std::vector<LineVertex> buildSceneWireframe( const SceneDescription& scene, bool excludeOversizedEntities = false );

// A sphere fit to a set of points — used to auto-frame an "external observer" camera at a scene.
// Not a minimal enclosing sphere: center is the centroid, radius is the farthest point from it —
// simple, deterministic, and good enough for camera framing.
struct FramingBounds
{
	simd_float3 center;
	float       radius;
};
FramingBounds fitFramingSphere( const std::vector<simd_float3>& points );

// Every entity's own center point (transform.position), excluding any entity whose
// entityBoundingRadius() exceeds kFramingSizeExcludeThreshold (see its comment) — plus whatever
// extra points the caller passes in (e.g. the scene camera's origin, or the image plane's 4
// corners for the Projective-Matrix stage). Feed the result to fitFramingSphere() to frame an
// external observer camera; the caller is responsible for transforming these points first if the
// stage being framed isn't world space (see PipelineStageRenderer.cpp's Camera-Matrix stage).
std::vector<simd_float3> collectFramingPoints( const SceneDescription& scene, const std::vector<simd_float3>& extraPoints = {} );

// Ray/plane intersection: projects `point` (as seen from `origin`) onto the plane through
// `planePoint` with normal `planeNormal`. Used for the Projective-Matrix stage's "objects deformed
// so they can be painted into the image plane" visual. Points behind `origin` aren't clipped — an
// explicit, documented scope limit (this app's own camera setups never put scene geometry behind
// themselves, so it never arises in practice).
simd_float3 projectOntoPlane( simd_float3 origin, simd_float3 point, simd_float3 planePoint, simd_float3 planeNormal );

// One rendered pipeline-stage diagram.
struct PipelineStageResult
{
	// Not owned by the caller; valid until the next render*Stage() call for the *same* stage on
	// this PipelineStageRenderer (each stage keeps its own texture so all three can be displayed
	// together without one overwriting another).
	MTL::Texture* pTexture;
};

// Renders the three wireframe pipeline-stage diagrams ("Projective Matrix", "Camera Matrix",
// "Orthographic Matrix" — see CLAUDE.md's pipeline-visualization design note for the full
// rationale). The fourth stage, "Viewport Matrix", is exactly RasterRenderer's existing output —
// deliberately not reimplemented here.
class PipelineStageRenderer
{
	public:
		explicit PipelineStageRenderer( MTL::Device* pDevice );
		~PipelineStageRenderer();

		// World space: the (dimmed) scene wireframe as context, a camera icon (axis triad), the
		// image-plane rectangle (CameraGPU's lowerLeftCorner/horizontal/vertical — the exact
		// rectangle getRay() already casts rays through), a schematic ground/reference plane, an
		// axis-aligned box over the entities actually within the camera's field of view (the
		// curated "object" a classic eye/sight-line/picture-plane perspective diagram draws to —
		// see computeObjectBox()), straight sight-line rays from the eye to that box's corners, and
		// the box's own corners ray-cast through the picture plane and redrawn with the box's edge
		// connectivity as the "deformed" outline — viewed by an auto-framed external observer camera.
		PipelineStageResult renderProjectiveStage( const SceneDescription& scene, uint32_t width, uint32_t height );
		// The same wireframe transformed by the scene camera's own view matrix (so the camera ends
		// up at the origin looking down -Z, by construction) plus both a coordinate-axis triad and a
		// small camera-body gizmo at the origin — viewed by a second auto-framed external observer
		// camera.
		PipelineStageResult renderCameraStage( const SceneDescription& scene, uint32_t width, uint32_t height );
		// The wireframe fully transformed to NDC (the scene's real view+projection matrix, then a
		// CPU-side perspective divide) plus a reference square at exactly (-1,-1)-(1,1), displayed
		// directly (identity MVP — the data is already normalized).
		PipelineStageResult renderOrthographicStage( const SceneDescription& scene, uint32_t width, uint32_t height );

	private:
		void   rebuildTargetIfNeeded( MTL::Texture** ppTexture, uint32_t width, uint32_t height );
		double drawLines( MTL::Texture* pTexture, const std::vector<LineVertex>& lines, simd_float4x4 mvp, uint32_t width, uint32_t height );

		MTL::Device*               _pDevice;
		MTL::CommandQueue*         _pCommandQueue;
		MTL::RenderPipelineState*  _pPipelineState;

		// One texture per stage (not shared) — all three stages get computed before any of them is
		// displayed, so a shared texture would have every panel show only the last stage rendered.
		MTL::Texture* _pProjectiveTexture;
		MTL::Texture* _pCameraTexture;
		MTL::Texture* _pOrthographicTexture;
};
