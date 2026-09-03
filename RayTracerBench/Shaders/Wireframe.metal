// Plain colored-line-segment shader for PipelineStageRenderer's three wireframe pipeline-stage
// diagrams ("Projective Matrix" / "Camera Matrix" / "Orthographic Matrix" — see CLAUDE.md's
// pipeline-visualization design note). No lighting, no depth test — this is a schematic diagram,
// not a solid-occlusion render, so overlapping lines are expected and meant to show through.
//
// Self-contained (no local #includes), so — like Blit.metal/Raster.metal — it's compiled from an
// in-memory source string at runtime rather than needing Raytracer.metal's xcrun/.metallib build
// step. LineVertex/Uniforms are hand-duplicated in PipelineStageRenderer.cpp — KEEP IN SYNC if
// either side changes, the same precedent Blit.metal's MagnifierUniforms already sets.

#include <metal_stdlib>
using namespace metal;

// KEEP IN SYNC with the identical struct in PipelineStageRenderer.hpp.
struct LineVertex
{
	float3 position;
	float3 color;
};

// KEEP IN SYNC with the identical struct in PipelineStageRenderer.cpp.
struct Uniforms
{
	float4x4 mvp;
};

struct RasterizerData
{
	float4 position [[position]];
	float3 color;
};

vertex RasterizerData wireframeVertex( uint vertexID [[vertex_id]],
	constant LineVertex* vertices [[buffer( 0 )]],
	constant Uniforms& uniforms [[buffer( 1 )]] )
{
	LineVertex in = vertices[ vertexID ];

	RasterizerData out;
	out.position = uniforms.mvp * float4( in.position, 1.0 );
	out.color = in.color;
	return out;
}

fragment float4 wireframeFragment( RasterizerData in [[stage_in]] )
{
	return float4( in.color, 1.0 );
}
