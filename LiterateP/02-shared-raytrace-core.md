# Chapter 2: The Shared Ray-Tracing Core

**Abstract.** This chapter covers `RayTracerBench/Core/RayTraceCore.h`, the
single 537-line file that *is* the ray tracer — hit-testing, material
scattering, camera-ray generation, and the bounded path-tracing loop that
ties them together. Its defining property is not any one algorithm but the
fact that it is compiled twice from one source: once as Metal Shading
Language, inside `Shaders/Raytracer.metal`'s GPU kernel, and once as plain
C++17, inside `CPU/CPURenderer.cpp`. Every design choice in this file —
value-returning functions instead of out-parameters, a single
`#ifdef __METAL_VERSION__`, hand-rolled vector math instead of library
calls — exists to make that dual compilation possible without maintaining
two hand-synced copies. As the top-level README puts it, this is "the heart
of the whole program"; Chapters 1, 3, 4, and 5 all exist either to feed it
data or to execute it.

Files covered: `RayTracerBench/Core/RayTraceCore.h`.

## §1. The constraint that shapes every line in this file

Metal Shading Language forbids virtual functions, RTTI, and dynamic
allocation inside kernel code. The reference implementations this project
is structurally modeled on — "Ray Tracing in One Weekend" and its CUDA
port — both lean on a polymorphic `Hittable`/`Material` class hierarchy with
virtual `hit()`/`scatter()` methods; CUDA allows that (its device compiler
supports virtual dispatch), but MSL does not. Rather than write a
CPU renderer with virtual dispatch and a separately-written GPU renderer
with tagged-switch dispatch — two implementations that merely *look*
similar, and would silently drift the moment someone fixed a bug in only
one of them — this project uses tagged structs and switch-based dispatch on
**both** sides, and, more importantly, makes both sides execute the
*same source file*. The file's own header comment states this plainly:

```cpp
// Dual-compiled: #include'd verbatim by both Shaders/Raytracer.metal and
// CPU/CPURenderer.cpp, so the CPU and GPU renderers execute the exact same
// algorithm source rather than two implementations that merely look similar.
```
`RayTracerBench/Core/RayTraceCore.h:3-5`

"Dual-compiled" is doing real work in that sentence, and it is worth being
precise about what it does *not* mean. It does not mean this project keeps
two copies of the ray-tracing algorithm — one in a `.metal` file, one in a
`.cpp` file — annotated with `// KEEP IN SYNC` comments at the top and
bottom, trusting future edits to touch both. That approach was considered
and explicitly rejected as a fallback of last resort: CLAUDE.md's Core
section notes it only as "the fallback... if shared compilation proves too
fussy," a fallback this project never needed. Instead, `Raytracer.metal`
literally `#include`s this header as MSL source, and `CPURenderer.cpp`
literally `#include`s the identical header as C++ source (Chapters 3 and 4
show both include sites). The compiler that turns this text into a GPU
kernel and the compiler that turns it into CPU machine code are reading
character-for-character the same bytes. If `hitSphere()` has a bug, it has
that bug on both renderers, simultaneously, by construction — there is no
"forgot to update the other copy" failure mode to guard against.

Getting one file to compile as two different, incompatible languages is
not free, though — MSL and C++17 disagree about address spaces, about
vector-construction syntax, and about which standard-library functions
exist. The rest of this chapter is largely the story of how this file
papers over those disagreements with the smallest possible number of
`#ifdef`s.

## §2. One address-space keyword, and the design discipline it forces

MSL requires every pointer that crosses a kernel function's argument
boundary to be annotated with an address space — `device`, `constant`,
`thread`, or `threadgroup`. Plain C++ has no such concept; a pointer is
just a pointer. If this file freely passed pointers or references around —
the natural C++ style for an out-parameter like "fill in this `HitRecord`
for me" — every one of those parameters would need an address-space
annotation on the MSL side, and C++ has no matching keyword to spell for
`thread`-space locals. That would force either two divergent function
signatures (defeating the entire point of dual-compiling one file) or an
even uglier tangle of macros per parameter.

