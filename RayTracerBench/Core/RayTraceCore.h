#pragma once

// Dual-compiled: #include'd verbatim by both Shaders/Raytracer.metal and
// CPU/CPURenderer.cpp, so the CPU and GPU renderers execute the exact same
// algorithm source rather than two implementations that merely look similar.
//
// Every function here passes and returns plain values (never references or
// pointers into caller-owned storage) with one deliberate exception:
// rayColor()'s scene-array parameters, which must point at the renderer's
// entity-component/material buffers (see ShaderTypes.h/Scene.hpp's ECS-style
// transforms/shapes/materials arrays). On the CPU those are ordinary
// pointers; in MSL a pointer coming from a kernel's buffer argument must be
// annotated with the `device` address space. RT_DEVICE is the one
// #ifdef __METAL_VERSION__ shim needed to reconcile that — every other
// function only ever touches thread-local copies (e.g. `transforms[i]`/
// `shapes[i]` are read into by-value TransformGPU/ShapeGPU structs before
// being passed anywhere else), which MSL treats as ordinary thread-space
// values needing no annotation at all.
//
// RNG state and hit/scatter outcomes are threaded through as return values
// (RandomFloatSample, HitResult, ScatterResult, ...) rather than
// out-parameters, precisely so no second address-space keyword is needed.

#include "ShaderTypes.h"

#if defined( __METAL_VERSION__ )
	#define RT_DEVICE device
#else
	#include <cmath>
	using std::fabs;
	using std::fmin;
	using std::pow;
	using std::sqrt;
	#define RT_DEVICE
#endif

//-------------------------------------------------------------------------------------------------
// Small vector helpers, implemented from scratch on field access (.x/.y/.z) and arithmetic
// operators only — both native to simd_float3 in C++ and to float3 in MSL — so no vector-math
// library function names (which differ between the two: simd_dot vs dot, etc.) are ever needed.
//
// makeFloat3 exists because even 3-argument *construction* isn't spelled the same way on both
// sides: MSL accepts float3(x,y,z) directly, but plain C++ rejects that as a functional cast with
// too many arguments — it needs simd_make_float3(x,y,z) or brace-init instead.
//-------------------------------------------------------------------------------------------------

inline simd_float3 makeFloat3( float x, float y, float z )
{
#if defined( __METAL_VERSION__ )
	return float3( x, y, z );
#else
	return simd_make_float3( x, y, z );
#endif
}

