#include <metal_stdlib>
using namespace metal;

#include "../Core/ShaderTypes.h"
#include "../Core/RayTraceCore.h"

// One thread per pixel. sphereCount/camera/params arrive via setBytes() (small, read-only,
// uniform across all threads) rather than being folded into RenderParams — spheres/materials stay
// separate `device` buffers exactly like the CPU renderer's raw pointers, per RayTraceCore.h's
// RT_DEVICE design (see CLAUDE.md).
kernel void renderKernel(
	device const SphereGPU*   spheres [[buffer( 0 )]],
	constant uint32_t&        sphereCount [[buffer( 1 )]],
	device const MaterialGPU* materials [[buffer( 2 )]],
	constant CameraGPU&       camera [[buffer( 3 )]],
	constant RenderParams&    params [[buffer( 4 )]],
	texture2d<float, access::write> outTexture [[texture( 0 )]],
	uint2                     gid [[thread_position_in_grid]] )
{
	if ( gid.x >= params.width || gid.y >= params.height )
		return;

	uint32_t seed = pcgHash( params.frameSeed ^ ( gid.y * 9781u + gid.x * 6271u + 1u ) );

	simd_float3 colorSum = makeFloat3( 0.0, 0.0, 0.0 );

	for ( uint32_t s = 0; s < params.samplesPerPixel; ++s )
	{
		RandomFloatSample ru = randomFloat( seed );
		seed = ru.seed;
		RandomFloatSample rv = randomFloat( seed );
		seed = rv.seed;

		float u = ( float( gid.x ) + ru.value ) / float( params.width - 1 );
		float v = ( float( params.height - 1 - gid.y ) + rv.value ) / float( params.height - 1 );

		CameraRaySample cameraRay = getRay( camera, u, v, seed );
		seed = cameraRay.rngSeed;

		RayColorResult sample = rayColor( cameraRay.ray, spheres, sphereCount, materials, params.maxDepth, seed );
		seed = sample.rngSeed;

		colorSum = colorSum + sample.color;
	}

	simd_float3 averageColor = colorSum / float( params.samplesPerPixel );
	simd_float3 gammaCorrected = sqrt( max( averageColor, 0.0f ) );
	simd_float3 clamped = min( gammaCorrected, makeFloat3( 0.999f, 0.999f, 0.999f ) );

	outTexture.write( float4( clamped, 1.0 ), gid );
}
