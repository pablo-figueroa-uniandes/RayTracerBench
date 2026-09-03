// Metal private-implementation macros are already defined once in DeterministicParityTests.cpp —
// see that file's header comment; must not be redefined here.

#include "EntityMesh.hpp"
#include "RasterRenderer.hpp"
#include "Scene.hpp"
#include "TestFramework.hpp"

#include <vector>

namespace
{
	// Renders the default scene through the real RasterRenderer (not a reimplementation) at a small
	// fixed size, headless — no window needed, matching DeterministicParityTests.cpp's pattern.
	RasterRenderResult renderDefaultScene( MTL::Device* pDevice, RasterRenderer& renderer, SceneDescription& outScene )
	{
		const float aspectRatio = 16.0f / 9.0f;
		outScene = buildDefaultScene( /*seed=*/777u, /*width=*/64, aspectRatio, /*spp=*/1, /*maxDepth=*/1 );
		return renderer.render( outScene );
	}
}

// Rasterized output is *expected* to look different from the ray-traced output (no shadows/GI/
// reflection — see CLAUDE.md's Project status note on this render path's scope), so this is
// deliberately not a pixel-parity test against the CPU/GPU ray tracers. It's a smoke test that the
// real pipeline actually ran: correct texture dimensions, and real color variation rather than a
// blank/uniform frame (which would indicate a broken matrix, everything culled, or similar).
TEST_CASE( rasterRenderer_producesNonBlankImageAtRequestedSize )
{
	MTL::Device* pDevice = MTL::CreateSystemDefaultDevice();
	RasterRenderer renderer( pDevice );

	SceneDescription   scene;
	RasterRenderResult result = renderDefaultScene( pDevice, renderer, scene );

	CHECK( result.pTexture->width() == scene.params.width );
	CHECK( result.pTexture->height() == scene.params.height );

	std::vector<uint8_t> pixels( (size_t)scene.params.width * scene.params.height * 4 );
	result.pTexture->getBytes( pixels.data(), (size_t)scene.params.width * 4,
		MTL::Region( 0, 0, 0, scene.params.width, scene.params.height, 1 ), 0 );

	uint8_t firstR = pixels[ 0 ];
	uint8_t firstG = pixels[ 1 ];
	uint8_t firstB = pixels[ 2 ];
	bool    hasVariation = false;
	for ( size_t i = 0; i + 3 < pixels.size(); i += 4 )
	{
		if ( pixels[ i ] != firstR || pixels[ i + 1 ] != firstG || pixels[ i + 2 ] != firstB )
		{
			hasVariation = true;
			break;
		}
	}
	CHECK( hasVariation );

	pDevice->release();
}

// The combined vertex/index buffer's triangle count should equal the sum of buildEntityMesh()'s own
// triangle counts across every entity — catches an indexing/offset bug in the buffer-flattening
// loop rather than a rendering bug.
TEST_CASE( rasterRenderer_triangleCountMatchesEntityMeshSum )
{
	MTL::Device* pDevice = MTL::CreateSystemDefaultDevice();
	RasterRenderer renderer( pDevice );

	SceneDescription   scene;
	RasterRenderResult result = renderDefaultScene( pDevice, renderer, scene );

	uint32_t expectedTriangles = 0;
	for ( size_t e = 0; e < scene.transforms.size(); ++e )
	{
		MeshData mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
		expectedTriangles += (uint32_t)( mesh.indices.size() / 3 );
	}

	CHECK( result.triangleCount == expectedTriangles );
	CHECK( result.triangleCount > 0 );

	pDevice->release();
}
