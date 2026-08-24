# Chapter 1: The Core Data Model

**Abstract.** Before a single ray is traced, RayTracerBench has to answer a
quieter question: what *is* a scene, as data? The answer has to satisfy two
masters at once — the CPU renderer, which is ordinary C++17, and the GPU
renderer, whose kernel is compiled from the exact same struct definitions as
Metal Shading Language, a language that forbids virtual functions, RTTI, and
dynamic allocation. This chapter covers the plain structs in
`Core/ShaderTypes.h` that make that dual life possible, and `Core/Scene.hpp`
and `Core/Scene.cpp`, which use those structs to assemble the classic "Ray
Tracing in One Weekend" demo scene — deterministically, from a single seed —
as flat, parallel component arrays rather than a tree of shape objects.

**Files covered:** `Core/ShaderTypes.h`, `Core/Scene.hpp`, `Core/Scene.cpp`.

---

## §1. Why the data has to be plain

Every struct in `ShaderTypes.h` looks almost aggressively simple: no
constructors beyond aggregate initialization, no methods, no inheritance, no
`std::` containers, nothing but `int`, `float`, and `simd_float3` fields. That
plainness is not a style preference — it is the price of admission for a
struct that has to be `#include`d, byte-for-byte identical, into both a
plain C++ translation unit and an MSL kernel:

```cpp
// RayTracerBench/Core/ShaderTypes.h:1-9
#pragma once

// Plain, byte-for-byte-shared data passed between the CPU renderer and the
// GPU kernel's argument buffers. Included verbatim from both plain C++
// (CPURenderer, Scene, GPURenderer) and Raytracer.metal — <simd/simd.h>
// types keep layout identical on both sides.

#include <simd/simd.h>
```

The choice of `<simd/simd.h>` rather than a hand-rolled `Vec3` class is the
same idea applied to vectors specifically. Apple's `simd_float3` is one of
the few vector types whose memory layout, alignment, and (crucially) whose
*header* both a C++17 compiler and Apple's Metal compiler agree on natively.
A hand-rolled `struct Vec3 { float x, y, z; }` with operator overloads would
work fine on the CPU side, but there would be no guarantee its layout
matches whatever MSL's `float3` produces, and every operator would have to
be reimplemented (or macro-shimmed) for MSL syntax. Using `simd_float3`
directly sidesteps the whole question: the same eight-line file, unmodified,
is a valid header on both sides of the CPU/GPU boundary. This is the
foundation the rest of the shared-core design in Chapter 2 is built on top
of — `Core/RayTraceCore.h`'s dual compilation only works because the structs
it operates on already agree byte-for-byte.

## §2. Materials: the first tagged struct

`MaterialGPU` is the simplest of the shared structs, and it previews the
pattern the rest of the chapter is really about:

```cpp
// RayTracerBench/Core/ShaderTypes.h:10-23
enum MaterialType
{
	MAT_LAMBERTIAN = 0,
	MAT_METAL      = 1,
	MAT_DIELECTRIC = 2,
};

struct MaterialGPU
{
	int         type; // MaterialType
	simd_float3 albedo;
	float       fuzz; // metal roughness in [0,1]; unused for other types
	float       ir;   // dielectric index of refraction; unused for other types
};
```

In an object-oriented raytracer — including the CUDA port this project takes
structural precedent from — this would ordinarily be three classes under a
polymorphic `Material` base, each overriding a `scatter()` virtual method.
Here it's one struct with an integer tag and every field any material kind
might need, with a comment noting which fields a given `type` simply
ignores. `fuzz` means nothing for a lambertian material; `ir` means nothing
for a metal. That waste (a few unused floats per material) is the deliberate
trade for the thing MSL actually requires: no virtual dispatch. Chapter 2's
`scatter()` function reads this `type` field and switches on it, the same
shape of solution `ShapeGPU` below generalizes to geometry.

## §3. Geometry as components, not a class hierarchy

The comment directly above `ShapeType` in `ShaderTypes.h` states the
project's central architectural decision as plainly as it will be stated
anywhere:

```cpp
// RayTracerBench/Core/ShaderTypes.h:25-34
// Geometry is organized ECS-style: an entity is just an array index into SceneDescription's
// parallel `transforms`/`shapes` component arrays (see Scene.hpp) rather than a polymorphic
// "Sphere : Hittable" object — the same tagged-struct idea MaterialGPU/MaterialType already uses
// for materials, generalized to shapes. See CLAUDE.md for the full rationale.

enum ShapeType
{
	SHAPE_SPHERE  = 0,
	SHAPE_PYRAMID = 1,
};
```

"Ray Tracing in One Weekend" and its CUDA port both model a scene as a list
of `Hittable*` — spheres (and whatever else) behind a common virtual
interface, with `hit()` dispatched dynamically per object. That design is a
non-starter for a Metal kernel: MSL simply does not have virtual functions,
so a `Hittable` base class could never compile as a kernel argument type in
the first place. Rather than maintaining two divergent geometry
representations — a polymorphic one for the CPU and a flat one for the GPU —
RayTracerBench uses one representation everywhere, built out of the same
tagged-struct-plus-switch idiom that `MaterialGPU` already established for
materials. An "entity" here has no class of its own; it is nothing but a
shared index into a pair of parallel arrays, one holding each entity's
*Transform* component and one holding its *Shape* component — an
Entity-Component-System (ECS) layout in miniature. Adding a third primitive
later would mean adding one more `SHAPE_*` tag and one more switch case in
Chapter 2's `hitEntity()`, not a new subclass and not a new vtable.

## §4. The Transform component and its orientation basis

Every entity, regardless of what shape it is, carries a `TransformGPU`:

```cpp
// RayTracerBench/Core/ShaderTypes.h:36-47
// Transform component, shared by every entity regardless of shape. `position` is the entity's
// local origin (a sphere's center; a pyramid's base-square center). `right`/`up`/`forward` are an
// orthonormal orientation basis — the identity basis for spheres (rotationally symmetric, so
// orientation is meaningless for them) and an arbitrary rotation for pyramids, whose `up` axis
// points from base to apex.
struct TransformGPU
{
	simd_float3 position;
	simd_float3 right;
	simd_float3 up;
	simd_float3 forward;
};
```

The orientation is stored as three explicit basis vectors rather than as a
3×3 rotation matrix or a quaternion. A matrix would carry the same
information more compactly in one sense, and a quaternion would avoid any
risk of the three vectors drifting out of orthonormality — but both would
need supporting machinery (matrix inversion, quaternion-to-basis
conversion) that this project would then have to implement twice, once for
plain C++ and once in a dialect MSL is willing to compile. Storing
`right`/`up`/`forward` directly as three already-orthonormal vectors buys
something specific for Chapter 2's ray/shape intersection code: because the
inverse of an orthogonal matrix is just its transpose, projecting an
incoming ray's direction onto each of these three basis vectors *is*
already the inverse rotation — transforming a world-space ray into the
entity's local space needs nothing more than three dot products, and no
matrix inverse routine has to exist anywhere in this codebase. That
consuming code lives in `Core/RayTraceCore.h` and is Chapter 2's subject,
not this chapter's, but it's worth naming here because it's the reason this
particular representation was chosen over the more familiar alternatives.

For a sphere, orientation is physically meaningless — a sphere looks
identical under any rotation — so `Scene.cpp` (§8 below) just gives every
sphere the plain identity basis and it is never read by sphere intersection.
Pyramids are the shape that actually exercises this field.

## §5. The Shape component as a tagged union

`ShapeGPU` plays the same role for geometry that `MaterialGPU` plays for
shading — a fixed-size struct wide enough to hold whichever primitive's
parameters are needed, plus a reference to another component array:

```cpp
// RayTracerBench/Core/ShaderTypes.h:49-60
// Shape component: a tagged union of per-primitive geometry parameters plus a reference to this
// entity's Material component. Adding a new primitive means adding one more SHAPE_* tag and one
// more field group here, dispatched by a switch (see RayTraceCore.h's hitEntity()) rather than a
// new virtual base class — virtual dispatch is forbidden in MSL kernel code.
struct ShapeGPU
{
	int   type;          // ShapeType
	float radius;        // sphere: radius. pyramid: unused.
	float baseHalfWidth; // pyramid: half the base square's side length. sphere: unused.
	float height;        // pyramid: apex height above the base. sphere: unused.
	int   materialIndex; // index into SceneDescription::materials
};
```

