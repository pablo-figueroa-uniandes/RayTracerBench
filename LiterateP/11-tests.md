# Chapter 11 — Tests: Parity, Geometry, and the Core Itself

**Abstract.** `RayTracerBenchTests` is the project's one committed test binary: a
plain command-line C++ tool with no XCTest and no Objective-C, built from a
homegrown ~100-line test framework rather than a vendored one. It exercises
three different layers of the program — Chapter 2's shared ray-tracing core
(`RayTraceCoreTests.cpp`), Chapter 6's mesh-export geometry
(`EntityMeshTests.cpp`), and a genuine CPU-vs-GPU numerical comparison
(`DeterministicParityTests.cpp`) — and each layer is tested for a different
reason. The centerpiece of this chapter is the last of those: a tolerance
that was set, measured against, found wrong, and revised, which is as close
as this codebase gets to a recorded scientific method. Eighteen tests exist
across the three files; all eighteen currently pass.

Files covered: `RayTracerBenchTests/TestFramework.hpp`,
`RayTracerBenchTests/main.cpp`, `RayTracerBenchTests/RayTraceCoreTests.cpp`,
`RayTracerBenchTests/EntityMeshTests.cpp`,
`RayTracerBenchTests/DeterministicParityTests.cpp`.

---

## §1. A test framework small enough to read in one sitting

Every other part of this project's UI layer works around gaps in vendored
Apple frameworks by writing project-local shims (Chapters 8–10). The test
layer takes the same posture toward *testing* frameworks: rather than vendor
doctest or Catch2, or reach for Xcode's XCTest (which would require
Objective-C, the one dependency this whole codebase is built to avoid — see
`CLAUDE.md`'s "What this project is"), it writes the ~50 lines of
self-registration and assertion machinery it actually needs, once, in
`TestFramework.hpp`. The file's own comment states the reasoning plainly:

```cpp
// A tiny, dependency-free, self-registering test framework — RayTracerBenchTests is a plain
// command-line C++ tool (no XCTest, no Objective-C), and this project already avoids external
// dependencies beyond the vendored Metal bindings, so a single header beats vendoring doctest.
```
`RayTracerBenchTests/TestFramework.hpp:3-5`

The registration mechanism is the classic "static object with a
side-effecting constructor" trick, chosen specifically so that test files
never need a hand-maintained list of which tests exist. A `TestCase` is just
a name and a `std::function<void()>`, stored in a `std::vector` behind a
Meyer's-singleton accessor:

```cpp
// Meyer's-singleton accessor rather than a plain global, so registration order across
// translation units (each with its own static TestRegistrar instances) is well-defined.
inline std::vector<TestCase>& registry()
{
	static std::vector<TestCase> s_registry;
	return s_registry;
}

struct TestRegistrar
{
	// Constructing a static TestRegistrar (see the TEST_CASE macro) registers its test function
	// as a side effect, before main() runs.
	TestRegistrar( const char* name, std::function<void()> fn ) { registry().push_back( { name, std::move( fn ) } ); }
};
```
`RayTracerBenchTests/TestFramework.hpp:22-35`

`TEST_CASE(name)` is the macro that ties a test function's *definition* to
its *registration* in one place, so writing a new test is just writing a
function body — nothing else has to be touched:

```cpp
#define TEST_CASE( name )                                                                        \
	static void          name();                                                                  \
	static ::TestFramework::TestRegistrar registrar_##name( #name, name );                        \
	static void          name()
```
`RayTracerBenchTests/TestFramework.hpp:74-77`

This expands `TEST_CASE( hitSphere_hitsDeadOn ) { ... }` into a forward
declaration, a file-scope `static TestRegistrar` whose constructor runs
before `main()` and pushes `{"hitSphere_hitsDeadOn", hitSphere_hitsDeadOn}`
into the registry, and then the function definition itself. Every
`TEST_CASE` in every `.cpp` file linked into the binary adds itself to the
same global list purely by existing — there is no `AllTests.cpp` enumerating
them.

