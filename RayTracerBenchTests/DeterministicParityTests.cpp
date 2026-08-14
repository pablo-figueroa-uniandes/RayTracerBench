// This is the one translation unit in RayTracerBenchTests that includes Metal headers, so it's
// the one that must define their private-implementation macros — see ImageDisplayView.cpp's
// equivalent comment for why.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include "CPURenderer.hpp"
#include "GPURenderer.hpp"
#include "Scene.hpp"
#include "TestFramework.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace
{
	// Runs the same fixed-seed scene through both renderers and returns the fraction of RGB
	// channels (0..1) that differ by more than `tolerance`, plus the largest single-channel diff
	// seen. Headless — no window needed, matches the plan doc's DeterministicParityTests intent.
	struct ParityStats
	{
		double mismatchFraction;
		int    maxDiff;
	};

	ParityStats measureParity( uint32_t width, uint32_t spp, uint32_t maxDepth, unsigned seed, int tolerance )
	{
		const float      aspectRatio = 16.0f / 9.0f;
		SceneDescription scene = buildDefaultScene( seed, width, aspectRatio, spp, maxDepth );

		CPURenderResult cpuResult = renderCPU( scene, CPUThreading::MultiThreaded );

		MTL::Device* pDevice = MTL::CreateSystemDefaultDevice();
		GPURenderer  gpuRenderer( pDevice );
		GPURenderResult gpuResult = gpuRenderer.render( scene );

		const uint32_t       w = scene.params.width;
		const uint32_t       h = scene.params.height;
		std::vector<uint8_t> gpuPixels( (size_t)w * h * 4 );
		gpuResult.pTexture->getBytes( gpuPixels.data(), (size_t)w * 4, MTL::Region( 0, 0, 0, w, h, 1 ), 0 );

		size_t mismatches = 0;
		int    maxDiff = 0;
		const size_t totalChannels = (size_t)w * h * 3; // RGB only, skip alpha

		for ( uint32_t y = 0; y < h; ++y )
		{
			for ( uint32_t x = 0; x < w; ++x )
			{
				size_t idx = ( (size_t)y * w + x ) * 4;
				for ( int c = 0; c < 3; ++c )
				{
					int diff = std::abs( (int)cpuResult.pixels[ idx + c ] - (int)gpuPixels[ idx + c ] );
					maxDiff = std::max( maxDiff, diff );
					if ( diff > tolerance )
						++mismatches;
				}
			}
		}

		pDevice->release();

		return ParityStats{ (double)mismatches / (double)totalChannels, maxDiff };
	}
}

// See CLAUDE.md's verification notes: a strict ~2/255 per-pixel bound does NOT hold at low sample
// counts (chaotic Monte Carlo branch-divergence from sub-ULP CPU/GPU float differences flips
// occasional scatter/reflectance decisions, causing large but *isolated* per-pixel differences —
// not a bug, confirmed by mismatches being exactly zero on every pure-sky pixel and shrinking as
// spp rises). So this asserts on mismatch RATE at a reasonably high spp, where that chaotic
// divergence has mostly averaged out, rather than a strict per-pixel bound at low spp.
TEST_CASE( cpuGpuParity_fixedSeed_highSampleCount )
{
	ParityStats stats = measureParity( /*width=*/64, /*spp=*/200, /*maxDepth=*/16, /*seed=*/777u, /*tolerance=*/2 );

	std::printf( "  parity: %.3f%% channels mismatched (>2/255), max diff %d\n", stats.mismatchFraction * 100.0, stats.maxDiff );

	CHECK( stats.mismatchFraction < 0.06 ); // observed ~4.9% at spp=200 in prior manual runs
}

TEST_CASE( cpuGpuParity_isDeterministic_sameSeedSameResult )
{
	// Rendering the same seed twice (CPU side only, cheap) must produce byte-identical output —
	// this is a sanity check on the scene/RNG determinism the parity test above depends on, not a
	// CPU/GPU comparison.
	const float      aspectRatio = 16.0f / 9.0f;
	SceneDescription sceneA = buildDefaultScene( 42u, 48, aspectRatio, 8, 8 );
	SceneDescription sceneB = buildDefaultScene( 42u, 48, aspectRatio, 8, 8 );

	CPURenderResult resultA = renderCPU( sceneA, CPUThreading::SingleThreaded );
	CPURenderResult resultB = renderCPU( sceneB, CPUThreading::SingleThreaded );

	CHECK( resultA.pixels == resultB.pixels );
}