This file avoids that by threading everything through **return values**
instead. `RandomFloatSample`, `RandomVec3Sample`, `HitResult`, `ScatterResult`,
`RayColorResult`, and `CameraRaySample` are all small structs that exist for
exactly this reason: to carry a function's "outputs" — including updated
RNG state — back to the caller as a single returned value, rather than
through a `HitRecord&` or `uint32_t&` parameter. The header comment names
this explicitly:

```cpp
// RNG state and hit/scatter outcomes are threaded through as return values
// (RandomFloatSample, HitResult, ScatterResult, ...) rather than
// out-parameters, precisely so no second address-space keyword is needed.
```
`RayTracerBench/Core/RayTraceCore.h:20-22`

With that discipline in place, exactly *one* address-space keyword ends up
needed anywhere in the file, and it shows up in exactly one place:
`rayColor()`'s three scene-array pointers, which must point at the GPU
kernel's actual buffer arguments (or, on the CPU, at ordinary heap arrays
owned by `SceneDescription` — see Chapter 1). That single keyword is
spelled through a macro:

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:26-35`

`__METAL_VERSION__` is defined automatically by Apple's Metal compiler and
never by a plain C++ compiler, so this `#ifdef` is the one and only branch
point between "being compiled as a shader" and "being compiled as a CPU
translation unit." On the MSL side, `RT_DEVICE` expands to the `device`
keyword; on the C++ side it expands to nothing at all, which is exactly
right, because a bare pointer *is* how C++ already spells "pointer to
something the caller owns." `rayColor()`'s signature shows the macro in its
one use site:

```cpp
inline RayColorResult rayColor( Ray r,
	RT_DEVICE const TransformGPU* transforms, RT_DEVICE const ShapeGPU* shapes, uint32_t entityCount,
	RT_DEVICE const MaterialGPU* materials, uint32_t maxDepth, uint32_t rngSeed )
```
`RayTracerBench/Core/RayTraceCore.h:467-469`

Every other function in the file — `hitSphere()`, `hitPyramid()`,
`hitEntity()`, `scatter()`, `getRay()` — takes its `TransformGPU`/`ShapeGPU`/
`MaterialGPU` arguments *by value*. That is not an incidental style choice;
it is what makes the rest of the file exempt from needing any address-space
annotation at all. `rayColor()`'s inner loop reads `transforms[i]` and
`shapes[i]` out of the `device`-space buffers exactly once per entity, into
by-value thread-local copies, and only *those* copies get passed onward
into `hitEntity()`:

```cpp
HitResult hr = hitEntity( transforms[ i ], shapes[ i ], r, 0.001f, closestSoFar );
```
`RayTracerBench/Core/RayTraceCore.h:484`

MSL treats a `TransformGPU` or `ShapeGPU` value copied out of a `device`
buffer as an ordinary `thread`-space value from that point on, needing no
further annotation — which is precisely why a second, MSL-only address-space
keyword (`thread`) is never needed anywhere in this file, even though a more
naively-ported C++ codebase (one that passed hit records by reference) would
have needed it constantly.

## §3. Vector arithmetic, built from scratch

Before any ray tracing happens, the file has to solve a smaller but equally
real portability problem: `simd_float3` (the C++/`<simd/simd.h>` 3-vector
type) and MSL's native `float3` are laid out identically but do not share
an API. Plain C++'s `simd` library spells dot product `simd_dot`; MSL spells
it `dot`. Worse, MSL allows `float3(x, y, z)` as ordinary function-style
construction, while C++ rejects that same syntax as an illegal 3-argument
functional cast — it insists on `simd_make_float3(x, y, z)` or brace-init
instead. Rather than macro away every one of these naming mismatches
individually, this file sidesteps essentially all of them by implementing
its own tiny vector-math layer using only field access (`.x`/`.y`/`.z`) and
arithmetic operators, which *are* spelled identically on both sides:

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:49-62`

`makeFloat3()` is the file's second and last `#ifdef __METAL_VERSION__` —
everything downstream of it (`length3`, `normalize3`, `nearZero3`,
`reflect3`, `refract3`, `reflectance`) is ordinary, portable arithmetic over
`.x`/`.y`/`.z` and needs no further conditional compilation at all. Any new
3-argument vector literal added later to this file is expected to go
through `makeFloat3()` rather than reaching for `simd_make_float3` or
`float3(...)` directly — `Scene.cpp`, by contrast, is pure host C++ that is
*never* Metal-compiled, and uses the native `simd_make_float3`/`simd_dot`
C API directly, because it has no dual-compilation constraint to honor.