`materialIndex` is the field that makes this a genuine component system
rather than just a struct-of-parameters: it is not a copy of a material, it
is a reference into a separate `materials` array, so many entities — the
whole randomized field of small spheres, for instance — can point at
distinct material records while entities that happen to share a look
(unlikely here, since materials are randomized per-entity, but architecturally
possible) could share one. It is exactly the same "index into another
array" idea that `transforms`/`shapes` themselves use to relate to an
entity, just one level removed: entity → shape → material.

## §6. Camera and per-frame parameters

Two more structs round out `ShaderTypes.h`, and neither needs the tagged-union
treatment because there is only ever one of each per render: `CameraGPU`
holds a fully precomputed viewport (origin, corners, basis vectors, lens
radius) —

```cpp
// RayTracerBench/Core/ShaderTypes.h:62-72
struct CameraGPU
{
	simd_float3 origin;
	simd_float3 lowerLeftCorner;
	simd_float3 horizontal;
	simd_float3 vertical;
	simd_float3 u;
	simd_float3 v;
	simd_float3 w;
	float       lensRadius;
};
```

— and `RenderParams` holds the handful of scalars that describe *how* to
render rather than *what* to render (image dimensions, sample count, bounce
depth, and the per-frame RNG seed):

```cpp
// RayTracerBench/Core/ShaderTypes.h:74-81
struct RenderParams
{
	uint32_t width;
	uint32_t height;
	uint32_t samplesPerPixel;
	uint32_t maxDepth;
	uint32_t frameSeed;
};
```

Precomputing the camera's viewport geometry into `CameraGPU` here, rather
than recomputing it from `vfov`/`aspectRatio`/etc. inside the per-pixel
kernel, means Chapter 2's `getRay()` does no trigonometry per ray — it only
needs to do vector arithmetic on values `Scene.cpp`'s `makeCamera()` (§7)
already worked out once, on the host, when the scene was built.

## §7. `SceneDescription`: the component arrays, assembled

`Scene.hpp` ties the individual structs above together into one object that
represents an entire scene:

```cpp
// RayTracerBench/Core/Scene.hpp:7-20
// ECS-style layout: an entity is simply an array index — transforms[i]/shapes[i] are that
// entity's Transform and Shape components (see ShaderTypes.h). There is no separate list per
// shape type; spheres and pyramids are interleaved in the same two parallel arrays and
// distinguished only by ShapeGPU::type, exactly like RayTraceCore.h's hitEntity() dispatches on it.
// Each entity's material is itself a component reference (ShapeGPU::materialIndex) into the
// separate `materials` component array, so many entities can share one material record.
struct SceneDescription
{
	std::vector<TransformGPU> transforms;
	std::vector<ShapeGPU>     shapes;
	std::vector<MaterialGPU>  materials;
	CameraGPU                 camera;
	RenderParams              params;
};
```

Note what is *not* here: there is no `std::vector<Sphere>` alongside a
`std::vector<Pyramid>`. Spheres and pyramids are interleaved in the same
`transforms`/`shapes` pair, in whatever order they were added, and the only
thing that tells them apart at render time is `shapes[i].type`. This is the
single representation both renderers share — `SceneDescription` is not
subclassed or specialized for the CPU or the GPU; the GPU renderer simply
uploads these same three vectors into Metal buffers, and the CPU renderer
walks them with raw pointers. There is no separate CPU/GPU scene
representation to keep in sync.

`Scene.hpp` also declares the one function that builds a `SceneDescription`,
`buildDefaultScene()`, with a doc comment that lays out its full contract —
determinism from a seed, the shape of the classic demo scene, and the
`floating` parameter this chapter returns to in §11:

```cpp
// RayTracerBench/Core/Scene.hpp:22-34
// Builds the classic "Ray Tracing in One Weekend" demo scene — a ground sphere, a randomized
// ~22x22 grid of small spheres, three large feature spheres (glass / lambertian / metal), and a
// handful of square pyramids at varied 3D orientations — deterministically for a given seed via a
// seeded std::mt19937. Both the CPU and GPU renderers consume this exact same SceneDescription, so
// identical seeds produce identical entity layouts/materials on both (per-pixel noise still
// differs — see CLAUDE.md's verification notes).
//
// `floating`, when true, places the small randomized-field spheres at a random height in
// [radius, kMaxFloatHeight] (see Scene.cpp) instead of resting on the ground — kMaxFloatHeight was
// chosen and verified by rendering the fixed camera setup below and confirming nothing clips out
// of frame, not derived from an unverified formula. The three large feature spheres (glass /
// lambertian / metal) and the pyramids always rest on the ground regardless of `floating`.
SceneDescription buildDefaultScene( unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, bool floating = false );
```

## §8. `Scene.cpp` is host-only, and gets to use the full `simd` API

Everything from here on is `Scene.cpp`, and its opening comment is worth
reading before any of its code, because it explains a small but easily
mixed-up asymmetry with Chapter 2's `RayTraceCore.h`:

```cpp
// RayTracerBench/Core/Scene.cpp:7-9
// Scene.cpp is pure host-side C++ (never compiled as MSL), so — unlike Core/RayTraceCore.h —
// it can freely use the full simd_* C API (simd_make_float3, simd_normalize, simd_cross,
// simd_length) instead of hand-rolled vector helpers.
```

`Core/RayTraceCore.h` (Chapter 2) is `#include`d verbatim into an MSL
translation unit, and Apple's Metal compiler does not accept
function-call-style construction like `simd_float3(x, y, z)` — only
brace-initialization or `simd_make_float3` compile under MSL, even though a
plain C++ compiler is happy to accept the constructor-style call too. Rather
than rely on that inconsistency case by case, `RayTraceCore.h` defines its
own small `makeFloat3()` helper and uses it everywhere a 3-vector literal is
needed, so the exact same line of source is guaranteed to compile
identically under both toolchains. `Scene.cpp`, by contrast, is compiled
exactly once, only ever by a normal C++17 compiler — it is part of the
framework-free `RayTracerCore` library and never touched by `xcrun -sdk
macosx metal` — so it has no reason to route around anything. It calls
`simd_make_float3`, `simd_normalize`, `simd_cross`, and `simd_length`
straight from `<simd/simd.h>`'s own C API throughout, which is exactly what
the code excerpts below show it doing.

## §9. `addEntity()`: spawning an entity is pushing two components

The whole ECS pattern collapses to one small function once you have the
parallel arrays already declared:

```cpp
// RayTracerBench/Core/Scene.cpp:55-62
// Spawns one entity: pushes its Transform and Shape components onto SceneDescription's
// parallel component arrays at the same index — the ECS analogue of "instantiate an entity
// with these components" (see ShaderTypes.h/Scene.hpp for the component layout).
void addEntity( SceneDescription& scene, TransformGPU transform, ShapeGPU shape )
{
	scene.transforms.push_back( transform );
	scene.shapes.push_back( shape );
}
```

There is no entity object returned, no handle, nothing to hold onto — the
entity's identity *is* its position in these two vectors, which is exactly
why `transforms` and `shapes` must always be pushed to together, in lock
step, and never independently. `addMaterial()` just above it in the file
follows the identical shape for the third component array:

```cpp
// RayTracerBench/Core/Scene.cpp:48-53
// Appends a material to the materials component array and returns its index.
int addMaterial( std::vector<MaterialGPU>& materials, MaterialGPU mat )
{
	materials.push_back( mat );
	return static_cast<int>( materials.size() - 1 );
}
```

— except that this one *does* return something, the freshly-assigned index,
because unlike a Transform/Shape pair a material is meant to be referenced
by index from elsewhere (`ShapeGPU::materialIndex`), not just spawned.

## §10. Building the scene: ground, field, and the three feature spheres

`buildDefaultScene()` follows the book's own scene almost line for line. It
seeds one `std::mt19937` from the caller's `seed` and draws everything —
material choices, colors, jitter, and (per §11) placement heights — from
that single generator, which is what makes the whole scene reproducible from
a seed alone. The ground is a giant sphere pushed far below the visible
area:

