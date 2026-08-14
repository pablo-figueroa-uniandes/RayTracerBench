#include "RayTraceCore.h"
#include "TestFramework.hpp"

namespace
{
	SphereGPU makeSphere( simd_float3 center, float radius )
	{
		SphereGPU s;
		s.center = center;
		s.radius = radius;
		s.materialIndex = 0;
		return s;
	}
}

TEST_CASE( hitSphere_hitsDeadOn )
{
	SphereGPU sphere = makeSphere( makeFloat3( 0.0f, 0.0f, -1.0f ), 0.5f );
	Ray       ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( sphere, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 0.5, 1e-5 );
	CHECK( result.record.frontFace );
	CHECK_NEAR( result.record.normal.z, 1.0, 1e-5 ); // ray hit the near face, normal points back at the ray
}

TEST_CASE( hitSphere_missesEntirely )
{
	SphereGPU sphere = makeSphere( makeFloat3( 10.0f, 10.0f, 10.0f ), 0.5f );
	Ray       ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( sphere, ray, 0.001f, 1000.0f );

	CHECK( !result.hit );
}

TEST_CASE( hitSphere_respectsTMaxRange )
{
	// Sphere is hit, but only beyond tMax — should report no hit.
	SphereGPU sphere = makeSphere( makeFloat3( 0.0f, 0.0f, -100.0f ), 0.5f );
	Ray       ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult tooShort = hitSphere( sphere, ray, 0.001f, 10.0f );
	CHECK( !tooShort.hit );

	HitResult longEnough = hitSphere( sphere, ray, 0.001f, 200.0f );
	CHECK( longEnough.hit );
	CHECK_NEAR( longEnough.record.t, 99.5, 1e-4 );
}

TEST_CASE( hitSphere_originInsideSphere_reportsBackFace )
{
	// Ray origin is inside the sphere: the nearest positive root is the far side of the
	// sphere, and the outward normal should be flipped (frontFace == false) since it points the
	// same direction as the ray.
	SphereGPU sphere = makeSphere( makeFloat3( 0.0f, 0.0f, 0.0f ), 1.0f );
	Ray       ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( sphere, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 1.0, 1e-5 );
	CHECK( !result.record.frontFace );
}

TEST_CASE( hitSphere_picksCloserOfTwoRoots )
{
	SphereGPU near = makeSphere( makeFloat3( 0.0f, 0.0f, -2.0f ), 0.5f );
	SphereGPU far = makeSphere( makeFloat3( 0.0f, 0.0f, -5.0f ), 0.5f );
	Ray       ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	// Simulate the scene-traversal loop rayColor() does: keep the closest hit across spheres.
	HitResult nearHit = hitSphere( near, ray, 0.001f, 1000.0f );
	HitResult farHit = hitSphere( far, ray, 0.001f, nearHit.hit ? nearHit.record.t : 1000.0f );

	CHECK( nearHit.hit );
	CHECK( !farHit.hit ); // far sphere is beyond the near sphere's t, so it must not register
	CHECK_NEAR( nearHit.record.t, 1.5, 1e-5 );
}

TEST_CASE( reflect3_mirrorsAboutNormal )
{
	simd_float3 incoming = makeFloat3( 1.0f, -1.0f, 0.0f );
	simd_float3 normal = makeFloat3( 0.0f, 1.0f, 0.0f );

	simd_float3 reflected = reflect3( incoming, normal );

	CHECK_NEAR( reflected.x, 1.0, 1e-5 );
	CHECK_NEAR( reflected.y, 1.0, 1e-5 );
	CHECK_NEAR( reflected.z, 0.0, 1e-5 );
}

TEST_CASE( pcgHash_isDeterministicAndVaries )
{
	uint32_t a1 = pcgHash( 12345u );
	uint32_t a2 = pcgHash( 12345u );
	uint32_t b = pcgHash( 54321u );

	CHECK( a1 == a2 ); // same input -> same output, every time
	CHECK( a1 != b );  // different input -> (overwhelmingly likely) different output
}