## §4. `pcgHash`/`randomFloat`: one RNG, run identically on both processors

The CUDA port this project takes structural cues from uses NVIDIA's
`curand` library for its per-thread random numbers — not an option here,
since it is neither portable to MSL nor to a plain CPU. This file replaces
it with a pcg-hash-based RNG that is small enough to be genuinely identical,
statement for statement, on both processors:

```cpp
inline uint32_t pcgHash( uint32_t input )
{
	uint32_t state = input * 747796405u + 2891336453u;
	uint32_t word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
```
`RayTracerBench/Core/RayTraceCore.h:116-121`

The state itself is nothing more exotic than a `uint32_t`, and per §2's
discipline it is threaded through by value rather than by mutable
reference: every draw function takes a `seed` and returns both the drawn
value *and* the advanced seed in one struct, so a caller chains state
explicitly (`seed = sample.seed`) instead of the RNG needing an out-parameter:

```cpp
inline RandomFloatSample randomFloat( uint32_t seed )
{
	uint32_t nextSeed = pcgHash( seed );
	float    value = (float)nextSeed / 4294967296.0f; // 2^32 -> [0,1)
	return RandomFloatSample{ value, nextSeed };
}
```
`RayTracerBench/Core/RayTraceCore.h:136-141`

`randomFloatRange()` and `randomVec3Range()` build on `randomFloat()` the
same way, each threading `seed` forward through the next draw
(`RayTracerBench/Core/RayTraceCore.h:144-158`). Two derived samplers are
worth calling out because of a small but deliberate departure from the
book's own code: `randomInUnitSphere()` and `randomInUnitDisk()` are both
rejection samplers (draw a point in a cube/square, keep it only if it
lands inside the inscribed sphere/disk), and the book's reference
implementation expresses that as a `while (true)` loop. An unbounded loop
is unacceptable in a kernel meant to run identically as a GPU compute
shader, so both are capped at 32 attempts instead, with a zero-vector
fallback if the cap is ever hit:

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:162-172`

This is the same "every loop in this shared source must be statically
bounded" principle that governs `rayColor()`'s bounce loop in §9 —
rejection probability for a random point landing inside an inscribed
sphere/disk is high enough (roughly 52% for a sphere in a cube, ~79% for a
disk in a square) that 32 tries failing is astronomically unlikely, but the
*loop shape itself*, not just the expected iteration count, has to be
boundable for MSL kernel code to be well-formed.

## §5. `hitSphere()`: the simplest entity, and the shape of a `HitResult`

`hitSphere()` is the first place the file's `HitResult`/`HitRecord` return
convention appears in full, and it is a good reference point before §6's
more involved pyramid case:

```cpp
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
	/* ... fills in HitRecord and returns result.hit = true ... */
}
```
`RayTracerBench/Core/RayTraceCore.h:234-270` (elided: `HitRecord`
construction, lines 259–269)

This is the book's usual quadratic-formula sphere test, algebraically
unchanged — the interesting part, given this chapter's theme, is what it
*doesn't* do: `transform` and `shape` arrive by value (per §2), `center`
comes from `transform.position` alone, and the comment above the function
notes that a sphere's `right`/`up`/`forward` basis fields are simply
ignored, "meaningless for a rotationally-symmetric shape"
(`RayTracerBench/Core/RayTraceCore.h:230-233`). That basis becomes essential
the moment the second primitive — a pyramid, which very much has an
orientation — enters the picture.

## §6. `hitPyramidLocal()`/`hitPyramid()`: five half-spaces, one slab test

A square pyramid is not a primitive either format or the ray-tracing book
gives you for free, and this file's solution generalizes a trick usually
reserved for axis-aligned boxes. An AABB is the intersection of 6
half-spaces (one pair per axis), and the standard fast way to ray-test one
is the Kay–Kajiya slab method: shrink a valid-`t` interval `[tNear, tFar]`
by intersecting it against each pair of parallel planes in turn, and reject
the ray the moment the interval becomes empty. A pyramid has 5 faces
instead of 6 and none of them come in parallel pairs, but the *same*
narrowing-interval algorithm still applies to any convex solid expressed as
an intersection of half-spaces — box or pyramid alike. The header comment
above `hitPyramidLocal()` states the generalization directly and also
pins down the canonical local frame the closed-form planes below are
derived in:

```cpp
// Square pyramid, represented as the intersection of 5 half-spaces (1 base + 4 triangular sides)
// and solved with the Kay-Kajiya slab method — the same technique an axis-aligned box uses with
// its 6 planes, generalized to these 5. Canonical local space: base square in the local XZ plane
// at y=0 (corners at (+-halfWidth, 0, +-halfWidth)), apex at local (0, height, 0).
```
`RayTracerBench/Core/RayTraceCore.h:273-277`

Each face's plane equation is not fit numerically from the corner points —
it falls straight out of the pyramid's symmetry. Take the +X side face: it
passes through the two base corners `(halfWidth, 0, ±halfWidth)` and the
apex `(0, height, 0)`. Solving for that plane's equation by hand gives
`height·x + halfWidth·y = halfWidth·height`, i.e. an (unnormalized) outward
normal of `(height, halfWidth, 0)` at signed distance `halfWidth·height`
from the local origin. The other three side faces are the same equation
under the pyramid's obvious symmetries — negate `x` for the −X face, swap
`x`/`z` for the two Z-facing faces — so all four can be written down without
solving anything a second time:

```cpp
const simd_float3 normals[ 5 ] = {
	makeFloat3( 0.0f, -1.0f, 0.0f ),        // base
	makeFloat3( height, halfWidth, 0.0f ),  // +X side
	makeFloat3( -height, halfWidth, 0.0f ), // -X side
	makeFloat3( 0.0f, halfWidth, height ),  // +Z side
	makeFloat3( 0.0f, halfWidth, -height ), // -Z side
};
const float planeDist = halfWidth * height;
const float dists[ 5 ] = { 0.0f, planeDist, planeDist, planeDist, planeDist };
```
`RayTracerBench/Core/RayTraceCore.h:292-300`

(The base plane is simpler still — its outward normal is just
world-down, `(0, -1, 0)`, at distance 0 from the local origin, since the
base sits exactly at local `y = 0`.)

With the five planes fixed, the slab loop narrows `[tNear, tFar]` one face
at a time. For each face, `numer`/`denom` gives the ray parameter `t` where
the ray crosses that face's plane; whether the face is being *entered* or
*exited* is decided by the sign of `denom` (the ray direction's component
along the outward normal):

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:306-334`

