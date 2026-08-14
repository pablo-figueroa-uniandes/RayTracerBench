// Fullscreen textured-quad blit: draws ImageDisplayView's source texture (the CPU renderer's
// RGBA8 output today; a GPU-rendered texture directly, once that exists) onto the view's
// CAMetalLayer drawable. Two triangles, no vertex buffer — positions/texCoords are baked into
// constant arrays indexed by vertex_id.

#include <metal_stdlib>
using namespace metal;

struct RasterizerData
{
	float4 position [[position]];
	float2 texCoord;
};

constant float2 kQuadPositions[ 6 ] = {
	float2( -1.0, -1.0 ), float2( 1.0, -1.0 ), float2( -1.0, 1.0 ),
	float2( -1.0, 1.0 ), float2( 1.0, -1.0 ), float2( 1.0, 1.0 )
};

// texCoord.y=0 at the top row: matches ImageDisplayView's source texture, which is uploaded via
// replaceRegion() directly from a row-major, row-0-is-top RGBA8 buffer (see CPURenderer.hpp).
constant float2 kQuadTexCoords[ 6 ] = {
	float2( 0.0, 1.0 ), float2( 1.0, 1.0 ), float2( 0.0, 0.0 ),
	float2( 0.0, 0.0 ), float2( 1.0, 1.0 ), float2( 1.0, 0.0 )
};

vertex RasterizerData blitVertex( uint vertexID [[vertex_id]] )
{
	RasterizerData out;
	out.position = float4( kQuadPositions[ vertexID ], 0.0, 1.0 );
	out.texCoord = kQuadTexCoords[ vertexID ];
	return out;
}

fragment float4 blitFragment( RasterizerData in [[stage_in]], texture2d<float, access::sample> sourceTexture [[texture( 0 )]] )
{
	constexpr sampler linearSampler( coord::normalized, address::clamp_to_edge, filter::linear );
	return sourceTexture.sample( linearSampler, in.texCoord );
}
