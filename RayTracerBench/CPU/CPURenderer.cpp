#include "CPURenderer.hpp"

#include "../Core/RayTraceCore.h"

#include <algorithm>
#include <thread>

namespace
{
	// Gamma-2 correction (sqrt) plus the book's [0,0.999] clamp before quantizing to a byte, so a
	// component of exactly 1.0 rounds down to 255 rather than overflowing to 0.
	uint8_t toByte( float c )
	{
		float gammaCorrected = std::sqrt( std::max( 0.0f, c ) );
		float clamped = std::min( gammaCorrected, 0.999f );
		return static_cast<uint8_t>( 256.0f * clamped );
	}

	// Renders rows [rowStart, rowEnd) of `scene` into `pixels` (RGBA8, row-major, row 0 = top),
	// tracing every sample of every pixel in that band through the shared RayTraceCore.h algorithm.
	void renderRows( const SceneDescription& scene, std::vector<uint8_t>& pixels, uint32_t rowStart, uint32_t rowEnd )
	{
		const uint32_t width = scene.params.width;
		const uint32_t height = scene.params.height;
		const uint32_t samplesPerPixel = scene.params.samplesPerPixel;
		const uint32_t maxDepth = scene.params.maxDepth;

		const TransformGPU* transforms = scene.transforms.data();
		const ShapeGPU*     shapes = scene.shapes.data();
		const uint32_t      entityCount = static_cast<uint32_t>( scene.transforms.size() );
		const MaterialGPU*  materials = scene.materials.data();

		for ( uint32_t j = rowStart; j < rowEnd; ++j )
		{
			for ( uint32_t i = 0; i < width; ++i )
			{
				// Distinct per-pixel seed derivation; only randomFloat()/pcgHash() themselves need
				// to match the GPU renderer bit-for-bit, not this seeding formula (see CLAUDE.md's
				// "eyeball correctness" note — per-pixel noise legitimately differs CPU vs GPU).
				uint32_t seed = pcgHash( scene.params.frameSeed ^ ( j * 9781u + i * 6271u + 1u ) );

				simd_float3 colorSum = makeFloat3( 0.0f, 0.0f, 0.0f );

				for ( uint32_t s = 0; s < samplesPerPixel; ++s )
				{
					RandomFloatSample ru = randomFloat( seed );
					seed = ru.seed;
					RandomFloatSample rv = randomFloat( seed );
					seed = rv.seed;

					float u = ( static_cast<float>( i ) + ru.value ) / static_cast<float>( width - 1 );
					float v = ( static_cast<float>( height - 1 - j ) + rv.value ) / static_cast<float>( height - 1 );

					CameraRaySample cameraRay = getRay( scene.camera, u, v, seed );
					seed = cameraRay.rngSeed;

					RayColorResult sample = rayColor( cameraRay.ray, transforms, shapes, entityCount, materials, maxDepth, seed );
					seed = sample.rngSeed;

					colorSum = colorSum + sample.color;
				}

				simd_float3 averageColor = colorSum / static_cast<float>( samplesPerPixel );

				const size_t pixelIndex = ( static_cast<size_t>( j ) * width + i ) * 4;
				pixels[ pixelIndex + 0 ] = toByte( averageColor.x );
				pixels[ pixelIndex + 1 ] = toByte( averageColor.y );
				pixels[ pixelIndex + 2 ] = toByte( averageColor.z );
				pixels[ pixelIndex + 3 ] = 255;
			}
		}
	}
}

// Renders the full image, either on the calling thread (SingleThreaded) or by partitioning rows
// across std::thread::hardware_concurrency() threads (MultiThreaded), and times the whole render.
CPURenderResult renderCPU( const SceneDescription& scene, CPUThreading::Mode mode )
{
	const uint32_t width = scene.params.width;
	const uint32_t height = scene.params.height;

	CPURenderResult result;
	result.pixels.resize( static_cast<size_t>( width ) * height * 4 );

	const auto startTime = std::chrono::high_resolution_clock::now();

	if ( mode == CPUThreading::SingleThreaded || height == 0 )
	{
		renderRows( scene, result.pixels, 0, height );
	}
	else
	{
		unsigned threadCount = std::thread::hardware_concurrency();
		if ( threadCount == 0 )
			threadCount = 4;
		threadCount = std::min<unsigned>( threadCount, height );

		std::vector<std::thread> threads;
		threads.reserve( threadCount );

		const uint32_t rowsPerThread = ( height + threadCount - 1 ) / threadCount;

		for ( unsigned t = 0; t < threadCount; ++t )
		{
			const uint32_t rowStart = t * rowsPerThread;
			const uint32_t rowEnd = std::min( rowStart + rowsPerThread, height );
			if ( rowStart >= rowEnd )
				continue;
			threads.emplace_back( renderRows, std::cref( scene ), std::ref( result.pixels ), rowStart, rowEnd );
		}

		for ( std::thread& t : threads )
			t.join();
	}

	const auto endTime = std::chrono::high_resolution_clock::now();
	result.renderTime = endTime - startTime;

	return result;
}