`denom < 0` means the ray is heading *into* that face's half-space
(entering), so it can only ever raise `tNear`, and whichever face last did
so is remembered in `enterFace` — that becomes the hit normal if the ray
ultimately hits at all. `denom > 0` means the ray is heading *out* of that
half-space (exiting), so it can only lower `tFar`. A ray parallel to a
face (`denom ≈ 0`) never changes the interval either way, except in the
degenerate case where the ray's origin is already outside that face's
plane, in which case no value of `t` can ever put it back inside and the
whole test is immediately rejected (`RayTracerBench/Core/RayTraceCore.h:311-318`).
After all five faces are folded into the interval, the pyramid was hit only
if some face actually raised `tNear` above `tMin` (`enterFace >= 0`) *and*
that interval never went empty (`tNear <= tFar`):

```cpp
if ( enterFace < 0 || tNear > tFar )
	return LocalHitResult{ false, 0.0f, makeFloat3( 0.0f, 0.0f, 0.0f ) };

return LocalHitResult{ true, tNear, normalize3( normals[ enterFace ] ) };
```
`RayTracerBench/Core/RayTraceCore.h:340-343`

That comment above the rejection check is where this file documents an
explicit, deliberate scope limit rather than a bug: `enterFace < 0` can
also mean the ray's origin already starts *inside* the pyramid (every
plane already satisfied at `tMin`), a case this test does not resolve
correctly. That case matters specifically for glass: refraction sends a
ray *through* a dielectric surface, so a correct glass pyramid would need
to detect the ray exiting from inside the solid on its far side — exactly
the situation this slab test's entry-face-only logic can't handle. Rather
than special-case that, `Scene.cpp` (Chapter 1) simply never assigns a
`MAT_DIELECTRIC` material to a pyramid entity; pyramids in this scene are
always lambertian or metal, a documented restriction rather than an
oversight.

