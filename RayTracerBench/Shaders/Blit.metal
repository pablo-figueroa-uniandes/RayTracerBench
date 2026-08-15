// Fullscreen textured-quad blit: draws ImageDisplayView's source texture (the CPU renderer's
// RGBA8 output today; a GPU-rendered texture directly, once that exists) onto the view's
// CAMetalLayer drawable. Two triangles, no vertex buffer — positions/texCoords are baked into
// constant arrays indexed by vertex_id.
//
// Also implements the magnifying-glass loupe: when active, fragments inside a circular region
// around a lens center sample the source texture from a smaller, de-magnified region instead of
// 1:1, producing a zoomed inset. MagnifierUniforms is compiled from a raw in-memory source string
// (see ImageDisplayView.cpp's readShaderSource() comment — unlike Raytracer.metal, this file has
// no local #includes, so that's fine), so its layout is hand-duplicated in ImageDisplayView.cpp
// rather than shared via a header — KEEP IN SYNC if either side changes.

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

// KEEP IN SYNC with the identical struct in ImageDisplayView.cpp.
struct MagnifierUniforms
{
	float centerU;
	float centerV;
	float viewAspect; // width / height, so the lens reads as a circle rather than an ellipse
	float radius;     // normalized, as a fraction of view height
	float zoom;
	int   active;     // 0 = disabled; draw the source texture unmodified
};

// Emits one of the 6 baked-in quad vertices (2 triangles, no vertex buffer) for the given vertex_id.
vertex RasterizerData blitVertex( uint vertexID [[vertex_id]] )
{
	RasterizerData out;
	out.position = float4( kQuadPositions[ vertexID ], 0.0, 1.0 );
	out.texCoord = kQuadTexCoords[ vertexID ];
	return out;
}

// Samples sourceTexture at in.texCoord, applying the magnifier lens effect when active (see the
// file header comment).
fragment float4 blitFragment( RasterizerData in [[stage_in]],
	texture2d<float, access::sample> sourceTexture [[texture( 0 )]],
	constant MagnifierUniforms& magnifier [[buffer( 0 )]] )
{
	constexpr sampler linearSampler( coord::normalized, address::clamp_to_edge, filter::linear );

	if ( magnifier.active != 0 )
	{
		float2 center = float2( magnifier.centerU, magnifier.centerV );
		float2 delta = in.texCoord - center;
		// Correct for aspect ratio: 1 unit of U spans `width` pixels but 1 unit of V spans
		// `height` pixels, so scale U by viewAspect to compare both in "V-equivalent" units —
		// otherwise the lens would read as an ellipse on a non-square view.
		float2 deltaScreen = float2( delta.x * magnifier.viewAspect, delta.y );
		float  dist = length( deltaScreen );

		if ( dist < magnifier.radius )
		{
			float2 zoomedUV = center + delta / magnifier.zoom;
			float4 color = sourceTexture.sample( linearSampler, zoomedUV );

			// Thin white border ring so the lens's edge reads clearly against any image content.
			float ringWidth = magnifier.radius * 0.035;
			float ring = smoothstep( magnifier.radius - ringWidth, magnifier.radius, dist );
			color.rgb = mix( color.rgb, float3( 1.0, 1.0, 1.0 ), ring * 0.85 );
			return color;
		}
	}

	return sourceTexture.sample( linearSampler, in.texCoord );
}
