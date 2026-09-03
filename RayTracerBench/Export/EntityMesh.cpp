#include "EntityMesh.hpp"

#include <cmath>

// Pure host-side C++ (never compiled as MSL), like Core/Scene.cpp — free to use the full simd_*
// C API directly instead of RayTraceCore.h's hand-rolled portable helpers.

namespace
{
	constexpr float kPi = 3.14159265358979323846f;

	// Tessellation density for exported spheres (kSphereLatSegments/kSphereLonSegments, declared in
	// EntityMesh.hpp so SceneImporter.cpp can share them): no native sphere primitive exists in
	// either glTF or OBJ, so every sphere (including the giant ground sphere) is approximated as a
	// UV grid. Chosen as a size/smoothness balance after estimating the full scene's export size at
	// a few candidate densities (see CLAUDE.md) — the ~480-sphere randomized field dominates
	// total triangle count, so doubling this from 8x12 would roughly double the file size for a
	// visually marginal smoothness gain at typical viewing distance.

	// A smooth-shaded UV sphere: shared vertices at each (lat,lon) grid point with an outward
	// radial normal, matching how a sphere is actually shaded (no hard edges to preserve).
	MeshData buildSphereMesh( simd_float3 center, float radius )
	{
		MeshData mesh;
		const int latN = kSphereLatSegments;
		const int lonN = kSphereLonSegments;

		mesh.positions.reserve( ( latN + 1 ) * ( lonN + 1 ) );
		mesh.normals.reserve( ( latN + 1 ) * ( lonN + 1 ) );

		for ( int lat = 0; lat <= latN; ++lat )
		{
			float theta = -kPi / 2.0f + kPi * (float)lat / (float)latN; // -90..+90 degrees
			float sinTheta = std::sin( theta );
			float cosTheta = std::cos( theta );

			for ( int lon = 0; lon <= lonN; ++lon )
			{
				float       phi = 2.0f * kPi * (float)lon / (float)lonN;
				simd_float3 dir = simd_make_float3( cosTheta * std::cos( phi ), sinTheta, cosTheta * std::sin( phi ) );
				mesh.normals.push_back( dir );
				mesh.positions.push_back( center + radius * dir );
			}
		}

		auto vertIndex = [ lonN ]( int lat, int lon ) { return (uint32_t)( lat * ( lonN + 1 ) + lon ); };

		// Two triangles per grid quad; winding chosen so cross(v10-v00, v11-v00) etc. point
		// outward (verified in RayTracerBenchTests/EntityMeshTests.cpp against a known point). At
		// lat==0 every longitude collapses to the south pole (v00==v01), and at lat==latN-1 every
		// longitude collapses to the north pole (v10==v11) — the triangle that would use both
		// collapsed vertices is zero-area, so it's skipped rather than emitted as useless
		// degenerate geometry; the other triangle in that quad (touching the pole only once) is
		// real and still emitted.
		for ( int lat = 0; lat < latN; ++lat )
		{
			for ( int lon = 0; lon < lonN; ++lon )
			{
				uint32_t v00 = vertIndex( lat, lon );
				uint32_t v01 = vertIndex( lat, lon + 1 );
				uint32_t v10 = vertIndex( lat + 1, lon );
				uint32_t v11 = vertIndex( lat + 1, lon + 1 );

				if ( lat != latN - 1 ) // degenerate here: v10 == v11 (north pole)
				{
					mesh.indices.push_back( v00 );
					mesh.indices.push_back( v10 );
					mesh.indices.push_back( v11 );
				}
				if ( lat != 0 ) // degenerate here: v00 == v01 (south pole)
				{
					mesh.indices.push_back( v00 );
					mesh.indices.push_back( v11 );
					mesh.indices.push_back( v01 );
				}
			}
		}

		return mesh;
	}

	// Appends one flat-shaded triangle (its own 3 vertices, not shared with any other triangle,
	// since the pyramid's edges are meant to look sharp rather than smoothed).
	void addFlatTriangle( MeshData& mesh, simd_float3 a, simd_float3 b, simd_float3 c )
	{
		simd_float3 normal = simd_normalize( simd_cross( b - a, c - a ) );
		uint32_t    base = (uint32_t)mesh.positions.size();

		mesh.positions.push_back( a );
		mesh.positions.push_back( b );
		mesh.positions.push_back( c );
		mesh.normals.push_back( normal );
		mesh.normals.push_back( normal );
		mesh.normals.push_back( normal );

		mesh.indices.push_back( base );
		mesh.indices.push_back( base + 1 );
		mesh.indices.push_back( base + 2 );
	}

	// An exact (not tessellated) faceted mesh: the same 5 local vertices (apex + 4 base corners,
	// in the same corner order C0..C3) RayTraceCore.h's hitPyramidLocal() derives its 5 half-space
	// planes from, transformed to world space via the Transform component's orthonormal basis
	// (world = position + x*right + y*up + z*forward — the same convention hitPyramid() uses).
	//
	// Winding order for the 4 side faces, triangle(C[i], C[(i+1)%4], apex), was derived by hand
	// (cross(C[i+1]-C[i], apex-C[i]) checked against each face's known outward normal from
	// RayTraceCore.h's plane-equation comment — e.g. the +X face's (height, halfWidth, 0)) and is
	// re-verified in RayTracerBenchTests/EntityMeshTests.cpp rather than trusted by inspection
	// alone. The base is split into triangles (C0,C2,C1) and (C0,C3,C2), whose outward normal is
	// (0,-1,0) by the same derivation.
	MeshData buildPyramidMesh( TransformGPU transform, float baseHalfWidth, float height )
	{
		MeshData mesh;

		simd_float3 localApex = simd_make_float3( 0.0f, height, 0.0f );
		simd_float3 localCorners[ 4 ] = {
			simd_make_float3( baseHalfWidth, 0.0f, baseHalfWidth ),
			simd_make_float3( baseHalfWidth, 0.0f, -baseHalfWidth ),
			simd_make_float3( -baseHalfWidth, 0.0f, -baseHalfWidth ),
			simd_make_float3( -baseHalfWidth, 0.0f, baseHalfWidth ),
		};

		auto toWorld = [ & ]( simd_float3 local ) {
			return transform.position + local.x * transform.right + local.y * transform.up + local.z * transform.forward;
		};

		simd_float3 apex = toWorld( localApex );
		simd_float3 corners[ 4 ];
		for ( int i = 0; i < 4; ++i )
			corners[ i ] = toWorld( localCorners[ i ] );

		for ( int i = 0; i < 4; ++i )
			addFlatTriangle( mesh, corners[ i ], corners[ ( i + 1 ) % 4 ], apex );

		addFlatTriangle( mesh, corners[ 0 ], corners[ 2 ], corners[ 1 ] );
		addFlatTriangle( mesh, corners[ 0 ], corners[ 3 ], corners[ 2 ] );

		return mesh;
	}
}

// Dispatches on the Shape component's tag to build that entity's mesh — the export-time "system"
// alongside RayTraceCore.h's hitEntity() (intersection) and scatter() (shading).
MeshData buildEntityMesh( TransformGPU transform, ShapeGPU shape )
{
	switch ( shape.type )
	{
		case SHAPE_SPHERE:
			return buildSphereMesh( transform.position, shape.radius );
		case SHAPE_PYRAMID:
			return buildPyramidMesh( transform, shape.baseHalfWidth, shape.height );
	}
	return MeshData{};
}