`hitPyramidLocal()` only ever sees the ray in the pyramid's own canonical
local frame, though — the ray this file actually receives is in world
space, where the pyramid can sit at any position and any orientation.
`hitPyramid()` is the wrapper that converts between the two, and it is
where `TransformGPU`'s orthonormal `right`/`up`/`forward` basis (Chapter 1)
earns its keep. The trick it relies on is that the inverse of an orthogonal
matrix is just its transpose — so instead of building and inverting a
3×3 rotation matrix, transforming a world-space vector into the pyramid's
local frame is just three dot products, one against each basis vector:

```cpp
simd_float3 oc = r.origin - transform.position;
Ray         localRay;
localRay.origin = makeFloat3( dot3( oc, transform.right ), dot3( oc, transform.up ), dot3( oc, transform.forward ) );
localRay.direction = makeFloat3(
	dot3( r.direction, transform.right ), dot3( r.direction, transform.up ), dot3( r.direction, transform.forward ) );
```
`RayTracerBench/Core/RayTraceCore.h:356-360`

and mapping the resulting local-space hit normal back out to world space
runs the same basis the other direction — not a transpose this time, but
literally interpreting the local normal's components as coefficients on
the basis vectors themselves:

```cpp
simd_float3 outwardNormal =
	lh.localNormal.x * transform.right + lh.localNormal.y * transform.up + lh.localNormal.z * transform.forward;
```
`RayTracerBench/Core/RayTraceCore.h:369-370`

The accompanying comment notes one more simplification this buys: because
translating and rotating a ray is an isometry (it doesn't stretch
distances), the intersection parameter `t` computed in local space is
identical to `t` in world space, so the world-space hit point itself can be
computed directly via `rayAt(r, lh.t)` on the *original* world-space ray —
no inverse transform of the hit point is ever needed
(`RayTracerBench/Core/RayTraceCore.h:346-350`).

## §7. `hitEntity()`: the collision system

With both primitives' intersection tests in hand, `hitEntity()` is the
function that ties `ShapeGPU`'s tag to the right test — and, per the
project's ECS framing (Chapter 1), this is properly thought of as a
"collision system" operating over the Transform+Shape component arrays,
not a shape method:

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:385-397`

The comment directly above it is explicit about what this replaces:

```cpp
// The "collision system": iterates one entity's Transform + Shape components and dispatches the
// intersection test by the Shape component's tag — the ECS analogue of scatter()'s tagged switch
// below, and the direct replacement for a virtual Hittable::hit() call (forbidden in MSL kernels;
// see CLAUDE.md's core correctness constraint). Adding a third primitive means one more case here,
// not a new subclass.
```
`RayTracerBench/Core/RayTraceCore.h:380-384`

This is the entire cost of adding a new primitive shape to the renderer:
one more `SHAPE_*` enumerator in `ShaderTypes.h`, one more intersection
function, and one more `case` here — never a new class, a new vtable slot,
or a change to any caller. `rayColor()` (§9) never needs to know how many
shape kinds exist; it only ever calls `hitEntity()` once per entity, and
`hitEntity()` is the only place that switches on `shape.type` at all.

## §8. `scatter()`: the same tagged-switch idea, applied to materials

`scatter()` is `hitEntity()`'s counterpart on the shading side — a tagged
switch replacing the book's virtual `Material::scatter()`, over the three
`MaterialType` tags from `ShaderTypes.h` (Chapter 1):

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:415-452`