```cpp
// RayTracerBench/Core/Scene.cpp:163-169
	// Ground.
	{
		MaterialGPU ground{ MAT_LAMBERTIAN, simd_make_float3( 0.5f, 0.5f, 0.5f ), 0.0f, 0.0f };
		int         groundMat = addMaterial( scene.materials, ground );
		addEntity( scene, identityTransformAt( simd_make_float3( 0.0f, -1000.0f, 0.0f ) ),
			ShapeGPU{ SHAPE_SPHERE, 1000.0f, 0.0f, 0.0f, groundMat } );
	}
```

Then the randomized ~22×22 field of small spheres, each independently rolling
a material (80% lambertian, 15% metal, 5% dielectric, matching the book's own
split) and skipping the cell nearest the large feature spheres so nothing
overlaps them:

```cpp
// RayTracerBench/Core/Scene.cpp:173-205
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
			addEntity( scene, identityTransformAt( center ), ShapeGPU{ SHAPE_SPHERE, 0.2f, 0.0f, 0.0f, matIndex } );
		}
	}
```

and, always resting on the ground regardless of `floating`, the three large
feature spheres — one of each material, the classic RTIOW hero shot:

```cpp
// RayTracerBench/Core/Scene.cpp:207-222
	// Three large feature spheres: glass, lambertian, metal. Always resting on the ground,
	// regardless of `floating` — only the small randomized field floats.
	{
		MaterialGPU glass{ MAT_DIELECTRIC, simd_make_float3( 0.0f, 0.0f, 0.0f ), 0.0f, 1.5f };
		int         glassMat = addMaterial( scene.materials, glass );
		addEntity( scene, identityTransformAt( simd_make_float3( 0.0f, 1.0f, 0.0f ) ), ShapeGPU{ SHAPE_SPHERE, 1.0f, 0.0f, 0.0f, glassMat } );

		MaterialGPU diffuse{ MAT_LAMBERTIAN, simd_make_float3( 0.4f, 0.2f, 0.1f ), 0.0f, 0.0f };
		int         diffuseMat = addMaterial( scene.materials, diffuse );
		addEntity( scene, identityTransformAt( simd_make_float3( -4.0f, 1.0f, 0.0f ) ),
			ShapeGPU{ SHAPE_SPHERE, 1.0f, 0.0f, 0.0f, diffuseMat } );

		MaterialGPU metal{ MAT_METAL, simd_make_float3( 0.7f, 0.6f, 0.5f ), 0.0f, 0.0f };
		int         metalMat = addMaterial( scene.materials, metal );
		addEntity( scene, identityTransformAt( simd_make_float3( 4.0f, 1.0f, 0.0f ) ), ShapeGPU{ SHAPE_SPHERE, 1.0f, 0.0f, 0.0f, metalMat } );
	}
```

Every sphere in both the ground/field/feature groups is placed with
`identityTransformAt()`, the small helper mentioned in §4 that fills
`right`/`up`/`forward` with the identity basis because a sphere has no
orientation to speak of:

```cpp
// RayTracerBench/Core/Scene.cpp:64-74
	// Spheres are rotationally symmetric, so their Transform component only ever uses `position`;
	// `right`/`up`/`forward` are set to the identity basis and simply ignored by hitSphere().
	TransformGPU identityTransformAt( simd_float3 position )
	{
		TransformGPU t;
		t.position = position;
		t.right = simd_make_float3( 1.0f, 0.0f, 0.0f );
		t.up = simd_make_float3( 0.0f, 1.0f, 0.0f );
		t.forward = simd_make_float3( 0.0f, 0.0f, 1.0f );
		return t;
	}
```

## §11. `floating`, and why its RNG draws are ordered so carefully

The `floating` parameter is the one piece of `buildDefaultScene()`'s
behavior that postdates the base scene, and its implementation is written
specifically to avoid perturbing every scene that was already relying on the
old, ground-only layout for a given seed. `placementHeight()` is the small
function that decides, per small-field sphere, where its `y` actually ends
up:

```cpp
// RayTracerBench/Core/Scene.cpp:76-89
	// Chosen and verified by rendering the fixed camera setup below (lookfrom (13,2,3), lookat
	// origin, vfov 20°) at several candidate heights and confirming by eye that nothing floats out
	// of frame — not derived from an unverified formula. Applies only to the small randomized-field
	// spheres; the three large feature spheres always rest on the ground regardless of `floating`.
	constexpr float kMaxFloatHeight = 3.0f;

	// Picks a random height in [radius, kMaxFloatHeight] when floating, or the given resting
	// height (touching the ground) otherwise.
	float placementHeight( bool floating, float radius, float restingHeight, std::mt19937& rng, std::uniform_real_distribution<float>& unit )
	{
		if ( !floating )
			return restingHeight;
		return radius + unit( rng ) * ( kMaxFloatHeight - radius );
	}
```

The critical detail is *where* this function is called relative to the RNG
draws around it, back at the call site in the field loop:
`center.y = placementHeight( floating, 0.2f, center.y, rng, unit );` runs
*after* `chooseMat`, the albedo/fuzz draws, and the `x`/`z` jitter have
already all been consumed for that cell — and, in the `floating == false`
branch, `placementHeight()` makes no `unit(rng)` call at all. That ordering
is what lets the doc comment in `Scene.hpp` promise "identical seeds produce
identical entity layouts" as a *strict* guarantee for existing callers: with
`floating` defaulting to `false`, the sequence of RNG draws `buildDefaultScene()`
makes for a given seed is byte-for-byte the same sequence it made before
`floating` existed, so nothing already depending on a seed's exact layout
(rendered images, saved exports, tests) shifts underneath it. Only when a
caller opts into `floating = true` does the extra `unit(rng)` draw for
height get inserted — and only ever for the small field spheres, never for
the ground, the three feature spheres, or the pyramids, which are always
built by the unconditional resting-position paths in §12.

`kMaxFloatHeight`'s value of `3.0f` is worth pausing on for what it is
*not*: it was not derived by working out the camera frustum's bounds
analytically and solving for the height at which an object would just start
clipping out of frame. It was chosen empirically — render the fixed
`lookfrom (13,2,3)` / `lookat origin` / `vfov 20°` camera at a few candidate
heights and look at the result. Section §12's pyramid placement takes the
opposite approach for a structurally similar problem, and the contrast is
the more instructive story.

## §12. Pyramids: an orientation that means something, and a resting height with an exact answer

Spheres cannot demonstrate `TransformGPU`'s `right`/`up`/`forward` basis
doing anything, since a sphere is invariant under rotation. Pyramids are
added specifically because they can:

```cpp
// RayTracerBench/Core/Scene.cpp:224-230
	// Square pyramids, at a spread of 3D orientations — the shape group that actually exercises
	// TransformGPU's orientation basis, since a sphere looks identical no matter how it's rotated.
	// Always resting on the ground (via makeRestingPyramidTransform's exact per-orientation
	// computation above), never affected by `floating`, and never dielectric: hitPyramidLocal only
	// resolves the ray's entry face, so a ray whose origin starts inside the solid — which glass
	// refraction requires once a ray exits the far side — isn't handled. That's an explicit,
	// documented scope limit for this primitive, not an oversight.
```

`makeOrientedBasis()` builds each pyramid's basis from two independent
rotations — a tilt around the local Z axis away from vertical, then a yaw
around world Y — deliberately so the five pyramids in the demo scene can
show genuinely different 3D poses (some merely spun in place, some visibly
tipped over) rather than variations on a single spin axis:

```cpp
// RayTracerBench/Core/Scene.cpp:91-114
	// Builds an orthonormal orientation basis for a pyramid: tilt the identity "up" axis away from
	// vertical by `tiltDegrees` around the local Z axis, then yaw the whole basis around world Y by
	// `yawDegrees` — two independent rotations so pyramids can show genuinely different 3D
	// orientations (some merely spun in place, some visibly tipped over), not just a spin around
	// one axis.
	void makeOrientedBasis( float yawDegrees, float tiltDegrees, simd_float3& outRight, simd_float3& outUp, simd_float3& outForward )
	{
		float tilt = tiltDegrees * kPi / 180.0f;
		simd_float3 up = simd_make_float3( std::sin( tilt ), std::cos( tilt ), 0.0f );
		simd_float3 right = simd_make_float3( std::cos( tilt ), -std::sin( tilt ), 0.0f );
		simd_float3 forward = simd_make_float3( 0.0f, 0.0f, 1.0f );

		float yaw = yawDegrees * kPi / 180.0f;
		float cosY = std::cos( yaw );
		float sinY = std::sin( yaw );

		auto rotateY = [ cosY, sinY ]( simd_float3 v ) {
			return simd_make_float3( v.x * cosY + v.z * sinY, v.y, -v.x * sinY + v.z * cosY );
		};

		outRight = rotateY( right );
		outUp = rotateY( up );
		outForward = rotateY( forward );
	}
```

