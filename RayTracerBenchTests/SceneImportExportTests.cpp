#include "Base64.hpp"
#include "Scene.hpp"
#include "SceneExporter.hpp"
#include "SceneImporter.hpp"
#include "TestFramework.hpp"

#include <cmath>

namespace
{
	constexpr float kAspectRatio = 16.0f / 9.0f;

	float maxComponentDiff( simd_float3 a, simd_float3 b )
	{
		return std::max( { std::fabs( a.x - b.x ), std::fabs( a.y - b.y ), std::fabs( a.z - b.z ) } );
	}

	// Every entity's Transform/Shape/Material, reconstructed from `loaded`, should match `original`
	// to float-text-precision (a few times float32 epsilon — see SceneExporter.cpp's setprecision(9)
	// comments) — this is the regression this test exists to catch: SceneExporter.cpp originally
	// shipped a bug (a bulk memcpy over simd_float3's SIMD padding) that scrambled glTF vertex data
	// entirely, which this same comparison caught by decoding a real export and finding its bounding
	// box didn't match the source mesh at all — see the fix's commit for the full story.
	void checkRoundTrip( const SceneDescription& original, const SceneDescription& loaded, float eps )
	{
		CHECK( loaded.transforms.size() == original.transforms.size() );
		CHECK( loaded.shapes.size() == original.shapes.size() );

		for ( size_t e = 0; e < original.transforms.size(); ++e )
		{
			const TransformGPU& ot = original.transforms[ e ];
			const ShapeGPU&     os = original.shapes[ e ];
			const MaterialGPU&  om = original.materials[ os.materialIndex ];
			const TransformGPU& lt = loaded.transforms[ e ];
			const ShapeGPU&     ls = loaded.shapes[ e ];
			const MaterialGPU&  lm = loaded.materials[ ls.materialIndex ];

			RT_CHECK_MESSAGE( os.type == ls.type, "entity " << e << " shape type mismatch" );
			RT_CHECK_MESSAGE( om.type == lm.type, "entity " << e << " material type mismatch" );
			RT_CHECK_MESSAGE( maxComponentDiff( ot.position, lt.position ) <= eps, "entity " << e << " position drifted" );

			if ( os.type == SHAPE_SPHERE )
				RT_CHECK_MESSAGE( std::fabs( os.radius - ls.radius ) <= eps, "entity " << e << " radius drifted" );
			else
			{
				RT_CHECK_MESSAGE( std::fabs( os.height - ls.height ) <= eps, "entity " << e << " height drifted" );
				RT_CHECK_MESSAGE( std::fabs( os.baseHalfWidth - ls.baseHalfWidth ) <= eps, "entity " << e << " baseHalfWidth drifted" );
			}

			RT_CHECK_MESSAGE( maxComponentDiff( om.albedo, lm.albedo ) <= eps, "entity " << e << " albedo drifted" );
			RT_CHECK_MESSAGE( std::fabs( om.fuzz - lm.fuzz ) <= eps, "entity " << e << " fuzz drifted" );
			RT_CHECK_MESSAGE( std::fabs( om.ir - lm.ir ) <= eps, "entity " << e << " ir drifted" );
		}
	}
}

TEST_CASE( base64_roundTrip_recoversOriginalBytesAtEveryLengthMod3 )
{
	// base64 pads differently depending on (length % 3) — exercise all three cases, plus empty.
	for ( size_t len : { (size_t)0, (size_t)1, (size_t)2, (size_t)3, (size_t)4, (size_t)5, (size_t)6, (size_t)97 } )
	{
		std::vector<uint8_t> original( len );
		for ( size_t i = 0; i < len; ++i )
			original[ i ] = (uint8_t)( ( i * 37 + 11 ) & 0xFF );

		std::string           encoded = base64Encode( original.data(), original.size() );
		std::vector<uint8_t>  decoded = base64Decode( encoded );

		RT_CHECK_MESSAGE( decoded == original, "length " << len << " round-trip mismatch (encoded=" << encoded << ")" );
	}
}

TEST_CASE( sceneRoundTrip_gltf_reconstructsEntitiesPositionsAndMaterials )
{
	// A small, fast scene (32px width keeps buildDefaultScene's ~490 entities but the export/import
	// cheap) at a fixed seed, exported then immediately re-imported — the same pattern as
	// DeterministicParityTests' fixed-seed determinism checks.
	SceneDescription scene = buildDefaultScene( 4242u, 32, kAspectRatio, 8, 6, false );

	std::string        stem = "test_gltf_roundtrip_scene";
	SceneExportResult  exportResult = exportSceneAsGLTF( scene, stem );
	CHECK( exportResult.ok );
	CHECK( exportResult.writtenFilePaths.size() == 1 );

	SceneImportResult importResult = importSceneFromGLTF( exportResult.writtenFilePaths[ 0 ], 32, kAspectRatio, 8, 6, 4242u );
	RT_CHECK_MESSAGE( importResult.ok, importResult.message );

	// A few times float32 epsilon at these coordinate magnitudes (positions up to ~1000) — see
	// SceneExporter.cpp's setprecision(17)/(9) comments for why this isn't exact to the last bit.
	checkRoundTrip( scene, importResult.scene, 1.0e-3f );
}

TEST_CASE( sceneRoundTrip_obj_reconstructsEntitiesPositionsAndMaterials )
{
	SceneDescription scene = buildDefaultScene( 4242u, 32, kAspectRatio, 8, 6, false );

	std::string        stem = "test_obj_roundtrip_scene";
	SceneExportResult  exportResult = exportSceneAsOBJ( scene, stem );
	CHECK( exportResult.ok );
	CHECK( exportResult.writtenFilePaths.size() == 2 ); // .obj + .mtl

	SceneImportResult importResult = importSceneFromOBJ( exportResult.writtenFilePaths[ 0 ], 32, kAspectRatio, 8, 6, 4242u );
	RT_CHECK_MESSAGE( importResult.ok, importResult.message );

	checkRoundTrip( scene, importResult.scene, 1.0e-3f );
}

TEST_CASE( sceneRoundTrip_floatingScene_alsoReconstructsExactly )
{
	// The floating-field-spheres variant exercises non-ground-resting Y positions — a distinct
	// geometry distribution from the two tests above, which both use floating=false.
	SceneDescription scene = buildDefaultScene( 99u, 32, kAspectRatio, 8, 6, true );

	std::string        stem = "test_floating_roundtrip_scene";
	SceneExportResult  exportResult = exportSceneAsGLTF( scene, stem );
	CHECK( exportResult.ok );

	SceneImportResult importResult = importSceneFromGLTF( exportResult.writtenFilePaths[ 0 ], 32, kAspectRatio, 8, 6, 99u );
	RT_CHECK_MESSAGE( importResult.ok, importResult.message );

	checkRoundTrip( scene, importResult.scene, 1.0e-3f );
}
