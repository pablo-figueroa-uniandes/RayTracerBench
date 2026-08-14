#include "RayTraceCore.h"
#include "TestFramework.hpp"

namespace
{
	TransformGPU sphereTransform( simd_float3 center )
	{
		TransformGPU t;
		t.position = center;
		t.right = makeFloat3( 1.0f, 0.0f, 0.0f );
		t.up = makeFloat3( 0.0f, 1.0f, 0.0f );
		t.forward = makeFloat3( 0.0f, 0.0f, 1.0f );
		return t;
	}

	ShapeGPU sphereShape( float radius )
	{
		ShapeGPU s;
		s.type = SHAPE_SPHERE;
		s.radius = radius;
		s.baseHalfWidth = 0.0f;
		s.height = 0.0f;
		s.materialIndex = 0;
		return s;
	}

	// Axis-aligned pyramid Transform (identity basis): base centered at `basePosition`, apex
	// pointing straight up (+Y).
	TransformGPU pyramidTransform( simd_float3 basePosition )
	{
		return sphereTransform( basePosition ); // identity basis works for either shape
	}

	ShapeGPU pyramidShape( float baseHalfWidth, float height )
	{
		ShapeGPU s;
		s.type = SHAPE_PYRAMID;
		s.radius = 0.0f;
		s.baseHalfWidth = baseHalfWidth;
		s.height = height;
		s.materialIndex = 0;
		return s;
	}
}

TEST_CASE( hitSphere_hitsDeadOn )
{
	TransformGPU transform = sphereTransform( makeFloat3( 0.0f, 0.0f, -1.0f ) );
	ShapeGPU      shape = sphereShape( 0.5f );
	Ray           ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 0.5, 1e-5 );
	CHECK( result.record.frontFace );
	CHECK_NEAR( result.record.normal.z, 1.0, 1e-5 ); // ray hit the near face, normal points back at the ray
}

TEST_CASE( hitSphere_missesEntirely )
{
	TransformGPU transform = sphereTransform( makeFloat3( 10.0f, 10.0f, 10.0f ) );
	ShapeGPU      shape = sphereShape( 0.5f );
	Ray           ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( !result.hit );
}

TEST_CASE( hitSphere_respectsTMaxRange )
{
	// Sphere is hit, but only beyond tMax — should report no hit.
	TransformGPU transform = sphereTransform( makeFloat3( 0.0f, 0.0f, -100.0f ) );
	ShapeGPU      shape = sphereShape( 0.5f );
	Ray           ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult tooShort = hitSphere( transform, shape, ray, 0.001f, 10.0f );
	CHECK( !tooShort.hit );

	HitResult longEnough = hitSphere( transform, shape, ray, 0.001f, 200.0f );
	CHECK( longEnough.hit );
	CHECK_NEAR( longEnough.record.t, 99.5, 1e-4 );
}

TEST_CASE( hitSphere_originInsideSphere_reportsBackFace )
{
	// Ray origin is inside the sphere: the nearest positive root is the far side of the
	// sphere, and the outward normal should be flipped (frontFace == false) since it points the
	// same direction as the ray.
	TransformGPU transform = sphereTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = sphereShape( 1.0f );
	Ray           ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	HitResult result = hitSphere( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 1.0, 1e-5 );
	CHECK( !result.record.frontFace );
}

TEST_CASE( hitSphere_picksCloserOfTwoRoots )
{
	TransformGPU nearTransform = sphereTransform( makeFloat3( 0.0f, 0.0f, -2.0f ) );
	TransformGPU farTransform = sphereTransform( makeFloat3( 0.0f, 0.0f, -5.0f ) );
	ShapeGPU      shape = sphereShape( 0.5f );
	Ray           ray{ makeFloat3( 0.0f, 0.0f, 0.0f ), makeFloat3( 0.0f, 0.0f, -1.0f ) };

	// Simulate the scene-traversal loop rayColor() does: keep the closest hit across entities.
	HitResult nearHit = hitSphere( nearTransform, shape, ray, 0.001f, 1000.0f );
	HitResult farHit = hitSphere( farTransform, shape, ray, 0.001f, nearHit.hit ? nearHit.record.t : 1000.0f );

	CHECK( nearHit.hit );
	CHECK( !farHit.hit ); // far sphere is beyond the near sphere's t, so it must not register
	CHECK_NEAR( nearHit.record.t, 1.5, 1e-5 );
}