`main.cpp` is consequently almost content-free — the entire executable's
entry point is "run whatever got registered":

```cpp
#include "TestFramework.hpp"

// Entry point: runs every TEST_CASE registered across this executable's translation units.
int main()
{
	return TestFramework::runAll();
}
```
`RayTracerBenchTests/main.cpp:1-7`

`runAll()` iterates the registry, calls each test function inside a
`try`/`catch`, and prints one `[PASS]`/`[FAIL]` line per test plus a summary:

```cpp
inline int runAll()
{
	int passed = 0;
	int failed = 0;
	for ( const TestCase& test : registry() )
	{
		try
		{
			test.fn();
			std::printf( "[PASS] %s\n", test.name.c_str() );
			++passed;
		}
		catch ( const CheckFailure& failure )
		{
			std::printf( "[FAIL] %s: %s\n", test.name.c_str(), failure.message.c_str() );
			++failed;
		}
		catch ( const std::exception& ex )
		{
			std::printf( "[FAIL] %s: unexpected exception: %s\n", test.name.c_str(), ex.what() );
			++failed;
		}
	}
	std::printf( "%d passed, %d failed, %d total\n", passed, failed, passed + failed );
	return failed == 0 ? 0 : 1;
}
```
`RayTracerBenchTests/TestFramework.hpp:44-69`

A failed test doesn't abort the run — it throws a `CheckFailure`, which
`runAll()` catches and logs as a `[FAIL]` line before moving on to the next
registered test, so one broken assertion never hides the results of the
tests after it. The process's exit code (0 or 1) is what a CI system or a
human running the binary directly checks; the printed `[PASS]`/`[FAIL]`
lines are for a human reading the output.

The two assertion macros both build on the same "throw a `CheckFailure`
carrying a `file:line`-prefixed message" primitive:

```cpp
#define RT_CHECK_MESSAGE( cond, extra )                                                           \
	do                                                                                              \
	{                                                                                               \
		if ( !( cond ) )                                                                             \
		{                                                                                             \
			std::ostringstream oss;                                                                    \
			oss << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << " " << extra;             \
			throw ::TestFramework::CheckFailure{ oss.str() };                                           \
		}                                                                                             \
	} while ( false )

#define CHECK( cond ) RT_CHECK_MESSAGE( cond, "" )

#define CHECK_NEAR( a, b, eps )                                                                   \
	do                                                                                              \
	{                                                                                               \
		double rt_a = ( a );                                                                         \
		double rt_b = ( b );                                                                         \
		if ( std::fabs( rt_a - rt_b ) > ( eps ) )                                                    \
		{                                                                                             \
			std::ostringstream oss;                                                                    \
			oss << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR failed: " #a " (" << rt_a << ") vs "    \
				<< #b " (" << rt_b << "), eps=" << ( eps );                                              \
			throw ::TestFramework::CheckFailure{ oss.str() };                                           \
		}                                                                                             \
	} while ( false )
```
`RayTracerBenchTests/TestFramework.hpp:81-109`

`CHECK` is for exact boolean conditions (`result.hit`, `!result.hit`,
`a1 == a2`); `CHECK_NEAR` exists because almost every geometric assertion in
this suite is a floating-point comparison, where exact equality is the wrong
question to ask.

### Why `xcodebuild test -scheme RayTracerBenchTests` does not work

