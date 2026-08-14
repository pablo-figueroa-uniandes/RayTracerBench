#pragma once

// Dual-compiled: #include'd verbatim by both Shaders/Raytracer.metal and
// CPU/CPURenderer.cpp, so the CPU and GPU renderers execute the exact same
// algorithm source rather than two implementations that merely look similar.
//
// Every function here passes and returns plain values (never references or
// pointers into caller-owned storage) with one deliberate exception:
// rayColor()'s two scene-array parameters, which must point at the
// renderer's sphere/material buffers. On the CPU those are ordinary
// pointers; in MSL a pointer coming from a kernel's buffer argument must be
// annotated with the `device` address space. RT_DEVICE is the one
// #ifdef __METAL_VERSION__ shim needed to reconcile that — every other
// function only ever touches thread-local copies (e.g. `spheres[i]` is read
// into a by-value SphereGPU before being passed anywhere else), which MSL
// treats as ordinary thread-space values needing no annotation at all.
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

// `sphere` is taken by value: callers read it out of the (possibly device-space) sphere buffer
// once, at the call site, into a plain thread-local copy — see the file header comment.
inline HitResult hitSphere( SphereGPU sphere, Ray r, float tMin, float tMax )
{
	HitResult result;
	result.hit = false;

	simd_float3 oc = r.origin - sphere.center;
	float       a = dot3( r.direction, r.direction );
	float       halfB = dot3( oc, r.direction );
	float       c = dot3( oc, oc ) - sphere.radius * sphere.radius;
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
	simd_float3 outwardNormal = ( rec.p - sphere.center ) / sphere.radius;
	rec.frontFace = dot3( r.direction, outwardNormal ) < 0.0f;
	rec.normal = rec.frontFace ? outwardNormal : -outwardNormal;
	rec.materialIndex = sphere.materialIndex;

	result.hit = true;
	result.record = rec;
	return result;
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

inline RayColorResult rayColor( Ray r, RT_DEVICE const SphereGPU* spheres, uint32_t sphereCount,
	RT_DEVICE const MaterialGPU* materials, uint32_t maxDepth, uint32_t rngSeed )
{
	simd_float3 accumulated = makeFloat3( 1.0f, 1.0f, 1.0f );

	for ( uint32_t depth = 0; depth < maxDepth; ++depth )
	{
		HitRecord closestRecord;
		bool      hitAnything = false;
		float     closestSoFar = 1.0e30f;

		for ( uint32_t i = 0; i < sphereCount; ++i )
		{
			HitResult hr = hitSphere( spheres[ i ], r, 0.001f, closestSoFar );
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
