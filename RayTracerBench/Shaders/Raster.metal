// Standard graphics-pipeline shader for RasterRenderer: a plain vertex/fragment pair over an
// already-world-space mesh (see Export/EntityMesh.hpp — the same mesh builder the glTF/OBJ
// exporters use), lit with one fixed directional light plus an ambient term. Deliberately basic —
// no shadows, no reflections, no global illumination — that contrast with the physically-based
// path tracer is the whole point of this render path existing, not a gap to close (see CLAUDE.md).
//
// Compiled from a raw in-memory source string (see ImageDisplayView.cpp's readShaderSource()
// comment — like Blit.metal, this file has no local #includes, so that's fine), so RasterVertex/
// Uniforms are hand-duplicated in RasterRenderer.cpp rather than shared via a header — KEEP IN SYNC
// if either side changes (same precedent Blit.metal's MagnifierUniforms already sets).

#include <metal_stdlib>
using namespace metal;

// KEEP IN SYNC with the identical struct in RasterRenderer.cpp.
struct RasterVertex
{
	float3 position;
	float3 normal;
	float3 color;
};

// KEEP IN SYNC with the identical struct in RasterRenderer.cpp.
struct Uniforms
{
	float4x4 mvp;
};

struct RasterizerData
{
	float4 position [[position]];
	float3 worldNormal;
	float3 color;
};

vertex RasterizerData rasterVertex( uint vertexID [[vertex_id]],
	constant RasterVertex* vertices [[buffer( 0 )]],
	constant Uniforms& uniforms [[buffer( 1 )]] )
{
	RasterVertex in = vertices[ vertexID ];

	RasterizerData out;
	out.position = uniforms.mvp * float4( in.position, 1.0 );
	out.worldNormal = in.normal;
	out.color = in.color;
	return out;
}

// One fixed directional light (a "sun" from above/front-right) plus a flat ambient term — a
// standard, non-physical rasterization lighting model, not an attempt to match the ray tracer's
// light transport.
fragment float4 rasterFragment( RasterizerData in [[stage_in]] )
{
	constexpr float3 kLightDir = float3( 0.4082483, 0.8164966, 0.4082483 ); // normalize((1,2,1))
	constexpr float  kAmbient = 0.2;

	float3 n = normalize( in.worldNormal );
	float  diffuse = max( dot( n, kLightDir ), 0.0 );
	float3 shaded = in.color * ( kAmbient + ( 1.0 - kAmbient ) * diffuse );
	return float4( shaded, 1.0 );
}
