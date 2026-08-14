#include <metal_stdlib>
using namespace metal;

#include "../Core/ShaderTypes.h"
#include "../Core/RayTraceCore.h"

// One thread per pixel. entityCount/camera/params arrive via setBytes() (small, read-only,
// uniform across all threads) rather than being folded into RenderParams — transforms/shapes/
// materials stay separate `device` buffers (the ECS component arrays; see ShaderTypes.h/Scene.hpp)
// exactly like the CPU renderer's raw pointers, per RayTraceCore.h's RT_DEVICE design (see
// CLAUDE.md).
kernel void renderKernel(
	device const TransformGPU* transforms [[buffer( 0 )]],
	device const ShapeGPU*     shapes [[buffer( 1 )]],
	constant uint32_t&         entityCount [[buffer( 2 )]],
	device const MaterialGPU*  materials [[buffer( 3 )]],
	constant CameraGPU&        camera [[buffer( 4 )]],
	constant RenderParams&     params [[buffer( 5 )]],
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

		RayColorResult sample = rayColor( cameraRay.ray, transforms, shapes, entityCount, materials, params.maxDepth, seed );
		seed = sample.rngSeed;

		colorSum = colorSum + sample.color;
	}

	simd_float3 averageColor = colorSum / float( params.samplesPerPixel );
	simd_float3 gammaCorrected = sqrt( max( averageColor, 0.0f ) );
	simd_float3 clamped = min( gammaCorrected, makeFloat3( 0.999f, 0.999f, 0.999f ) );

	outTexture.write( float4( clamped, 1.0 ), gid );
}