All three branches are the book's standard physical models —
cosine-weighted diffuse scatter for Lambertian (with a degenerate-direction
guard via `nearZero3()`, §3), fuzzy mirror reflection for metal, and
Schlick-approximated reflect/refract branching (via `reflectance()`, §3)
for dielectric — but every one of them ends the same way this file always
does: reading `rngSeed` in, drawing whatever randomness it needs, and
packaging a new `rngSeed` back out inside `ScatterResult` rather than
mutating anything by reference. `mat` itself is a plain by-value copy, per
the same discipline as `hitSphere()`/`hitPyramid()`'s `shape` parameter —
the comment directly above the struct spells this out:

```cpp
// Tagged-switch scatter, replacing the book's virtual material dispatch (forbidden in MSL kernel
// code). `mat` is likewise a by-value thread-local copy, read out of the materials buffer by the
// caller.
```
`RayTracerBench/Core/RayTraceCore.h:400-402`

## §9. `rayColor()`: the bounded loop that both renderers actually call

Every function discussed so far is a building block; `rayColor()` is the
function that assembles them into the path tracer that a caller — the
CPU's per-pixel loop in Chapter 4, or the GPU kernel's per-thread body in
Chapter 3 — invokes once per sample. The book's reference implementation
expresses bounce depth as *recursion*: `ray_color()` calls itself with
`depth - 1` on every scatter. Recursion of unbounded (or even just
data-dependent) depth is exactly the kind of thing that risks a call-stack
blowup on a GPU thread, where stack space per thread is far smaller and
less forgiving than on a CPU — and, independent of that risk, keeping the
two renderers' control flow *structurally* identical (not just
algorithmically equivalent) is the whole premise of dual-compiling one
source file at all. So `rayColor()` is iterative: a single bounded `for`
loop up to `maxDepth`, which the app's default settings put around 50:

```cpp
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
```
`RayTracerBench/Core/RayTraceCore.h:467-513`

Each iteration is the book's `world.hit()` loop, but ECS-flavored: instead
of asking a polymorphic scene object "did anything get hit," it walks the
`transforms[]`/`shapes[]` component arrays in lockstep and calls
`hitEntity()` (§7) per entity, keeping whichever hit has the smallest `t`
so far. That inner scan is annotated in the source as "the render system,"
drawing the same ECS parallel `hitEntity()` itself draws to "the collision
system" (`RayTracerBench/Core/RayTraceCore.h:479-481`). If nothing was hit,
the loop terminates immediately with the sky gradient (a simple vertical
lerp between white and light blue, unchanged from the book). If something
*was* hit, `scatter()` (§8) is called once, `accumulated` is attenuated by
whatever color that material contributed, `r` becomes the new scattered
ray, and the loop continues — recursion turned into plain iteration by
carrying the running product forward as a local variable instead of a
return-value multiplication unwound on the call stack. A ray absorbed by a
material (`sr.scattered == false`, which only `MAT_METAL` can produce, when
the fuzzed reflection points back into the surface) or one that survives
all `maxDepth` bounces without escaping to sky both return black — the
former physically (no light reached the camera along that path), the
latter as the same bounded-loop safety net every other loop in this file
relies on (§4).

## §10. `getRay()`: camera-ray sampling with defocus blur

