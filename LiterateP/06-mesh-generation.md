# Chapter 6: Mesh Generation for Export

**Abstract.** `Core/RayTraceCore.h` (Chapter 2) already knows how to answer two
questions about an entity's Transform and Shape components: "does this ray hit
it?" (`hitEntity()`) and "how does light scatter off it?" (`scatter()`). This
chapter covers a third question asked of the exact same two components: "what
triangle mesh does this entity look like?" `Export/EntityMesh.hpp/.cpp`
answers it with `buildEntityMesh()`, a function with no ray in sight — it
exists purely to hand `Export/SceneExporter.hpp/.cpp` (Chapter 7) real
geometry to write into a glTF or OBJ file. The interesting design decisions
here are architectural (this is the same tagged-dispatch ECS pattern doing a
third job, not a new idea), geometric (spheres need approximating, pyramids
don't — and reuse the same 5 local vertices Chapter 2's `hitPyramidLocal()`
already derived), and methodological (triangle winding was hand-derived and
then caught wrong by a test before it ever shipped).

**Files covered:** `RayTracerBench/Export/EntityMesh.hpp`,
`RayTracerBench/Export/EntityMesh.cpp`. `RayTracerBenchTests/EntityMeshTests.cpp`
is cited as the verification for §5 below and covered fully in Chapter 11.

---

## §1. A third ECS system over the same two components

Chapter 2 introduced `hitEntity()` as "the collision system": it takes an
entity's `TransformGPU` and `ShapeGPU` components, switches on
`ShapeGPU::type`, and dispatches to `hitSphere()` or `hitPyramid()`. The
project status notes in `CLAUDE.md` describe `scatter()`'s tagged switch over
`MaterialGPU::type` the same way — a second system, dispatching on a
different component, doing a different job (shading instead of intersection).
`buildEntityMesh()` is explicitly the third member of that family, and the
header says so directly:

```cpp
// Builds a world-space mesh for one entity's Transform+Shape components, dispatching on the Shape
// component's tag — the mesh-export analogue of RayTraceCore.h's hitEntity() intersection system:
// same ECS dispatch-by-tag idea (see CLAUDE.md), a different "system" operating over the same
// components. Spheres are tessellated (no native sphere primitive in glTF/OBJ); pyramids are
// exact, faceted geometry (flat per-triangle normals — see buildPyramidMesh's winding-order note).
MeshData buildEntityMesh( TransformGPU transform, ShapeGPU shape );
```
`RayTracerBench/Export/EntityMesh.hpp:17-22`

The implementation's own dispatch function reads almost like a restatement of
`hitEntity()` with the ray parameters removed:

```cpp
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
```
`RayTracerBench/Export/EntityMesh.cpp:144-156`

Compare this against `hitEntity()` itself (Chapter 2):

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

The shape of the two functions is identical: switch on `shape.type`, dispatch
to one of two per-shape helpers, fall through to a harmless default if the tag
is ever something else. This is the payoff of `ShaderTypes.h`'s ECS design
(Chapter 1) actually being followed through rather than just used once. The
comment on `ShapeGPU` itself frames adding a primitive as "one more `SHAPE_*`
tag and one more field group... dispatched by a switch... rather than a new
virtual base class" (`RayTracerBench/Core/ShaderTypes.h:49-52`) — and that
promise now has to hold for *three* switches, not one, every time a new
primitive is added: one in `hitEntity()`, one in `scatter()`'s material
counterpart, and one here. `buildEntityMesh()` is the proof that the pattern
generalizes past the two cases it was designed against.

## §2. Framework-free, and why that matters here specifically

`EntityMesh.cpp` opens with a comment placing it alongside `Core/Scene.cpp`:

```cpp
// Pure host-side C++ (never compiled as MSL), like Core/Scene.cpp — free to use the full simd_*
// C API directly instead of RayTraceCore.h's hand-rolled portable helpers.
```
`RayTracerBench/Export/EntityMesh.cpp:5-6`