TEST_CASE( hitPyramid_hitsApexFromAbove )
{
	// Axis-aligned pyramid, base centered at origin, apex at (0, 1, 0). A ray straight down
	// through the apex's (x,z) should hit the apex tip almost immediately (t just under 1).
	TransformGPU transform = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = pyramidShape( /*baseHalfWidth=*/1.0f, /*height=*/1.0f );
	Ray           ray{ makeFloat3( 0.0f, 5.0f, 0.0f ), makeFloat3( 0.0f, -1.0f, 0.0f ) };

	HitResult result = hitPyramid( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 4.0, 1e-4 ); // travels from y=5 down to the apex at y=1
	CHECK( result.record.frontFace );
}

TEST_CASE( hitPyramid_hitsSideFaceWhenOffApexAxis )
{
	// Same pyramid; a ray straight down at x=0.9 (off the apex axis) enters through the sloped +X
	// side face rather than the apex — the "roof" at that (x,z) is lower than at the apex, exactly
	// as the +X face's plane equation (RayTraceCore.h) predicts: y <= height - (height/baseHalfWidth)*x
	// = 1 - 0.9 = 0.1 here, so the ray (descending from y=5) enters at y=0.1, i.e. t=4.9.
	TransformGPU transform = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = pyramidShape( /*baseHalfWidth=*/1.0f, /*height=*/1.0f );
	Ray           ray{ makeFloat3( 0.9f, 5.0f, 0.0f ), makeFloat3( 0.0f, -1.0f, 0.0f ) };

	HitResult result = hitPyramid( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 4.9, 1e-4 );
	CHECK( result.record.normal.x > 0.0f ); // +X side face, outward normal tilts toward +X
	CHECK( result.record.normal.y > 0.0f );
}

TEST_CASE( hitPyramid_hitsBaseFromBelow )
{
	// A ray approaching from underground, straight up through the pyramid's center: the base
	// plane (y=0) is the first face it crosses, well before it would ever reach a side face.
	TransformGPU transform = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = pyramidShape( /*baseHalfWidth=*/1.0f, /*height=*/1.0f );
	Ray           ray{ makeFloat3( 0.0f, -5.0f, 0.0f ), makeFloat3( 0.0f, 1.0f, 0.0f ) };

	HitResult result = hitPyramid( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK_NEAR( result.record.t, 5.0, 1e-4 );
	CHECK_NEAR( result.record.normal.y, -1.0, 1e-4 ); // base's outward normal points down
}

TEST_CASE( hitPyramid_missesEntirely )
{
	TransformGPU transform = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = pyramidShape( /*baseHalfWidth=*/1.0f, /*height=*/1.0f );
	Ray           ray{ makeFloat3( 10.0f, 5.0f, 10.0f ), makeFloat3( 0.0f, -1.0f, 0.0f ) };

	HitResult result = hitPyramid( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( !result.hit );
}

TEST_CASE( hitPyramid_sideNormalPointsOutwardAndUpward )
{
	// A ray fired horizontally into the pyramid's +X side face should be stopped by that face,
	// with an outward normal that has a positive X component (points away from the pyramid) and a
	// positive Y component (the side faces all slope outward from the apex) — matches the +X face
	// plane derived in RayTraceCore.h's comment: normal (height, halfWidth, 0).
	TransformGPU transform = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      shape = pyramidShape( /*baseHalfWidth=*/1.0f, /*height=*/1.0f );
	Ray           ray{ makeFloat3( 5.0f, 0.25f, 0.0f ), makeFloat3( -1.0f, 0.0f, 0.0f ) };

	HitResult result = hitPyramid( transform, shape, ray, 0.001f, 1000.0f );

	CHECK( result.hit );
	CHECK( result.record.normal.x > 0.0f );
	CHECK( result.record.normal.y > 0.0f );
}

TEST_CASE( hitEntity_dispatchesByShapeTag )
{
	// hitEntity() is the tagged-switch "collision system": confirm it reaches hitSphere() and
	// hitPyramid() for the matching tags rather than, say, always taking the sphere path.
	Ray downward{ makeFloat3( 0.0f, 5.0f, 0.0f ), makeFloat3( 0.0f, -1.0f, 0.0f ) };

	TransformGPU sphereT = sphereTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      sphereS = sphereShape( 1.0f );
	HitResult     sphereResult = hitEntity( sphereT, sphereS, downward, 0.001f, 1000.0f );
	CHECK( sphereResult.hit );
	CHECK_NEAR( sphereResult.record.t, 4.0, 1e-4 ); // sphere surface at y=1

	TransformGPU pyramidT = pyramidTransform( makeFloat3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU      pyramidS = pyramidShape( 1.0f, 1.0f );
	HitResult     pyramidResult = hitEntity( pyramidT, pyramidS, downward, 0.001f, 1000.0f );
	CHECK( pyramidResult.hit );
	CHECK_NEAR( pyramidResult.record.t, 4.0, 1e-4 ); // apex at y=1
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