Once a pyramid can be tilted at an arbitrary angle, "resting on the ground"
stops being as trivial as setting `position.y` to a constant — a tilted
pyramid's lowest point might be the apex, or it might be one of the four
base corners, depending on the tilt. Unlike `kMaxFloatHeight`'s frustum
question in §11, *this* geometric question does have a closed form: a
pyramid has exactly five vertices in local space (the apex plus four base
corners), and once you know the orientation basis, the world-space height of
each is just a dot product against `up`. The lowest of those five numbers is
the exact amount the pyramid needs to be lifted (or dropped) so it touches
the ground precisely, at any orientation, with no iteration and no
eyeballing:

```cpp
// RayTracerBench/Core/Scene.cpp:116-149
	// Places a pyramid's Transform so its lowest vertex (the apex or one of the 4 base corners,
	// whichever ends up lowest once rotated) touches `groundY` exactly, regardless of orientation —
	// an exact closed-form computation from the 5 local vertices, unlike kMaxFloatHeight above
	// (which needed a rendered check against the camera frustum and couldn't be solved in closed
	// form). This is what lets every pyramid below rest flush on the ground at any tilt without a
	// per-orientation eyeball check.
	TransformGPU makeRestingPyramidTransform( simd_float3 groundXZ, float groundY, float yawDegrees, float tiltDegrees,
		float baseHalfWidth, float height )
	{
		simd_float3 right, up, forward;
		makeOrientedBasis( yawDegrees, tiltDegrees, right, up, forward );

		const simd_float3 localVerts[ 5 ] = {
			simd_make_float3( 0.0f, height, 0.0f ),
			simd_make_float3( baseHalfWidth, 0.0f, baseHalfWidth ),
			simd_make_float3( baseHalfWidth, 0.0f, -baseHalfWidth ),
			simd_make_float3( -baseHalfWidth, 0.0f, -baseHalfWidth ),
			simd_make_float3( -baseHalfWidth, 0.0f, baseHalfWidth ),
		};

		float minWorldY = 1.0e30f;
		for ( const simd_float3& v : localVerts )
		{
			float worldY = v.x * right.y + v.y * up.y + v.z * forward.y;
			minWorldY = std::min( minWorldY, worldY );
		}

		TransformGPU transform;
		transform.position = simd_make_float3( groundXZ.x, groundY - minWorldY, groundXZ.z );
		transform.right = right;
		transform.up = up;
		transform.forward = forward;
		return transform;
	}
```

`kMaxFloatHeight` and this function are the same chapter's two answers to
structurally similar-sounding questions — "how high can something go before
it looks wrong" versus "how low does something sit given its orientation" —
and the fact that one was solved by rendering-and-looking while the other
was solved algebraically is not an inconsistency; it reflects which question
actually admits a closed-form answer. The five local vertices these
world-space heights are computed from — one apex, four base corners — are
exactly the same five points `Core/RayTraceCore.h`'s `hitPyramidLocal()`
(Chapter 2) derives its five half-space planes from, and the same five
points Chapter 6's `EntityMesh.hpp` triangulates into a faceted mesh: this
one geometric definition of "what a pyramid is" is reused for placement,
intersection, and export alike.

With the basis and resting transform in hand, the five demo pyramids are
declared as a small table of specs — ground position, yaw, tilt, color, and
whether it's metal — and built in a loop, each getting its own material and
its `ShapeGPU` filled with `baseHalfWidth`/`height` (its `radius` field left
at `0.0f`, unused, exactly as the comment in §5 documents):