Because `RayTracerBenchTests` is a plain `add_executable()` target with a
`main()` that calls `runAll()` directly — not an XCTest bundle — the Xcode
scheme that CMake's Xcode generator produces for it has no Test action
configured at all. There is nothing in the scheme for `xcodebuild test` to
invoke; the target has a Run action (it's an ordinary executable) but no
XCTest target to attach a Test action to. `CLAUDE.md`'s build-and-test
section documents this precisely, because it's exactly the kind of thing
that looks like it should work and silently doesn't — worth quoting exactly
rather than paraphrased, since it's operational instruction, not narrative:

> - Build: Xcode GUI (`Product > Run`), or `xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBench -configuration Debug build`
> - Test: `xcodebuild -project RayTracerBench.xcodeproj -scheme RayTracerBenchTests -configuration Debug build`, then run the built binary directly (find it via `xcodebuild -showBuildSettings -scheme RayTracerBenchTests | grep TARGET_BUILD_DIR`, or just run it from Xcode's GUI). **`xcodebuild test -scheme RayTracerBenchTests` does not work** — confirmed by trying it, not assumed — since `RayTracerBenchTests` is a plain command-line C++ tool (no XCTest/ObjC), so its CMake-generated scheme has no Test action configured; `xcodebuild test` fails with "Scheme ... is not currently configured for the test action."

(`CLAUDE.md`, "Build and test commands")

The note that this was "confirmed by trying it, not assumed" matters as much
as the fact itself: it's the same discipline this whole project applies
everywhere else (the AppKit/metal-cpp gaps in Chapter 10, the
`RayTraceCore.h`-cannot-be-compiled-from-a-string finding in Chapter 3) —
don't write down a claim about tooling behavior without having actually
observed the failure message. The correct sequence is therefore: build the
`RayTracerBenchTests` target with a plain `xcodebuild ... build` (not
`test`), locate the resulting binary's `TARGET_BUILD_DIR`, and execute it
like any other command-line tool. Its `main()` (§1 above) does the rest —
`runAll()`'s process exit code is what a script or CI step should check.

---

## §2. `RayTraceCoreTests.cpp`: exercising Chapter 2 directly

`RayTraceCore.h` is dual-compiled — the exact same header is `#include`d
verbatim by `Shaders/Raytracer.metal` (MSL) and `CPU/CPURenderer.cpp` (plain
C++), which is the entire reason the CPU/GPU comparison in this app is
apples-to-apples (Chapter 2). `RayTraceCoreTests.cpp` links against the
*plain C++ compile* of that header — it never touches Metal — so it's really
testing the one shared source file that both renderers execute, from the
host side where it's cheap and fast to test.

The file opens with small helpers that build a `TransformGPU`/`ShapeGPU`
pair for a sphere or an axis-aligned pyramid, since almost every test needs
one:

```cpp
// An identity-orientation Transform at `center` — sufficient for a sphere, whose Transform
// component never uses right/up/forward.
TransformGPU sphereTransform( simd_float3 center )
{
	TransformGPU t;
	t.position = center;
	t.right = makeFloat3( 1.0f, 0.0f, 0.0f );
	t.up = makeFloat3( 0.0f, 1.0f, 0.0f );
	t.forward = makeFloat3( 0.0f, 0.0f, 1.0f );
	return t;
}
```
`RayTracerBenchTests/RayTraceCoreTests.cpp:6-16`

Thirteen `TEST_CASE`s follow, walking outward from `hitSphere()` through
`hitPyramid()` to `hitEntity()`'s dispatch, then a couple of small pure-math
utilities. Two representative cases:

`hitSphere_originInsideSphere_reportsBackFace` checks a specific correctness
property of `RayTraceCore.h`'s `hitSphere()` — that the `frontFace` flag
flips correctly when the ray origin is *inside* the sphere, since this is
exactly the situation `scatter()`'s dielectric branch depends on to decide
whether a ray is entering or exiting glass:

```cpp
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
```
`RayTracerBenchTests/RayTraceCoreTests.cpp:92-106`

`hitPyramid_hitsSideFaceWhenOffApexAxis` is a good example of the suite
testing not just "does it hit" but "does it hit at the geometrically
predicted `t` and `normal`" — here by working out, in the comment, exactly
what the +X face's plane equation from `RayTraceCore.h` predicts for a ray
fired straight down at `x = 0.9`, and then asserting the intersection lands
precisely there:

```cpp
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
```
`RayTracerBenchTests/RayTraceCoreTests.cpp:139-155`

`hitEntity_dispatchesByShapeTag` (`RayTracerBenchTests/RayTraceCoreTests.cpp:200-217`)
is the test that specifically targets `hitEntity()` itself rather than
either primitive's intersection routine — its comment states the point
directly: "confirm it reaches `hitSphere()` and `hitPyramid()` for the
matching tags rather than, say, always taking the sphere path," which is
exactly the failure mode a tagged switch (as opposed to a virtual call) can
silently fall into if a `case` is missing or mis-ordered.

The remaining two tests step outside intersection geometry entirely to
cover small, pure utility functions the whole core depends on:
`reflect3_mirrorsAboutNormal` (`RayTracerBenchTests/RayTraceCoreTests.cpp:220-230`)
checks that reflecting a 45° incoming vector off a flat normal produces the
mirrored vector `scatter()`'s metal branch needs, and
`pcgHash_isDeterministicAndVaries` (`RayTracerBenchTests/RayTraceCoreTests.cpp:234-242`)
checks the one property the per-thread RNG absolutely must have — same
input, same output, every time — since that determinism is the entire
premise §4's parity test relies on.

---

## §3. `EntityMeshTests.cpp`: catching a winding-order bug that actually happened

Chapter 6's `buildEntityMesh()` is a third ECS "system" over the same
Transform/Shape components `hitEntity()` dispatches on, but instead of
testing a ray it emits a world-space triangle mesh for glTF/OBJ export. A
triangle mesh has a property that a ray-intersection routine doesn't need to
worry about explicitly: *winding order* — the order a triangle's three
vertices are listed in determines, via the right-hand rule on the
cross-product of its edges, which way its face normal points. Get the
winding backwards and the geometry is still the right *shape*, but every
face normal points into the solid instead of out of it — a bug that is very
easy to miss by inspection (assimp/trimesh will happily load the mesh, and
it silhouette-renders identically from most raytracers) and would only show
up as inverted lighting in some downstream renderer.

`EntityMeshTests.cpp`'s central helper, `allTrianglesFaceAwayFrom()`, is
exactly this project's answer to that risk: for every triangle in a
`MeshData`, take its edge cross product as the face normal, and check that
the vector from a known interior point to the triangle's centroid points in
the *same* half-space as that normal (a positive dot product):

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

The comment is explicit that this isn't a check added defensively after the
fact for good hygiene — it's "the same check used to hand-derive
`EntityMesh.cpp`'s winding orders." `CLAUDE.md`'s project-status notes name
the actual outcome of running it: "an initial sphere-winding attempt was in
fact backwards and caught this way" — meaning the very first version of the
sphere tessellation in `buildEntityMesh()` had its indices listed in the
wrong order, and this test (or the standalone harness it was promoted from)
is what surfaced it, rather than a visual inspection in a 3D viewer.

`buildEntityMesh_sphere_hasNoDegenerateTriangles_andFacesOutward` applies
that check to a tessellated sphere, and additionally confirms every emitted
vertex actually lies on the sphere's surface (a check that would catch a
tessellation-math bug independent of winding):

```cpp
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
```
`RayTracerBenchTests/EntityMeshTests.cpp:45-70`

`buildEntityMesh_pyramid_hasExactlySixFacetedTriangles_andFacesOutward`
(`RayTracerBenchTests/EntityMeshTests.cpp:74-93`) does the same winding
check for the pyramid's exact 5-plane geometry, plus a structural assertion
specific to how Chapter 6 builds pyramid meshes — 6 triangles × 3 unshared
vertices each (`mesh.indices.size() == 18`, `mesh.positions.size() == 18`),
confirming the faceted, flat-normal construction (as opposed to a
shared-vertex, smooth-normal one, which is what the sphere tessellation
uses instead) actually emits unique vertices per triangle rather than
accidentally reusing indices.

The third test,
`buildEntityMesh_pyramid_respectsOrientationBasis`
(`RayTracerBenchTests/EntityMeshTests.cpp:95-122`), checks a different axis
of correctness entirely: that a pyramid's `TransformGPU` orientation basis
(the same `right`/`up`/`forward` frame `hitPyramid()` uses to transform rays
into local space) is honored by the mesh builder too, by rotating a pyramid
90° so its local "up" points along world +X and confirming the apex vertex
lands at world `(1, 0, 0)` rather than the untransformed `(0, 1, 0)`.

---

## §4. `DeterministicParityTests.cpp`: the revised-tolerance story

This is the one test file in the suite that actually runs both renderers
and is only meaningful *because* `RayTraceCore.h` is shared verbatim between
them (Chapter 2) — a CPU/GPU pixel diff on two independently-written ray
tracers would tell you nothing except that two different programs produce
different output. Here, a mismatch is potentially informative precisely
because both sides execute the same intersection/scatter/`rayColor()` logic,
so any divergence has to come from somewhere identifiable: RNG *stream*
differences (both sides use the same `pcgHash`/`randomFloat` function, but
seeded per-thread differently — see Chapter 2 and 4/5), or sub-ULP
floating-point differences between CPU and GPU transcendental functions
(`sqrt`, `pow`), and nothing else.

`measureParity()` builds one fixed-seed `SceneDescription` via
`buildDefaultScene()`, renders it with `renderCPU()`, renders the identical
scene with a real `GPURenderer` (headless — `MTL::CreateSystemDefaultDevice()`
with no window, matching the "plan doc's `DeterministicParityTests` intent"
noted in its own comment), reads the GPU texture back with `getBytes()`, and
diffs every RGB channel (skipping alpha) between the two:

```cpp
ParityStats measureParity( uint32_t width, uint32_t spp, uint32_t maxDepth, unsigned seed, int tolerance )
{
	const float      aspectRatio = 16.0f / 9.0f;
	SceneDescription scene = buildDefaultScene( seed, width, aspectRatio, spp, maxDepth );

	CPURenderResult cpuResult = renderCPU( scene, CPUThreading::MultiThreaded );

	MTL::Device* pDevice = MTL::CreateSystemDefaultDevice();
	GPURenderer  gpuRenderer( pDevice );
	GPURenderResult gpuResult = gpuRenderer.render( scene );

	const uint32_t       w = scene.params.width;
	const uint32_t       h = scene.params.height;
	std::vector<uint8_t> gpuPixels( (size_t)w * h * 4 );
	gpuResult.pTexture->getBytes( gpuPixels.data(), (size_t)w * 4, MTL::Region( 0, 0, 0, w, h, 1 ), 0 );

	size_t mismatches = 0;
	int    maxDiff = 0;
	const size_t totalChannels = (size_t)w * h * 3; // RGB only, skip alpha

	for ( uint32_t y = 0; y < h; ++y )
	{
		for ( uint32_t x = 0; x < w; ++x )
		{
			size_t idx = ( (size_t)y * w + x ) * 4;
			for ( int c = 0; c < 3; ++c )
			{
				int diff = std::abs( (int)cpuResult.pixels[ idx + c ] - (int)gpuPixels[ idx + c ] );
				maxDiff = std::max( maxDiff, diff );
				if ( diff > tolerance )
					++mismatches;
			}
		}
	}

	pDevice->release();

	return ParityStats{ (double)mismatches / (double)totalChannels, maxDiff };
}
```
`RayTracerBenchTests/DeterministicParityTests.cpp:28-66`

### What was assumed, what was measured, and what changed

`CLAUDE.md`'s verification-approach notes record the actual history behind
this function's two parameters — `tolerance` and the spp at which the
resulting assertion is set — and it is worth reconstructing precisely,
because the file's current test only encodes the *end* of the story, not
the middle.

The original plan called for a strict per-pixel bound: every corresponding
CPU/GPU pixel should agree to within roughly 2/255. That was the "originally
planned" tolerance CLAUDE.md refers to, and it was optimistic. Measuring it
for real — at a low sample count, spp=16, on the 64×36 preview size this
suite still uses today — found that per-channel mismatches over that
tolerance-of-2 affected **~8.8% of all channels**, with a **max diff of
61/255**: a large, occasional disagreement, not universal noise. Critically,
this was *not* treated as evidence of a bug, because of where the
mismatches did and didn't occur: diffs were exactly zero on every pure-sky
pixel — the case with no scatter/branching at all, where CPU and GPU
execute an identical, branch-free code path — and non-zero only where rays
actually hit geometry and had to make a scatter or reflectance decision.
Raising spp from 16 to 200 then *cut* both figures — mismatch rate 8.8% →
4.9%, max diff 61 → 13 — which is the signature of a chaotic Monte Carlo
system, not a systematic bug: a real bug (a sign error, a wrong constant, a
mixed-up axis) produces an error that doesn't shrink as more samples get
averaged together, whereas a sub-ULP floating-point difference between
CPU's and GPU's `sqrt`/`pow` implementations flipping an occasional
scatter/reflectance branch produces exactly what was observed — two ray
paths that fully diverge from the flip point onward (hence the large max
diff on the pixels that are affected at all), diluted more and more as spp
rises and each pixel averages over more independent samples.

The engineering rule this produced, stated explicitly in CLAUDE.md, is:
**assert on mismatch rate at high spp, not a strict per-pixel bound at low
spp.** The 8.8%-at-spp=16 measurement itself is *not* what's encoded in the
committed test — it lives only in CLAUDE.md's record of the manual
investigation that established the rule. What the committed test actually
asserts is the high-spp side of that same investigation:

```cpp
// See CLAUDE.md's verification notes: a strict ~2/255 per-pixel bound does NOT hold at low sample
// counts (chaotic Monte Carlo branch-divergence from sub-ULP CPU/GPU float differences flips
// occasional scatter/reflectance decisions, causing large but *isolated* per-pixel differences —
// not a bug, confirmed by mismatches being exactly zero on every pure-sky pixel and shrinking as
// spp rises). So this asserts on mismatch RATE at a reasonably high spp, where that chaotic
// divergence has mostly averaged out, rather than a strict per-pixel bound at low spp.
TEST_CASE( cpuGpuParity_fixedSeed_highSampleCount )
{
	ParityStats stats = measureParity( /*width=*/64, /*spp=*/200, /*maxDepth=*/16, /*seed=*/777u, /*tolerance=*/2 );

	std::printf( "  parity: %.3f%% channels mismatched (>2/255), max diff %d\n", stats.mismatchFraction * 100.0, stats.maxDiff );

	CHECK( stats.mismatchFraction < 0.06 ); // observed ~4.9% at spp=200 in prior manual runs
}
```
`RayTracerBenchTests/DeterministicParityTests.cpp:69-82`

Two things about this assertion are worth noticing precisely, since they're
the load-bearing detail of the whole chapter. First, the per-channel
tolerance itself is *still* 2/255 — that part of the original plan wasn't
wrong, it was just never going to be satisfied on 100% of channels, so what
changed is not the per-pixel threshold but the *statistic asserted over it*
(a rate, not a universal bound) and the *spp at which that statistic is
checked* (200, not 16). Second, the assertion's own bound, `< 0.06`, is
deliberately looser than the ~4.9% actually observed at spp=200 — the
comment names the observed figure directly as the basis for the threshold,
leaving headroom above it rather than pinning the assertion to the exact
measured value, which would make the test brittle to the ordinary run-to-run
variance of a Monte Carlo renderer.

The second test in this file is a narrower, cheaper sanity check that the
parity test above implicitly depends on: that `buildDefaultScene()` plus
`renderCPU()` are actually deterministic for a fixed seed, so that any
CPU/GPU divergence measured above can be attributed to CPU-vs-GPU execution
rather than to scene-construction or RNG non-determinism on the CPU side
alone.

```cpp
TEST_CASE( cpuGpuParity_isDeterministic_sameSeedSameResult )
{
	// Rendering the same seed twice (CPU side only, cheap) must produce byte-identical output —
	// this is a sanity check on the scene/RNG determinism the parity test above depends on, not a
	// CPU/GPU comparison.
	const float      aspectRatio = 16.0f / 9.0f;
	SceneDescription sceneA = buildDefaultScene( 42u, 48, aspectRatio, 8, 8 );
	SceneDescription sceneB = buildDefaultScene( 42u, 48, aspectRatio, 8, 8 );

	CPURenderResult resultA = renderCPU( sceneA, CPUThreading::SingleThreaded );
	CPURenderResult resultB = renderCPU( sceneB, CPUThreading::SingleThreaded );

	CHECK( resultA.pixels == resultB.pixels );
}
```
`RayTracerBenchTests/DeterministicParityTests.cpp:84-97`

It deliberately renders `SingleThreaded` — with `CPUThreading::MultiThreaded`
(the CPU renderer's default; see Chapter 4), row-partitioned worker threads
could in principle finish and touch shared state in a different order
between two runs, which would make this test about thread-scheduling
noise rather than about the RNG/scene determinism it actually intends to
check.

---

## §5. Eighteen tests, all passing

Counting `TEST_CASE` occurrences across the three test files: thirteen in
`RayTraceCoreTests.cpp` (`hitSphere_hitsDeadOn`, `hitSphere_missesEntirely`,
`hitSphere_respectsTMaxRange`, `hitSphere_originInsideSphere_reportsBackFace`,
`hitSphere_picksCloserOfTwoRoots`, `hitPyramid_hitsApexFromAbove`,
`hitPyramid_hitsSideFaceWhenOffApexAxis`, `hitPyramid_hitsBaseFromBelow`,
`hitPyramid_missesEntirely`, `hitPyramid_sideNormalPointsOutwardAndUpward`,
`hitEntity_dispatchesByShapeTag`, `reflect3_mirrorsAboutNormal`,
`pcgHash_isDeterministicAndVaries`), three in `EntityMeshTests.cpp`, and two
in `DeterministicParityTests.cpp` — 13 + 3 + 2 = **18**, matching the count
`CLAUDE.md`'s project-status notes give ("18 tests, all passing"), so the
number hasn't drifted since that note was written. `CMakeLists.txt` links
exactly these three test `.cpp`s plus `main.cpp` into the
`RayTracerBenchTests` executable target (`CMakeLists.txt:113-117`), so this
count is also the complete registry `runAll()` (§1) will walk at runtime —
nothing else contributes a `TEST_CASE` to this binary.

---

## Where this connects

- **Chapter 2** (`Core/RayTraceCore.h`) is the subject of nearly all of
  §2's tests directly, and is the entire reason §4's CPU/GPU diff can be
  interpreted at all — without the dual-compiled shared core, a pixel
  mismatch would be uninformative noise between two unrelated programs
  rather than a signal about RNG streams and floating-point precision.
- **Chapter 6** (`Export/EntityMesh.hpp/.cpp`) is what §3 tests; the
  winding-order check there is a direct, reusable form of the same
  hand-verification technique used to derive `buildEntityMesh()`'s geometry
  in the first place, and is the reason a genuine sphere-winding bug was
  caught before shipping rather than after.
- **Chapters 4 and 5** (`CPU/CPURenderer.hpp/.cpp`, `GPU/GPURenderer.hpp/.cpp`)
  are the two renderers §4's `measureParity()` actually invokes and diffs —
  this chapter's parity story is really about the divergence between those
  two chapters' execution of the one shared core from Chapter 2.
- **Chapter 12** (`CMakeLists.txt`) is the build system that compiles this
  test binary and is the source of the "no Test action configured" scheme
  limitation discussed in §1 — and its own text should be read alongside
  the exact `xcodebuild ... build` + run-the-binary-directly sequence
  quoted from `CLAUDE.md` above, since that sequence is the only way this
  chapter's tests actually get executed.