Unlike `RayTraceCore.h`, this file is never dual-compiled into MSL — there is
no GPU-side use for a triangle mesh, since neither renderer draws triangles;
they both ray-march the analytic sphere/pyramid primitives directly. That
frees `EntityMesh.cpp` to call `simd_make_float3`, `simd_normalize`, and
`simd_cross` straight from Apple's `<simd/simd.h>`, the same C API
`Core/Scene.cpp` uses (Chapter 1), rather than routing through
`RayTraceCore.h`'s `makeFloat3()`/`normalize3()`/`dot3()` helpers that exist
only to paper over MSL's stricter constructor rules.

`CMakeLists.txt` backs up the "framework-free" claim structurally, not just in
prose: `EntityMesh.cpp` is compiled into the same static library as
`Core/Scene.cpp` and `CPU/CPURenderer.cpp`, not the `RayTracerBench` app
target:

```cmake
# RayTraceCore.h and ShaderTypes.h are header-only and dual-compiled into
# Shaders/Raytracer.metal directly (not through this library) once the GPU
# renderer exists. CPURenderer.cpp is framework-free C++17 like Core/, so it
# lives in the same static library rather than a separate CMake target.
add_library(RayTracerCore STATIC
    RayTracerBench/Core/Scene.cpp
    RayTracerBench/CPU/CPURenderer.cpp
    RayTracerBench/Export/EntityMesh.cpp
    RayTracerBench/Export/SceneExporter.cpp
)
```
`CMakeLists.txt:20-29`

`Export/ImageWriter.cpp` — the sibling module that actually needs
CoreGraphics/ImageIO to write PNG bytes (Chapter 7) — is conspicuously *not*
in that list; it's built only into the `RayTracerBench` executable target
further down the same file (`CMakeLists.txt:37-46`), alongside the AppKit and
Metal sources. `EntityMesh.cpp` needing neither Metal nor AppKit nor even
CoreGraphics is what earns it a place in `RayTracerCore` instead: it can be
linked into `RayTracerBenchTests` (Chapter 11) and exercised as a plain
command-line C++ program, with no GPU device, no window server, and no macOS
UI session required to run its tests — which is exactly the environment this
project's test binary runs in (see `CLAUDE.md`'s note that
`RayTracerBenchTests` is "a plain command-line C++ tool"). A module that
needed Metal or AppKit to construct a mesh would have dragged that dependency
into every test run for no reason the mesh-generation logic itself needs.

## §3. Sphere tessellation: a UV grid standing in for a primitive neither format has

Both glTF and OBJ describe geometry as triangle meshes; neither has a native
analytic sphere. `buildSphereMesh()` approximates one with a latitude/longitude
grid, at a fixed resolution chosen as a deliberate size/quality tradeoff (the
comment cites the ~480-sphere randomized field as the dominant cost, per
`CLAUDE.md`):

```cpp
// Tessellation density for exported spheres: no native sphere primitive exists in either
// glTF or OBJ, so every sphere (including the giant ground sphere) is approximated as a UV
// grid. Chosen as a size/smoothness balance after estimating the full scene's export size at
// a few candidate densities (see CLAUDE.md) — the ~480-sphere randomized field dominates
// total triangle count, so doubling this from 8x12 would roughly double the file size for a
// visually marginal smoothness gain at typical viewing distance.
constexpr int kSphereLatSegments = 8;
constexpr int kSphereLonSegments = 12;
```
`RayTracerBench/Export/EntityMesh.cpp:12-19`

The grid itself walks latitude from the south pole to the north pole (`theta`
from `-π/2` to `+π/2`) and longitude all the way around (`phi` from `0` to
`2π`), generating one vertex per `(lat, lon)` pair with an outward radial
normal equal to its own position direction — this is what makes the sphere
*smooth*-shaded rather than faceted, unlike the pyramid in §4:

```cpp
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
```
`RayTracerBench/Export/EntityMesh.cpp:32-45`

## §4. The pole-row skip

