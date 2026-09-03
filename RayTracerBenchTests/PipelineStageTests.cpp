// Metal private-implementation macros are already defined once in DeterministicParityTests.cpp —
// see that file's header comment; must not be redefined here.

#include "PipelineStageRenderer.hpp"
#include "Scene.hpp"
#include "TestFramework.hpp"

#include <cmath>

namespace
{
	constexpr float kAspectRatio = 16.0f / 9.0f;

	TransformGPU identityTransformAt( simd_float3 position )
	{
		TransformGPU t;
		t.position = position;
		t.right = simd_make_float3( 1.0f, 0.0f, 0.0f );
		t.up = simd_make_float3( 0.0f, 1.0f, 0.0f );
		t.forward = simd_make_float3( 0.0f, 0.0f, 1.0f );
		return t;
	}

	// A single sphere entity, appended to `scene` with its own material.
	void addSphere( SceneDescription& scene, simd_float3 center, float radius )
	{
		MaterialGPU mat{ MAT_LAMBERTIAN, simd_make_float3( 0.5f, 0.5f, 0.5f ), 0.0f, 0.0f };
		scene.materials.push_back( mat );
		int matIndex = (int)scene.materials.size() - 1;

		scene.transforms.push_back( identityTransformAt( center ) );
		scene.shapes.push_back( ShapeGPU{ SHAPE_SPHERE, radius, 0.0f, 0.0f, matIndex } );
	}
}

// The image-plane rectangle's own 4 corners are fixed points of their own projection (they already
// lie on the plane), and a point straight down the camera's forward axis lands at the plane's
// center — the two sanity checks projectOntoPlane()'s "objects deformed onto the image plane" visual
// depends on being correct.
TEST_CASE( projectOntoPlane_cornersAreFixedPoints_andForwardAxisHitsCenter )
{
	SceneDescription scene = buildDefaultScene( 1234u, 64, kAspectRatio, 1, 1 );
	const CameraGPU& cam = scene.camera;

	simd_float3 c00 = cam.lowerLeftCorner;
	simd_float3 c10 = cam.lowerLeftCorner + cam.horizontal;
	simd_float3 c01 = cam.lowerLeftCorner + cam.vertical;
	simd_float3 c11 = cam.lowerLeftCorner + cam.horizontal + cam.vertical;

	for ( simd_float3 corner : { c00, c10, c01, c11 } )
	{
		simd_float3 projected = projectOntoPlane( cam.origin, corner, c00, cam.w );
		CHECK_NEAR( projected.x, corner.x, 1e-3 );
		CHECK_NEAR( projected.y, corner.y, 1e-3 );
		CHECK_NEAR( projected.z, corner.z, 1e-3 );
	}

	simd_float3 viewportCenter = c00 + ( cam.horizontal + cam.vertical ) * 0.5f;
	simd_float3 pointOnForwardAxis = cam.origin - cam.w * 5.0f; // "forward" is -w
	simd_float3 projectedCenter = projectOntoPlane( cam.origin, pointOnForwardAxis, c00, cam.w );
	CHECK_NEAR( projectedCenter.x, viewportCenter.x, 1e-3 );
	CHECK_NEAR( projectedCenter.y, viewportCenter.y, 1e-3 );
	CHECK_NEAR( projectedCenter.z, viewportCenter.z, 1e-3 );
}

// This is this session's Unity-import lesson (see CLAUDE.md), encoded as a permanent regression
// check: a scene with one radius-1000 sphere and a small cluster must not let the giant one dictate
// how an external observer camera frames the scene.
TEST_CASE( collectFramingPoints_excludesOversizedEntities_fromFramingBounds )
{
	SceneDescription scene;
	addSphere( scene, simd_make_float3( 0.0f, -1000.0f, 0.0f ), 1000.0f ); // the "ground sphere"
	addSphere( scene, simd_make_float3( 1.0f, 0.2f, 1.0f ), 0.2f );
	addSphere( scene, simd_make_float3( -1.0f, 0.2f, -1.0f ), 0.2f );
	addSphere( scene, simd_make_float3( 1.0f, 0.2f, -1.0f ), 0.2f );

	std::vector<simd_float3> points = collectFramingPoints( scene );
	CHECK( points.size() == 3 ); // the giant sphere's center is excluded, the small cluster isn't

	FramingBounds bounds = fitFramingSphere( points );
	CHECK( bounds.radius < 10.0f ); // would be ~1000 if the giant sphere weren't excluded
}

// The line-soup's vertex count should exactly match ringSegments*2 per sphere + 36 (18 edges * 2
// vertices) per pyramid, and every sphere-ring point should sit at exactly that sphere's radius
// from its center — catches a wrong ring-construction formula without needing a GPU readback.
TEST_CASE( buildSceneWireframe_vertexCountAndSphereRingRadius )
{
	SceneDescription scene;
	addSphere( scene, simd_make_float3( 0.0f, 0.0f, 0.0f ), 2.0f );
	addSphere( scene, simd_make_float3( 5.0f, 0.0f, 0.0f ), 0.5f );

	MaterialGPU pyramidMat{ MAT_LAMBERTIAN, simd_make_float3( 0.5f, 0.5f, 0.5f ), 0.0f, 0.0f };
	scene.materials.push_back( pyramidMat );
	TransformGPU pyramidTransform = identityTransformAt( simd_make_float3( -5.0f, 0.0f, 0.0f ) );
	scene.transforms.push_back( pyramidTransform );
	scene.shapes.push_back( ShapeGPU{ SHAPE_PYRAMID, 0.0f, 0.6f, 1.2f, (int)scene.materials.size() - 1 } );

	std::vector<LineVertex> lines = buildSceneWireframe( scene );

	size_t expected = 2 * (size_t)kSphereRingSegments * 2 /* two spheres */ + 36 /* one pyramid */;
	CHECK( lines.size() == expected );

	for ( size_t i = 0; i < (size_t)kSphereRingSegments * 2; ++i )
	{
		float dist = simd_length( lines[ i ].position - simd_make_float3( 0.0f, 0.0f, 0.0f ) );
		CHECK_NEAR( dist, 2.0, 1e-3 );
	}
}
