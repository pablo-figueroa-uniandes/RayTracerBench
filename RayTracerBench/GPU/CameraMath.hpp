#pragma once

#include "../Core/ShaderTypes.h"

#include <simd/simd.h>

// Small, shared view/projection matrix helpers, factored out of RasterRenderer.cpp's
// buildViewProjectionMatrix() so PipelineStageRenderer's "external observer" cameras (which have no
// CameraGPU of their own — they look *at* the scene's real camera, not through it) can reuse the
// exact same math instead of duplicating it. Header-only: both call sites are tiny, host-side C++
// (never Metal-compiled), so there's no dual-compile concern the way Core/RayTraceCore.h has.

// Builds a right-handed view matrix from an orthonormal right/up/back basis at `origin` — `back`
// is the "away from the view direction" axis (e.g. CameraGPU::w, or lookFrom-lookAt normalized),
// matching Metal's -Z-forward view space convention. view * point = the basis's transpose applied
// to (point - origin), i.e. project (point - origin) onto each basis axis.
inline simd_float4x4 buildViewMatrix( simd_float3 origin, simd_float3 right, simd_float3 up, simd_float3 back )
{
	simd_float4x4 view;
	view.columns[ 0 ] = simd_make_float4( right.x, up.x, back.x, 0.0f );
	view.columns[ 1 ] = simd_make_float4( right.y, up.y, back.y, 0.0f );
	view.columns[ 2 ] = simd_make_float4( right.z, up.z, back.z, 0.0f );
	view.columns[ 3 ] = simd_make_float4(
		-simd_dot( right, origin ), -simd_dot( up, origin ), -simd_dot( back, origin ), 1.0f );
	return view;
}

// Builds a right-handed-view-space-to-Metal's-[0,1]-depth-NDC perspective projection matrix from
// each axis's half-FOV tangent and the near/far planes — near maps to NDC z=0, far to NDC z=1
// (verified algebraically and by confirming a rendered frame actually shows the full scene, per
// RasterRenderer.cpp's original header comment).
inline simd_float4x4 buildPerspectiveProjection( float halfHFovTan, float halfVFovTan, float nearPlane, float farPlane )
{
	simd_float4x4 proj{};
	proj.columns[ 0 ] = simd_make_float4( 1.0f / halfHFovTan, 0.0f, 0.0f, 0.0f );
	proj.columns[ 1 ] = simd_make_float4( 0.0f, 1.0f / halfVFovTan, 0.0f, 0.0f );
	proj.columns[ 2 ] = simd_make_float4( 0.0f, 0.0f, farPlane / ( nearPlane - farPlane ), -1.0f );
	proj.columns[ 3 ] = simd_make_float4( 0.0f, 0.0f, ( nearPlane * farPlane ) / ( nearPlane - farPlane ), 0.0f );
	return proj;
}

// Scene extends roughly ±1000 units around the origin (the ground sphere has radius 1000 — see
// CLAUDE.md's export-bug story for that exact figure); these give generous margin on both sides
// without meaningful depth-precision loss at this scene's scale. Shared by RasterRenderer (the
// "Viewport Matrix" pipeline stage) and PipelineStageRenderer's "Orthographic Matrix" stage, which
// must use the exact same projection to be a faithful intermediate step of the same pipeline.
constexpr float kSceneNearPlane = 0.05f;
constexpr float kSceneFarPlane = 5000.0f;

// Derives the exact perspective view-projection matrix for the scene's own CameraGPU — see
// RasterRenderer.cpp's original design note for the full derivation (focus distance recovered from
// the precomputed viewport corner/extent vectors, half-FOV tangents from viewport extent /
// focusDist, `u`/`v`/`w` already a right-handed right/up/backward basis matching Metal's -Z-forward
// view space). Kept here, not duplicated, so every consumer of "the scene's real camera" — the
// raster renderer and the pipeline-stage visualizer alike — is locked to the exact same matrix.
inline simd_float4x4 buildSceneViewProjectionMatrix( const CameraGPU& cam )
{
	simd_float3 viewportCenter = cam.lowerLeftCorner + cam.horizontal * 0.5f + cam.vertical * 0.5f;
	float       focusDist = simd_dot( cam.origin - viewportCenter, cam.w );

	float halfVFovTan = ( simd_length( cam.vertical ) * 0.5f ) / focusDist;
	float halfHFovTan = ( simd_length( cam.horizontal ) * 0.5f ) / focusDist;

	simd_float4x4 view = buildViewMatrix( cam.origin, cam.u, cam.v, cam.w );
	simd_float4x4 proj = buildPerspectiveProjection( halfHFovTan, halfVFovTan, kSceneNearPlane, kSceneFarPlane );
	return simd_mul( proj, view );
}