inline float dot3( simd_float3 a, simd_float3 b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float length3( simd_float3 a )
{
	return sqrt( dot3( a, a ) );
}

inline simd_float3 normalize3( simd_float3 a )
{
	return a / length3( a );
}

inline bool nearZero3( simd_float3 a )
{
	const float eps = 1e-8f;
	return ( fabs( a.x ) < eps ) && ( fabs( a.y ) < eps ) && ( fabs( a.z ) < eps );
}

inline simd_float3 reflect3( simd_float3 v, simd_float3 n )
{
	return v - 2.0f * dot3( v, n ) * n;
}

inline simd_float3 refract3( simd_float3 uv, simd_float3 n, float etaiOverEtat )
{
	float       cosTheta = fmin( dot3( -uv, n ), 1.0f );
	simd_float3 rOutPerp = etaiOverEtat * ( uv + cosTheta * n );
	simd_float3 rOutParallel = -sqrt( fabs( 1.0f - dot3( rOutPerp, rOutPerp ) ) ) * n;
	return rOutPerp + rOutParallel;
}

inline float reflectance( float cosine, float refractionIndex )
{
	float r0 = ( 1.0f - refractionIndex ) / ( 1.0f + refractionIndex );
	r0 = r0 * r0;
	return r0 + ( 1.0f - r0 ) * pow( 1.0f - cosine, 5.0f );
}

//-------------------------------------------------------------------------------------------------
// pcg-hash RNG. Per-thread state is a plain uint32_t threaded through by value; every draw returns
// the value alongside the advanced state, so callers chain `seed = sample.seed` explicitly instead
// of the RNG needing a mutable reference parameter.
//-------------------------------------------------------------------------------------------------

inline uint32_t pcgHash( uint32_t input )
{
	uint32_t state = input * 747796405u + 2891336453u;
	uint32_t word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

struct RandomFloatSample
{
	float    value;
	uint32_t seed;
};

struct RandomVec3Sample
{
	simd_float3 value;
	uint32_t    seed;
};

inline RandomFloatSample randomFloat( uint32_t seed )
{
	uint32_t nextSeed = pcgHash( seed );
	float    value = (float)nextSeed / 4294967296.0f; // 2^32 -> [0,1)
	return RandomFloatSample{ value, nextSeed };
}

inline RandomFloatSample randomFloatRange( uint32_t seed, float minVal, float maxVal )
{
	RandomFloatSample s = randomFloat( seed );
	s.value = minVal + ( maxVal - minVal ) * s.value;
	return s;
}

inline RandomVec3Sample randomVec3Range( uint32_t seed, float minVal, float maxVal )
{
	RandomFloatSample sx = randomFloatRange( seed, minVal, maxVal );
	RandomFloatSample sy = randomFloatRange( sx.seed, minVal, maxVal );
	RandomFloatSample sz = randomFloatRange( sy.seed, minVal, maxVal );
	return RandomVec3Sample{ makeFloat3( sx.value, sy.value, sz.value ), sz.seed };
}

// Bounded rejection sampling (32 tries) rather than the book's while(true) loop, to keep every
// loop in this shared source statically bounded — matching rayColor()'s bounded bounce depth.
inline RandomVec3Sample randomInUnitSphere( uint32_t seed )
{
	for ( int i = 0; i < 32; ++i )
	{
		RandomVec3Sample s = randomVec3Range( seed, -1.0f, 1.0f );
		seed = s.seed;
		if ( dot3( s.value, s.value ) < 1.0f )
			return s;
	}
	return RandomVec3Sample{ makeFloat3( 0.0f, 0.0f, 0.0f ), seed };
}

inline RandomVec3Sample randomUnitVector( uint32_t seed )
{
	RandomVec3Sample s = randomInUnitSphere( seed );
	s.value = normalize3( s.value );
	return s;
}

inline RandomVec3Sample randomInUnitDisk( uint32_t seed )
{
	for ( int i = 0; i < 32; ++i )
	{
		RandomFloatSample sx = randomFloatRange( seed, -1.0f, 1.0f );
		RandomFloatSample sy = randomFloatRange( sx.seed, -1.0f, 1.0f );
		simd_float3       p = makeFloat3( sx.value, sy.value, 0.0f );
		seed = sy.seed;
		if ( dot3( p, p ) < 1.0f )
			return RandomVec3Sample{ p, seed };
	}
	return RandomVec3Sample{ makeFloat3( 0.0f, 0.0f, 0.0f ), seed };
}

//-------------------------------------------------------------------------------------------------
// Ray / hit record / camera ray.
//-------------------------------------------------------------------------------------------------

struct Ray
{
	simd_float3 origin;
	simd_float3 direction;
};

inline simd_float3 rayAt( Ray r, float t )
{
	return r.origin + t * r.direction;
}

struct HitRecord
{
	simd_float3 p;
	simd_float3 normal;
	float       t;
	int         materialIndex;
	bool        frontFace;
};

struct HitResult
{
	bool      hit;
	HitRecord record;
};

// `transform`/`shape` are taken by value: callers read them out of the (possibly device-space)
// component arrays once, at the call site, into plain thread-local copies — see the file header
// comment. A sphere's Transform only ever contributes `position` (its center); `right`/`up`/
// `forward` are meaningless for a rotationally-symmetric shape and are simply ignored here.
inline HitResult hitSphere( TransformGPU transform, ShapeGPU shape, Ray r, float tMin, float tMax )
{
	HitResult result;
	result.hit = false;

	simd_float3 center = transform.position;
	float       radius = shape.radius;

	simd_float3 oc = r.origin - center;
	float       a = dot3( r.direction, r.direction );
	float       halfB = dot3( oc, r.direction );
	float       c = dot3( oc, oc ) - radius * radius;
	float       discriminant = halfB * halfB - a * c;
	if ( discriminant < 0.0f )
		return result;
	float sqrtd = sqrt( discriminant );

	float root = ( -halfB - sqrtd ) / a;
	if ( root < tMin || root > tMax )
	{
		root = ( -halfB + sqrtd ) / a;
		if ( root < tMin || root > tMax )
			return result;
	}

	HitRecord rec;
	rec.t = root;
	rec.p = rayAt( r, root );
	simd_float3 outwardNormal = ( rec.p - center ) / radius;
	rec.frontFace = dot3( r.direction, outwardNormal ) < 0.0f;
	rec.normal = rec.frontFace ? outwardNormal : -outwardNormal;
	rec.materialIndex = shape.materialIndex;

	result.hit = true;
	result.record = rec;
	return result;
}

//-------------------------------------------------------------------------------------------------
// Square pyramid, represented as the intersection of 5 half-spaces (1 base + 4 triangular sides)
// and solved with the Kay-Kajiya slab method — the same technique an axis-aligned box uses with
// its 6 planes, generalized to these 5. Canonical local space: base square in the local XZ plane
// at y=0 (corners at (+-halfWidth, 0, +-halfWidth)), apex at local (0, height, 0). Each side face's
// plane equation falls out of the pyramid's symmetry: e.g. the +X face passes through corners
// (halfWidth,0,+-halfWidth) and the apex, giving height*x + halfWidth*y = halfWidth*height, i.e.
// outward normal (height, halfWidth, 0) at distance halfWidth*height from the origin; the other
// three side faces follow by symmetry (negate x, or swap x/z for the z-facing pair).
//-------------------------------------------------------------------------------------------------

struct LocalHitResult
{
	bool        hit;
	float       t;
	simd_float3 localNormal;
};

inline LocalHitResult hitPyramidLocal( Ray localRay, float halfWidth, float height, float tMin, float tMax )
{
	const simd_float3 normals[ 5 ] = {
		makeFloat3( 0.0f, -1.0f, 0.0f ),        // base
		makeFloat3( height, halfWidth, 0.0f ),  // +X side
		makeFloat3( -height, halfWidth, 0.0f ), // -X side
		makeFloat3( 0.0f, halfWidth, height ),  // +Z side
		makeFloat3( 0.0f, halfWidth, -height ), // -Z side
	};
	const float planeDist = halfWidth * height;
	const float dists[ 5 ] = { 0.0f, planeDist, planeDist, planeDist, planeDist };

	float tNear = tMin;
	float tFar = tMax;
	int   enterFace = -1;

	for ( int i = 0; i < 5; ++i )
	{
		float denom = dot3( normals[ i ], localRay.direction );
		float numer = dists[ i ] - dot3( normals[ i ], localRay.origin );

		if ( fabs( denom ) < 1e-8f )
		{
			// Ray parallel to this face: if the origin is already outside its half-space, the ray
			// can never enter the pyramid at all, regardless of the other four faces.
			if ( numer < 0.0f )
				return LocalHitResult{ false, 0.0f, makeFloat3( 0.0f, 0.0f, 0.0f ) };
			continue;
		}

		float t = numer / denom;
		if ( denom < 0.0f )
		{
			if ( t > tNear )
			{
				tNear = t;
				enterFace = i;
			}
		}
		else
		{
			if ( t < tFar )
				tFar = t;
		}
	}

	// enterFace < 0 means no face constrained tNear away from tMin — either every plane was
	// satisfied at tMin already (ray origin outside the solid but the entry point falls before the
	// caller's valid range) or the ray starts inside the pyramid (not handled: see Scene.cpp's note
	// on why pyramids are never dielectric).
	if ( enterFace < 0 || tNear > tFar )
		return LocalHitResult{ false, 0.0f, makeFloat3( 0.0f, 0.0f, 0.0f ) };

	return LocalHitResult{ true, tNear, normalize3( normals[ enterFace ] ) };
}

// Transforms the world-space ray into the pyramid's local space by projecting onto its orthonormal
// right/up/forward basis (an orthogonal-matrix inverse is just its transpose, so no matrix inverse
// is needed), runs the local intersection above, then maps the hit normal back to world space by
// the same basis run forward. Because translation+rotation is an isometry, `t` is identical in
// local and world space — the hit point itself is computed directly in world space via rayAt().
inline HitResult hitPyramid( TransformGPU transform, ShapeGPU shape, Ray r, float tMin, float tMax )
{
	HitResult result;
	result.hit = false;

	simd_float3 oc = r.origin - transform.position;
	Ray         localRay;
	localRay.origin = makeFloat3( dot3( oc, transform.right ), dot3( oc, transform.up ), dot3( oc, transform.forward ) );
	localRay.direction = makeFloat3(
		dot3( r.direction, transform.right ), dot3( r.direction, transform.up ), dot3( r.direction, transform.forward ) );

	LocalHitResult lh = hitPyramidLocal( localRay, shape.baseHalfWidth, shape.height, tMin, tMax );
	if ( !lh.hit )
		return result;

	HitRecord rec;
	rec.t = lh.t;
	rec.p = rayAt( r, lh.t );
	simd_float3 outwardNormal =
		lh.localNormal.x * transform.right + lh.localNormal.y * transform.up + lh.localNormal.z * transform.forward;
	rec.frontFace = dot3( r.direction, outwardNormal ) < 0.0f;
	rec.normal = rec.frontFace ? outwardNormal : -outwardNormal;
	rec.materialIndex = shape.materialIndex;

	result.hit = true;
	result.record = rec;
	return result;
}

// The "collision system": iterates one entity's Transform + Shape components and dispatches the
// intersection test by the Shape component's tag — the ECS analogue of scatter()'s tagged switch
// below, and the direct replacement for a virtual Hittable::hit() call (forbidden in MSL kernels;
// see CLAUDE.md's core correctness constraint). Adding a third primitive means one more case here,
// not a new subclass.
inline HitResult hitEntity( TransformGPU transform, ShapeGPU shape, Ray r, float tMin, float tMax )
{
	switch ( shape.type )
	{
		case SHAPE_SPHERE:
			return hitSphere( transform, shape, r, tMin, tMax );
		case SHAPE_PYRAMID:
			return hitPyramid( transform, shape, r, tMin, tMax );
	}
	HitResult miss;
	miss.hit = false;
	return miss;
}

//-------------------------------------------------------------------------------------------------
// Tagged-switch scatter, replacing the book's virtual material dispatch (forbidden in MSL kernel
// code). `mat` is likewise a by-value thread-local copy, read out of the materials buffer by the
// caller.
//-------------------------------------------------------------------------------------------------

struct ScatterResult
{
	bool        scattered;
	simd_float3 attenuation;
	Ray         scatteredRay;
	uint32_t    rngSeed;
};

inline ScatterResult scatter( Ray rayIn, HitRecord rec, MaterialGPU mat, uint32_t rngSeed )
{
	switch ( mat.type )
	{
		case MAT_LAMBERTIAN:
		{
			RandomVec3Sample rv = randomUnitVector( rngSeed );
			simd_float3      scatterDirection = rec.normal + rv.value;
			if ( nearZero3( scatterDirection ) )
				scatterDirection = rec.normal;
			return ScatterResult{ true, mat.albedo, Ray{ rec.p, scatterDirection }, rv.seed };
		}
		case MAT_METAL:
		{
			RandomVec3Sample rv = randomInUnitSphere( rngSeed );
			simd_float3      reflected = reflect3( normalize3( rayIn.direction ), rec.normal );
			Ray              scattered = Ray{ rec.p, reflected + mat.fuzz * rv.value };
			bool             ok = dot3( scattered.direction, rec.normal ) > 0.0f;
			return ScatterResult{ ok, mat.albedo, scattered, rv.seed };
		}
		case MAT_DIELECTRIC:
		{
			float             refractionRatio = rec.frontFace ? ( 1.0f / mat.ir ) : mat.ir;
			simd_float3       unitDirection = normalize3( rayIn.direction );
			float             cosTheta = fmin( dot3( -unitDirection, rec.normal ), 1.0f );
			float             sinTheta = sqrt( 1.0f - cosTheta * cosTheta );
			bool              cannotRefract = refractionRatio * sinTheta > 1.0f;
			RandomFloatSample rf = randomFloat( rngSeed );
			simd_float3       direction;
			if ( cannotRefract || reflectance( cosTheta, refractionRatio ) > rf.value )
				direction = reflect3( unitDirection, rec.normal );
			else
				direction = refract3( unitDirection, rec.normal, refractionRatio );
			return ScatterResult{ true, makeFloat3( 1.0f, 1.0f, 1.0f ), Ray{ rec.p, direction }, rf.seed };
		}
	}
	return ScatterResult{ false, makeFloat3( 0.0f, 0.0f, 0.0f ), rayIn, rngSeed };
}

//-------------------------------------------------------------------------------------------------
// Iterative, bounded-depth path trace — no recursion, so CPU and GPU stay structurally identical
// and the GPU side never risks a call-stack blowup.
//-------------------------------------------------------------------------------------------------

struct RayColorResult
{
	simd_float3 color;
	uint32_t    rngSeed;
};

inline RayColorResult rayColor( Ray r,
	RT_DEVICE const TransformGPU* transforms, RT_DEVICE const ShapeGPU* shapes, uint32_t entityCount,
	RT_DEVICE const MaterialGPU* materials, uint32_t maxDepth, uint32_t rngSeed )
{
	simd_float3 accumulated = makeFloat3( 1.0f, 1.0f, 1.0f );

	for ( uint32_t depth = 0; depth < maxDepth; ++depth )
	{
		HitRecord closestRecord;
		bool      hitAnything = false;
		float     closestSoFar = 1.0e30f;

		// The "render system": walks every entity's Transform+Shape components and keeps the
		// closest hit, exactly like the book's world.hit() loop but over flat component arrays
		// and a tagged dispatch (hitEntity()) instead of a virtual call per entity.
		for ( uint32_t i = 0; i < entityCount; ++i )
		{
			HitResult hr = hitEntity( transforms[ i ], shapes[ i ], r, 0.001f, closestSoFar );
			if ( hr.hit )
			{
				hitAnything = true;
				closestSoFar = hr.record.t;
				closestRecord = hr.record;
			}
		}

		if ( !hitAnything )
		{
			simd_float3 unitDirection = normalize3( r.direction );
			float       t = 0.5f * ( unitDirection.y + 1.0f );
			simd_float3 skyColor = ( 1.0f - t ) * makeFloat3( 1.0f, 1.0f, 1.0f ) + t * makeFloat3( 0.5f, 0.7f, 1.0f );
			return RayColorResult{ accumulated * skyColor, rngSeed };
		}

		MaterialGPU    mat = materials[ closestRecord.materialIndex ];
		ScatterResult  sr = scatter( r, closestRecord, mat, rngSeed );
		rngSeed = sr.rngSeed;

		if ( !sr.scattered )
			return RayColorResult{ makeFloat3( 0.0f, 0.0f, 0.0f ), rngSeed };

		accumulated = accumulated * sr.attenuation;
		r = sr.scatteredRay;
	}

	return RayColorResult{ makeFloat3( 0.0f, 0.0f, 0.0f ), rngSeed }; // exceeded bounce budget
}

//-------------------------------------------------------------------------------------------------
// Camera ray generation, including thin-lens defocus blur sampling.
//-------------------------------------------------------------------------------------------------

struct CameraRaySample
{
	Ray      ray;
	uint32_t rngSeed;
};

inline CameraRaySample getRay( CameraGPU cam, float s, float t, uint32_t rngSeed )
{
	RandomVec3Sample diskSample = randomInUnitDisk( rngSeed );
	simd_float3      rd = cam.lensRadius * diskSample.value;
	simd_float3      offset = cam.u * rd.x + cam.v * rd.y;

	simd_float3 origin = cam.origin + offset;
	simd_float3 direction = cam.lowerLeftCorner + s * cam.horizontal + t * cam.vertical - cam.origin - offset;

	return CameraRaySample{ Ray{ origin, direction }, diskSample.seed };
}