```cpp
// RayTracerBench/Core/Scene.cpp:232-259
		struct PyramidSpec
		{
			simd_float3 groundXZ;
			float       yawDegrees;
			float       tiltDegrees;
			simd_float3 albedo;
			bool        metal;
		};

		const PyramidSpec pyramids[] = {
			{ simd_make_float3( -2.2f, 0.0f, 1.8f ), 0.0f, 0.0f, simd_make_float3( 0.85f, 0.2f, 0.2f ), false },
			{ simd_make_float3( 2.2f, 0.0f, 1.8f ), 40.0f, 0.0f, simd_make_float3( 0.2f, 0.8f, 0.3f ), false },
			{ simd_make_float3( 0.0f, 0.0f, -2.0f ), 20.0f, 25.0f, simd_make_float3( 0.3f, 0.4f, 0.9f ), false },
			{ simd_make_float3( -4.5f, 0.0f, 2.2f ), 70.0f, -20.0f, simd_make_float3( 0.75f, 0.75f, 0.2f ), false },
			{ simd_make_float3( 4.5f, 0.0f, 2.2f ), 55.0f, 35.0f, simd_make_float3( 0.7f, 0.7f, 0.7f ), true },
		};

		const float baseHalfWidth = 0.6f;
		const float pyramidHeight = 1.2f;

		for ( const PyramidSpec& p : pyramids )
		{
			MaterialGPU mat = p.metal ? MaterialGPU{ MAT_METAL, p.albedo, 0.15f, 0.0f } : MaterialGPU{ MAT_LAMBERTIAN, p.albedo, 0.0f, 0.0f };
			int         matIndex = addMaterial( scene.materials, mat );

			TransformGPU transform = makeRestingPyramidTransform( p.groundXZ, 0.0f, p.yawDegrees, p.tiltDegrees, baseHalfWidth, pyramidHeight );
			addEntity( scene, transform, ShapeGPU{ SHAPE_PYRAMID, 0.0f, baseHalfWidth, pyramidHeight, matIndex } );
		}
```

Note also that these five specs are fixed, hand-picked constants, not drawn
from `rng` — so adding pyramids to the scene draws no extra random numbers
at all, which is one more reason `floating`'s backward-compatibility
argument in §11 only had to reason about the field-sphere loop.

## §13. Wiring up the camera and finishing the scene

The function's last steps build the one `CameraGPU` the whole scene shares,
using the `makeCamera()` helper mentioned in §6, and fill in `RenderParams`
from the caller's arguments:

```cpp
// RayTracerBench/Core/Scene.cpp:262-275
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
```

`(13, 2, 3)` looking at the origin with a 20° vertical field of view is the
exact camera the book itself uses for this scene, and it's also the fixed
setup `kMaxFloatHeight` in §11 was verified against by rendering — so that
verification and this camera declaration describe the same view, just from
two different places in the file. The returned `SceneDescription` at this
point is complete and self-contained: every entity's Transform and Shape
sit at matching indices in `transforms`/`shapes`, every `materialIndex`
points somewhere valid in `materials`, and `camera`/`params` are filled in —
ready to be hand to either renderer with nothing further to translate.

---

## Where this connects

**Chapter 2, The Shared Ray-Tracing Core**, is where these structs stop
being passive data. `Core/RayTraceCore.h`'s `hitEntity()` is the direct
consumer of `transforms[]`/`shapes[]` walked in lockstep — the tagged-switch
dispatch this chapter set up the vocabulary for — and its `hitPyramid()`
is where projecting onto `TransformGPU`'s orthonormal basis (§4) actually
pays off as "no matrix inverse needed." That same file's `scatter()` is the
tagged dispatch over `MaterialGPU::type` this chapter's §2 previewed.

**Chapter 6, Mesh Generation for Export**, reuses this exact same Transform +
Shape data for a completely different purpose: `Export/EntityMesh.hpp`'s
`buildEntityMesh()` is a third system (alongside `hitEntity()`'s ray
intersection and `scatter()`'s shading) that dispatches on `ShapeGPU::type`
the same way, but produces a triangle mesh instead of a hit test — including
building pyramid meshes from the identical five local vertices
`makeRestingPyramidTransform()` (§12) already used to figure out where to
place them.
