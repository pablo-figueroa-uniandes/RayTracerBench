#include "EntityMesh.hpp"
#include "TestFramework.hpp"

#include <cmath>

namespace
{
	float dot3( simd_float3 a, simd_float3 b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	TransformGPU identityTransformAt( simd_float3 position )
	{
		TransformGPU t;
		t.position = position;
		t.right = simd_make_float3( 1.0f, 0.0f, 0.0f );
		t.up = simd_make_float3( 0.0f, 1.0f, 0.0f );
		t.forward = simd_make_float3( 0.0f, 0.0f, 1.0f );
		return t;
	}

	// True if every triangle in `mesh` winds so its face normal (via cross product) points away
	// from `interiorPoint` — the same check used to hand-derive EntityMesh.cpp's winding orders,
	// re-run here so a future edit that breaks winding fails a test instead of only looking wrong
	// in a 3D viewer.
	bool allTrianglesFaceAwayFrom( const MeshData& mesh, simd_float3 interiorPoint )
	{
		for ( size_t i = 0; i + 2 < mesh.indices.size(); i += 3 )
		{
			simd_float3 a = mesh.positions[ mesh.indices[ i ] ];
			simd_float3 b = mesh.positions[ mesh.indices[ i + 1 ] ];
			simd_float3 c = mesh.positions[ mesh.indices[ i + 2 ] ];
			simd_float3 faceNormal = simd_cross( b - a, c - a );
			simd_float3 centroid = ( a + b + c ) / 3.0f;
			if ( dot3( faceNormal, centroid - interiorPoint ) <= 0.0f )
				return false;
		}
		return true;
	}
}

TEST_CASE( buildEntityMesh_sphere_hasNoDegenerateTriangles_andFacesOutward )
{
	simd_float3 center = simd_make_float3( 1.0f, 2.0f, 3.0f );
	float       radius = 5.0f;

	TransformGPU transform = identityTransformAt( center );
	ShapeGPU     shape;
	shape.type = SHAPE_SPHERE;
	shape.radius = radius;
	shape.baseHalfWidth = 0.0f;
	shape.height = 0.0f;
	shape.materialIndex = 0;

	MeshData mesh = buildEntityMesh( transform, shape );

	CHECK( !mesh.positions.empty() );
	CHECK( mesh.indices.size() % 3 == 0 );
	CHECK( allTrianglesFaceAwayFrom( mesh, center ) );

	// Every position should lie on the sphere's surface (within float tolerance).
	for ( const simd_float3& p : mesh.positions )
	{
		simd_float3 offset = p - center;
		CHECK_NEAR( std::sqrt( dot3( offset, offset ) ), radius, 1e-3 );
	}
}

TEST_CASE( buildEntityMesh_pyramid_hasExactlySixFacetedTriangles_andFacesOutward )
{
	TransformGPU transform = identityTransformAt( simd_make_float3( 0.0f, 0.0f, 0.0f ) );
	ShapeGPU     shape;
	shape.type = SHAPE_PYRAMID;
	shape.radius = 0.0f;
	shape.baseHalfWidth = 1.0f;
	shape.height = 1.0f;
	shape.materialIndex = 0;

	MeshData mesh = buildEntityMesh( transform, shape );

	// Faceted (no shared vertices across triangles): 6 triangles * 3 unique vertices each.
	CHECK( mesh.indices.size() == 18 );
	CHECK( mesh.positions.size() == 18 );
	CHECK( mesh.normals.size() == 18 );

	// A point just above the base center is inside the pyramid for any height > 0.4.
	CHECK( allTrianglesFaceAwayFrom( mesh, simd_make_float3( 0.0f, 0.2f, 0.0f ) ) );
}

TEST_CASE( buildEntityMesh_pyramid_respectsOrientationBasis )
{
	// A pyramid rotated 90 degrees so its local "up" (apex direction) points along world +X
	// instead of +Y — the apex should land at world (1,0,0) instead of (0,1,0).
	TransformGPU transform;
	transform.position = simd_make_float3( 0.0f, 0.0f, 0.0f );
	transform.right = simd_make_float3( 0.0f, -1.0f, 0.0f );
	transform.up = simd_make_float3( 1.0f, 0.0f, 0.0f );
	transform.forward = simd_make_float3( 0.0f, 0.0f, 1.0f );

	ShapeGPU shape;
	shape.type = SHAPE_PYRAMID;
	shape.radius = 0.0f;
	shape.baseHalfWidth = 0.5f;
	shape.height = 1.0f;
	shape.materialIndex = 0;

	MeshData mesh = buildEntityMesh( transform, shape );

	bool foundApex = false;
	for ( const simd_float3& p : mesh.positions )
	{
		simd_float3 delta = p - simd_make_float3( 1.0f, 0.0f, 0.0f );
		if ( dot3( delta, delta ) < 1e-6f )
			foundApex = true;
	}
	CHECK( foundApex );
}