The last piece is where a ray originates in the first place.
`getRay()` builds one camera ray for a normalized image coordinate `(s, t)`,
including thin-lens defocus-blur jitter — sampling a random point on the
lens disk (via `randomInUnitDisk()`, §4) and offsetting the ray's origin by
it, which is what produces a depth-of-field effect rather than a pinhole
camera's perfectly sharp focus at every distance:

```cpp
inline CameraRaySample getRay( CameraGPU cam, float s, float t, uint32_t rngSeed )
{
	RandomVec3Sample diskSample = randomInUnitDisk( rngSeed );
	simd_float3      rd = cam.lensRadius * diskSample.value;
	simd_float3      offset = cam.u * rd.x + cam.v * rd.y;

	simd_float3 origin = cam.origin + offset;
	simd_float3 direction = cam.lowerLeftCorner + s * cam.horizontal + t * cam.vertical - cam.origin - offset;

	return CameraRaySample{ Ray{ origin, direction }, diskSample.seed };
}
```
`RayTracerBench/Core/RayTraceCore.h:527-537`

`cam.u`/`cam.v` here are the camera's own right/up basis vectors (part of
`CameraGPU`, Chapter 1) — the same "project onto an orthonormal basis"
idea §6 used to move a ray into a pyramid's local space, applied instead to
spread the lens-disk sample across the camera's image plane. As with every
other sampling function in this file, the caller — one loop iteration per
sample per pixel, in both `CPURenderer.cpp` and `Raytracer.metal` — gets
back both the ray and the advanced RNG seed in one struct, ready to feed
straight into `rayColor()`.

## §11. Trusted before it was ever wired up

This file's dual-compilation claim was not left to be discovered the first
time the GPU renderer was assembled and something failed to link. Before
`GPURenderer` existed at all, `RayTraceCore.h` was validated standalone
against the real Metal compiler — `xcrun -sdk macosx metal -c ... -Wall
-Wextra` — and it compiled clean, zero warnings, on the first attempt. That
is the kind of claim this project treats as needing independent evidence
rather than confidence: the value-returning, single-address-space-keyword
design described in §2 is only actually validated by a real compiler
accepting it as legal MSL, not by inspection. Chapter 3 picks up from
exactly this point — how `Raytracer.metal` wraps this file's `rayColor()`
in an actual `kernel void renderKernel(...)` entry point and wires
`RT_DEVICE`'s one real buffer argument to Metal's `dispatchThreads:`
machinery.

## Where this connects

- **Chapter 1 (The Core Data Model)** defines every struct this file
  consumes without owning: `TransformGPU`, `ShapeGPU`/`ShapeType`,
  `MaterialGPU`/`MaterialType`, `CameraGPU`. This chapter's `hitPyramid()`
  (§6) and `getRay()` (§10) are the two places that basis vectors
  (`right`/`up`/`forward`, or `u`/`v`) defined in Chapter 1 actually get
  used as an orthonormal frame rather than just carried around as data.
- **Chapter 3 (The Metal Shaders)** is one of this file's two compile
  targets: `Shaders/Raytracer.metal` `#include`s `RayTraceCore.h` verbatim
  and wraps `rayColor()` in a `kernel void renderKernel(...)` entry point,
  supplying the real `device`-space buffer arguments that make `RT_DEVICE`
  (§2) resolve to something concrete.
- **Chapter 4 (The CPU Renderer)** is this file's other compile target:
  `CPU/CPURenderer.cpp` `#include`s the same header as plain C++, where
  `RT_DEVICE` disappears entirely, and drives `rayColor()`/`getRay()` from
  an `std::thread`-partitioned per-row loop instead of a GPU dispatch grid.
- **Chapter 11 (Tests: Parity, Geometry, and the Core Itself)** is how the
  claim underlying this entire chapter — that the CPU and GPU renderers
  really are executing the same algorithm, not just similar-looking ones —
  gets checked mechanically: a fixed-seed scene run through both compiled
  outputs and diffed, plus the standalone Metal-compiler validation
  mentioned in §11.