A naive UV grid turns each `(lat, lon)` quad into two triangles by connecting
its four corners. That works everywhere except the two end rows. At
`lat == 0`, every vertex at every longitude sits at the same point — the south
pole — because `theta = -π/2` collapses `cosTheta` to zero regardless of
`phi`; symmetrically, at `lat == latN` every longitude collapses to the north
pole. A quad that touches one of those rows therefore has two of its four
corners identical in world space, and the triangle built from *both*
collapsed corners has zero area — three vertices, two of which are the same
point, is a degenerate triangle that contributes nothing to the rendered mesh
but does bloat the file and can confuse loaders that assume non-degenerate
input.

`buildSphereMesh()` handles this not by special-casing the pole rows
separately, but by observing that each grid quad emits *two* triangles
sharing a diagonal, and only one of those two ever touches both collapsed
corners at once:

```cpp
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
```
`RayTracerBench/Export/EntityMesh.cpp:49-78`

Walk it through concretely. At the very bottom row, `lat == 0`: `v00` and
`v01` are both the south pole (`v00 == v01`, since every `lon` maps to the
same collapsed point at that latitude), while `v10`/`v11` are two distinct,
real vertices one latitude ring up. The triangle `(v00, v10, v11)` uses only
one copy of the pole and two genuinely distinct points — that's a real,
non-degenerate triangle, and it's the one the `if ( lat != latN - 1 )` branch
emits (that condition is unrelated to this row; it's guarding the *other*
pole at the top). The triangle `(v00, v11, v01)`, by contrast, would use
`v00` and `v01` — the same point twice — plus `v11`; that's the zero-area
triangle, and the `if ( lat != 0 )` guard skips it exactly at `lat == 0`. The
symmetric argument holds at the top row, `lat == latN - 1`, where `v10 ==
v11` is now the collapsed pair and it's the *first* `if` that guards against
emitting it. Each pole ring therefore contributes exactly one real triangle
per longitude segment instead of two, and no triangle with a repeated vertex
ever reaches `mesh.indices`.

## §5. Pyramids: one geometric truth feeding two systems

Where a sphere needs approximating, a pyramid does not — `hitPyramidLocal()`
in Chapter 2 already has an *exact* representation of the pyramid's solid as
the intersection of five half-spaces (one base, four sides), each derived in
closed form from `baseHalfWidth`/`height`:

```cpp
const simd_float3 normals[ 5 ] = {
	makeFloat3( 0.0f, -1.0f, 0.0f ),        // base
	makeFloat3( height, halfWidth, 0.0f ),  // +X side
	makeFloat3( -height, halfWidth, 0.0f ), // -X side
	makeFloat3( 0.0f, halfWidth, height ),  // +Z side
	makeFloat3( 0.0f, halfWidth, -height ), // -Z side
};
```
`RayTracerBench/Core/RayTraceCore.h:292-298`

`buildPyramidMesh()` doesn't re-derive that geometry from scratch or
tessellate an approximation of it — it builds its mesh from the *same* five
local points (apex plus four base corners) that those plane equations
implicitly define, restated explicitly as vertex positions this time instead
of half-space normals:

```cpp
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
```
`RayTracerBench/Export/EntityMesh.cpp:102-141`

This is deliberately "one geometric truth, two systems": `hitPyramidLocal()`
answers "does a ray cross this half-space boundary" using the plane's normal
and distance, while `buildPyramidMesh()` answers "what does this solid's
surface look like as triangles" using the same corners those planes pass
through — the pyramid's shape is defined exactly once (as `baseHalfWidth` and
`height` in local space), and both the intersection system and the export
system read off that single definition rather than each carrying its own
copy that could drift out of sync. The world-space transform is likewise the
same convention `hitPyramid()` uses: `toWorld()`'s
`position + x*right + y*up + z*forward` (`EntityMesh.cpp:125-127`) is the
forward half of exactly the same orthonormal-basis projection
`hitPyramid()` runs in the other direction to bring a world-space ray *into*
local space (`RayTraceCore.h:356-360`).

The mesh this produces is faceted rather than smooth: `addFlatTriangle()`
gives each of the six triangles (four sides plus two base triangles) its own
private three vertices and one shared face normal computed directly from
that triangle's own edges, rather than sharing vertices — and therefore
normals — across faces the way the sphere does:

```cpp
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
```
`RayTracerBench/Export/EntityMesh.cpp:83-100`

That's the correct choice for a pyramid's actual geometry: a real pyramid
has hard edges between its faces, and averaging normals across them (as a
smooth-shaded sphere legitimately does) would fake a curvature the shape
doesn't have.

## §6. Winding order: hand-derived, then caught wrong by a test

The comment on `buildPyramidMesh()` above is explicit about method: the side
faces' winding, `triangle(C[i], C[(i+1)%4], apex)`, was worked out by hand —
computing `cross(C[i+1]-C[i], apex-C[i])` and checking the result against
each face's already-known outward normal from `hitPyramidLocal()`'s plane
comment (e.g. the +X face's `(height, halfWidth, 0)`) — and the base's two
triangles, `(C0,C2,C1)` and `(C0,C3,C2)`, were derived the same way against
the known base normal `(0,-1,0)`. The sphere's winding comment makes the
identical claim about method: "winding chosen so `cross(v10-v00, v11-v00)`
etc. point outward" (`EntityMesh.cpp:49-50`).

But `CLAUDE.md`'s own account of this module is blunt about hand-derivation
alone not being trusted: "Both mesh generators' winding orders were derived
by hand and then verified programmatically... rather than trusted by
inspection — an initial sphere-winding attempt was in fact backwards and
caught this way." In other words, the first version of `buildSphereMesh()`'s
triangle winding — worked out by exactly the same by-hand cross-product
reasoning documented in the comments above — was actually wrong, and it was
a test, not a code reviewer or a 3D viewer, that caught it.

That test lives in `RayTracerBenchTests/EntityMeshTests.cpp` (Chapter 11
covers the whole test suite; this is the specific check relevant here). It
doesn't re-derive the correct winding by hand and compare — it defines
"correct" operationally, as "every triangle's face normal points away from a
point known to be inside the shape":

```cpp
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
```
`RayTracerBenchTests/EntityMeshTests.cpp:23-40`

For a sphere the interior point is trivially its own center
(`EntityMeshTests.cpp:62`, `allTrianglesFaceAwayFrom( mesh, center )`); for a
pyramid it's "a point just above the base center," valid for any height
greater than 0.4 (`EntityMeshTests.cpp:91-92`). Both tests run every single
triangle in the generated mesh through this check, not a hand-picked sample —
which is precisely the exhaustiveness a by-eye inspection or a one-off
hand-computed cross product can't offer, and precisely why the backwards
sphere winding surfaced here instead of shipping. A third test in the same
file, `buildEntityMesh_pyramid_respectsOrientationBasis`
(`EntityMeshTests.cpp:95-122`), checks that a pyramid rotated so its local
"up" points along world +X actually places its apex at world `(1,0,0)` —
confirming `toWorld()`'s basis projection, not just the winding, survives an
arbitrary orientation.

---

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) defines the
  `TransformGPU`/`ShapeGPU` components this chapter's `buildEntityMesh()`
  consumes — the same two components every system in this project reads,
  never a mesh-specific representation of its own.
- **Chapter 2** (`Core/RayTraceCore.h`) is where `hitEntity()` and `scatter()`
  establish the tagged-dispatch pattern `buildEntityMesh()` reuses, and where
  `hitPyramidLocal()`'s five half-space planes are the same geometric
  definition `buildPyramidMesh()` reads its five vertices from (§5 above).
- **Chapter 7** (`Export/SceneExporter.hpp/.cpp`) is `buildEntityMesh()`'s
  only caller: it invokes it once per entity — see
  `RayTracerBench/Export/SceneExporter.cpp:237` and `:317` — to assemble the
  full scene's geometry into a glTF or OBJ+MTL file, and is also where the
  companion preview PNG (`Export/ImageWriter.hpp/.cpp`) gets written into the
  same output directory.
- **Chapter 11** (`RayTracerBenchTests/*`) covers `EntityMeshTests.cpp` in
  full, including the degenerate-triangle and vertex-on-surface checks for
  spheres and the exact-triangle-count check for pyramids that this chapter
  only used selectively (§6) to narrate the winding-order bug.
