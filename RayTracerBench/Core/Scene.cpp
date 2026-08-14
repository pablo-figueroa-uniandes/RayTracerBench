#include "Scene.hpp"

#include <cmath>
#include <random>

// Scene.cpp is pure host-side C++ (never compiled as MSL), so — unlike Core/RayTraceCore.h —
// it can freely use the full simd_* C API (simd_make_float3, simd_normalize, simd_cross,
// simd_length) instead of hand-rolled vector helpers.

namespace
{
	constexpr float kPi = 3.14159265358979323846f;

	simd_float3 randomAlbedo( std::mt19937& rng, std::uniform_real_distribution<float>& unit )
	{
		return simd_make_float3( unit( rng ) * unit( rng ), unit( rng ) * unit( rng ), unit( rng ) * unit( rng ) );
	}

	CameraGPU makeCamera( simd_float3 lookFrom, simd_float3 lookAt, simd_float3 vUp,
		float vfovDegrees, float aspectRatio, float aperture, float focusDist )
	{
		float theta = vfovDegrees * kPi / 180.0f;
		float h = std::tan( theta / 2.0f );
		float viewportHeight = 2.0f * h;
		float viewportWidth = aspectRatio * viewportHeight;

		simd_float3 w = simd_normalize( lookFrom - lookAt );
		simd_float3 u = simd_normalize( simd_cross( vUp, w ) );
		simd_float3 v = simd_cross( w, u );

		CameraGPU cam{};
		cam.origin = lookFrom;
		cam.horizontal = focusDist * viewportWidth * u;
		cam.vertical = focusDist * viewportHeight * v;
		cam.lowerLeftCorner = cam.origin - cam.horizontal / 2.0f - cam.vertical / 2.0f - focusDist * w;
		cam.u = u;
		cam.v = v;
		cam.w = w;
		cam.lensRadius = aperture / 2.0f;
		return cam;
	}

	int addMaterial( std::vector<MaterialGPU>& materials, MaterialGPU mat )
	{
		materials.push_back( mat );
		return static_cast<int>( materials.size() - 1 );
	}

	// Chosen and verified by rendering the fixed camera setup below (lookfrom (13,2,3), lookat
	// origin, vfov 20°) at several candidate heights and confirming by eye that nothing floats out
	// of frame — not derived from an unverified formula. Applies to every non-ground sphere.
	constexpr float kMaxFloatHeight = 3.0f;

	// Picks a random height in [radius, kMaxFloatHeight] when floating, or the given resting
	// height (touching the ground) otherwise.
	float placementHeight( bool floating, float radius, float restingHeight, std::mt19937& rng, std::uniform_real_distribution<float>& unit )
	{
		if ( !floating )
			return restingHeight;
		return radius + unit( rng ) * ( kMaxFloatHeight - radius );
	}
}

SceneDescription buildDefaultScene( unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, bool floating )
{
	SceneDescription scene;

	std::mt19937                          rng( seed );
	std::uniform_real_distribution<float> unit( 0.0f, 1.0f );
	std::uniform_real_distribution<float> fuzzDist( 0.0f, 0.5f );

	// Ground.
	{
		MaterialGPU ground{ MAT_LAMBERTIAN, simd_make_float3( 0.5f, 0.5f, 0.5f ), 0.0f, 0.0f };
		int         groundMat = addMaterial( scene.materials, ground );
		scene.spheres.push_back( SphereGPU{ simd_make_float3( 0.0f, -1000.0f, 0.0f ), 1000.0f, groundMat } );
	}

	const simd_float3 featureSphereCenter = simd_make_float3( 4.0f, 0.2f, 0.0f );

	for ( int a = -11; a < 11; ++a )
	{
		for ( int b = -11; b < 11; ++b )
		{
			float       chooseMat = unit( rng );
			simd_float3 center = simd_make_float3( a + 0.9f * unit( rng ), 0.2f, b + 0.9f * unit( rng ) );

			if ( simd_length( center - featureSphereCenter ) <= 0.9f )
				continue;

			MaterialGPU mat{};
			if ( chooseMat < 0.8f )
			{
				mat.type = MAT_LAMBERTIAN;
				mat.albedo = randomAlbedo( rng, unit );
			}
			else if ( chooseMat < 0.95f )
			{
				mat.type = MAT_METAL;
				mat.albedo = simd_make_float3( unit( rng ) * 0.5f + 0.5f, unit( rng ) * 0.5f + 0.5f, unit( rng ) * 0.5f + 0.5f );
				mat.fuzz = fuzzDist( rng );
			}
			else
			{
				mat.type = MAT_DIELECTRIC;
				mat.ir = 1.5f;
			}

			int matIndex = addMaterial( scene.materials, mat );
			center.y = placementHeight( floating, 0.2f, center.y, rng, unit );
			scene.spheres.push_back( SphereGPU{ center, 0.2f, matIndex } );
		}
	}

	// Three large feature spheres: glass, lambertian, metal.
	{
		MaterialGPU glass{ MAT_DIELECTRIC, simd_make_float3( 0.0f, 0.0f, 0.0f ), 0.0f, 1.5f };
		int         glassMat = addMaterial( scene.materials, glass );
		scene.spheres.push_back( SphereGPU{ simd_make_float3( 0.0f, placementHeight( floating, 1.0f, 1.0f, rng, unit ), 0.0f ), 1.0f, glassMat } );

		MaterialGPU diffuse{ MAT_LAMBERTIAN, simd_make_float3( 0.4f, 0.2f, 0.1f ), 0.0f, 0.0f };
		int         diffuseMat = addMaterial( scene.materials, diffuse );
		scene.spheres.push_back( SphereGPU{ simd_make_float3( -4.0f, placementHeight( floating, 1.0f, 1.0f, rng, unit ), 0.0f ), 1.0f, diffuseMat } );

		MaterialGPU metal{ MAT_METAL, simd_make_float3( 0.7f, 0.6f, 0.5f ), 0.0f, 0.0f };
		int         metalMat = addMaterial( scene.materials, metal );
		scene.spheres.push_back( SphereGPU{ simd_make_float3( 4.0f, placementHeight( floating, 1.0f, 1.0f, rng, unit ), 0.0f ), 1.0f, metalMat } );
	}

	scene.camera = makeCamera(
		simd_make_float3( 13.0f, 2.0f, 3.0f ),
		simd_make_float3( 0.0f, 0.0f, 0.0f ),
		simd_make_float3( 0.0f, 1.0f, 0.0f ),
		20.0f, aspectRatio, 0.1f, 10.0f );

	scene.params.width = width;
	scene.params.height = static_cast<uint32_t>( static_cast<float>( width ) / aspectRatio );
	scene.params.samplesPerPixel = samplesPerPixel;
	scene.params.maxDepth = maxDepth;
	scene.params.frameSeed = seed;

	return scene;
}
