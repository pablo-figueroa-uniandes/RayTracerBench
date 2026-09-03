# Chapter 7: Scene Export and Import — glTF, OBJ, Preview Images, and Loading a Scene Back

**Abstract.** The renderers turn a `SceneDescription` into pixels; most of this chapter covers the
code that instead turns it into files a 3D modeling tool can open. `Export/SceneExporter.hpp/.cpp`
walks the same entity arrays Chapter 1 describes, calls Chapter 6's `buildEntityMesh()` per
entity, and serializes the result as either a single self-contained `.gltf` file or an
`.obj`+`.mtl` pair, with a from-scratch material mapping into each format's native shading model.
Alongside the 3D file, `Export/ImageWriter.hpp/.cpp` writes a same-named PNG — a real render at the
current settings, not a placeholder — which is why `AppDelegate::saveScene()` (Chapter 8) had to
become a background-thread operation instead of the fast, synchronous, main-thread call it started
out as. §10 covers the verification story CLAUDE.md records for the export side: an external-tool
check of a real ~490-entity export, and a temporary env-var-gated hook run through the built app and
then reverted. §11 closes the chapter with the reverse direction, added later at user request:
`Export/SceneImporter.hpp/.cpp` reconstructs a scene's entities and materials from a `.gltf`/`.obj`
this app itself previously wrote — a round-trip reader, not a general-purpose importer — using a
small hand-rolled JSON parser and `Export/Base64.hpp/.cpp`'s decode half (its encode half already
existed for §4's own embedded buffer).

Files covered: `Export/SceneExporter.hpp`, `Export/SceneExporter.cpp`, `Export/ImageWriter.hpp`,
`Export/ImageWriter.cpp`, `Export/SceneImporter.hpp`, `Export/SceneImporter.cpp`,
`Export/Base64.hpp`, `Export/Base64.cpp`.

---

## §1. Why this module exists, and where it sits

Everything up through Chapter 6 is in service of turning a `SceneDescription` into pixels. The
`ControlsPanel` also exposes two buttons — "Save glTF" and "Save OBJ" — that instead turn the
*same* scene into a standard 3D-interchange file, so it can be opened in Blender, Preview, or any
glTF/OBJ-aware tool outside this app entirely. `SceneExporter.hpp` states the intent plainly:

```cpp
#pragma once

#include "../Core/Scene.hpp"

#include <string>
#include <vector>

// Result of an export attempt: `ok` plus a human-readable summary (a success message naming the
// written path(s), or an error) and the absolute path(s) actually written.
struct SceneExportResult
{
	bool                     ok;
	std::string              message;
	std::vector<std::string> writtenFilePaths;
};
```
`Export/SceneExporter.hpp:1-15`

`Export/`, like `Core/`, is framework-free C++17 and is compiled into the same `RayTracerCore`
static library — it has no dependency on Metal or AppKit and needs neither to run. That placement
matters for `ImageWriter`, covered in §7 below, which *does* need two Apple frameworks and is
consequently built differently.

The module has four public entry points, each covered in its own section: `executableDirectory()`
(§2), `sceneFilenameStem()` (§3), `exportSceneAsGLTF()` (§4), and `exportSceneAsOBJ()` (§5), plus
`ensureSavedScenesDirectoryPath()` (§6), which exists purely for the preview-PNG feature to reuse.

## §2. `executableDirectory()`: resolving the real binary location

Every export lands in a `SavedScenes/` directory next to the running executable. Finding "next to
the running executable" correctly is less trivial than it sounds, and the header comment is
explicit about the two wrong answers that were considered and rejected:

```cpp
// Absolute path to the directory containing the currently-running executable, resolved via
// _NSGetExecutablePath (not argv[0] or getcwd() — this app can be launched from any working
// directory, e.g. Xcode's "Product > Run" or a Finder double-click).
std::string executableDirectory();
```
`Export/SceneExporter.hpp:17-20`

`argv[0]` is unreliable because it's whatever the parent process chose to pass — it can be a bare
program name with no path component at all, depending on how the process was launched.
`getcwd()` is unreliable for the opposite reason: the current working directory reflects wherever
the *launcher* happened to be (Xcode's own build directory when run via "Product > Run", the
Finder's notion of "current directory" — effectively undefined — for a double-click), not where
the binary itself lives. Both would make `SavedScenes/` show up in an unpredictable place
depending on how the app happens to be started that particular time.

The fix is to ask the OS directly. `_NSGetExecutablePath` is a Darwin-specific `mach-o/dyld.h` API
with an unusual two-call contract: call it once with a null buffer to learn the required size,
then call it again with a buffer of that size to get the actual path.

```cpp
// Resolves _NSGetExecutablePath's two-call pattern (first call reports the required buffer size)
// and canonicalizes the result so a relative or symlinked launch path still resolves correctly.
std::string executableDirectory()
{
	uint32_t size = 0;
	_NSGetExecutablePath( nullptr, &size ); // always "fails" here; size is now the required length

	std::vector<char> buffer( size );
	_NSGetExecutablePath( buffer.data(), &size );

	std::error_code       ec;
	std::filesystem::path exePath = std::filesystem::canonical( std::filesystem::path( buffer.data() ), ec );
	if ( ec )
		return std::filesystem::path( buffer.data() ).parent_path().string();
	return exePath.parent_path().string();
}
```
`Export/SceneExporter.cpp:103-118`

The `std::filesystem::canonical` pass is a second layer of correctness on top of the syscall
itself: `_NSGetExecutablePath` is documented to potentially return a path containing symlinks or
`..` components rather than a fully resolved absolute path, so `canonical()` resolves those before
`parent_path()` is taken. If canonicalization fails for some reason (`ec` set), the code falls back
to `parent_path()` on the raw, uncanonicalized path rather than propagating an error — a directory
that's merely un-canonicalized is still usable, so the fallback degrades gracefully instead of
failing the whole export over a cosmetic issue.

## §3. `sceneFilenameStem()`: what varies the file, and what doesn't

Every export — `.gltf`, `.obj`/`.mtl`, and the companion `.png` from §8 — shares one filename stem,
computed here:

```cpp
// A filename stem encoding the scene-identifying parameters — seed, image width (which feeds the
// camera's aspect ratio), and the Floating? state — plus a timestamp, so re-exporting identical
// settings doesn't silently overwrite a prior export. samplesPerPixel/maxDepth are deliberately
// excluded: they affect only rendering, not the exported geometry itself.
std::string sceneFilenameStem( unsigned seed, uint32_t width, bool floating );
```
`Export/SceneExporter.hpp:22-26`

```cpp
// Encodes the scene-identifying parameters plus a timestamp — see the header comment for why
// samplesPerPixel/maxDepth are excluded.
std::string sceneFilenameStem( unsigned seed, uint32_t width, bool floating )
{
	std::time_t t = std::chrono::system_clock::to_time_t( std::chrono::system_clock::now() );
	std::tm     localTime{};
	localtime_r( &t, &localTime );

	char buf[ 160 ];
	std::snprintf( buf, sizeof( buf ), "scene_seed%u_w%u_%s_%04d%02d%02d-%02d%02d%02d",
		seed, width, floating ? "floating" : "grounded",
		localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
		localTime.tm_hour, localTime.tm_min, localTime.tm_sec );
	return std::string( buf );
}
```
`Export/SceneExporter.cpp:120-134`

The subtle point worth dwelling on is *which* settings make it into the stem and which don't.
`RenderSettings` (surfaced by `ControlsPanel::currentSettings()`, Chapter 9) bundles six values:
image width, samples per pixel, max depth, CPU-threading mode, scene seed, and the Floating?
checkbox. Only three of those six — seed, width, and floating — actually change what geometry
`buildDefaultScene()` produces. The seed drives the `std::mt19937` that places the randomized-field
spheres (Chapter 1); width feeds the camera's aspect ratio, which changes the camera's frustum and
therefore which of the *fixed* scene entities are laid out identically but which region of them a
render would frame (Chapter 1's `buildDefaultScene()` also takes width for this reason); and
floating decides whether those spheres sit on the ground or at a random height per Chapter 1's
`kMaxFloatHeight`. Samples-per-pixel and max-depth, by contrast, are pure rendering-quality knobs:
they change how many rays are traced and how deep they bounce, which affects the *pixel* output of
a render but never the *geometry* a 3D export walks. Baking them into the filename would imply
that two exports with different spp values contain different scenes, when in fact — for a fixed
seed/width/floating triple — they'd be byte-for-byte the same `.gltf`/`.obj`. Excluding them keeps
the filename an honest description of what varies in the exported file.

The trailing timestamp exists for an orthogonal reason: re-exporting the *same* seed/width/floating
combination twice (e.g. after tweaking spp and re-saving) would otherwise silently overwrite the
earlier file, since seed/width/floating alone can repeat. The timestamp guarantees each export
click produces a distinct file regardless.

## §4. `exportSceneAsGLTF()`: one self-contained file

glTF is written as a single `.gltf` file with the mesh's binary vertex/index data embedded directly
in the JSON as a base64 `data:` URI, rather than the more common two-file layout of a `.gltf` plus
a companion `.bin`. The encoder that makes this possible now lives in its own small shared module:

```cpp
// RFC 4648 standard base64 — used by the glTF exporter/importer to embed/recover the geometry
// buffer as a self-contained data: URI (no companion .bin file). Shared between SceneExporter.cpp
// and SceneImporter.cpp rather than duplicated, since encode/decode need to agree byte-for-byte.
std::string base64Encode( const uint8_t* data, size_t len );
```
`Export/Base64.hpp:7-10`

`base64Encode()` itself is a plain three-bytes-in/four-characters-out RFC 4648 encoder with the
usual `=`/`==` padding for a 1- or 2-byte remainder (`Export/Base64.cpp:17-51`) — nothing
glTF-specific, just infrastructure for what follows. It wasn't always its own file: `Base64.hpp/.cpp`
didn't exist until §11's `SceneImporter` needed a *decoder* for the same alphabet, at which point the
encoder (previously a private helper inside `SceneExporter.cpp`) was moved out to a shared home
rather than have the decode half duplicate the same 64-character alphabet table a second time where
it could silently drift out of sync.

`exportSceneAsGLTF()`'s job is to walk every entity, hand its `TransformGPU`/`ShapeGPU` pair to
Chapter 6's `buildEntityMesh()`, and fold the resulting `MeshData` into one shared binary buffer
plus the JSON structures (`bufferViews`, `accessors`, `meshes`, `nodes`) that describe how to slice
that buffer back into typed arrays. Two small lambdas do the buffer-appending, each returning the
byte offset the just-appended data starts at (which becomes each `bufferView`'s `byteOffset`):

```cpp
auto appendVec3s = [ &buffer ]( const std::vector<simd_float3>& v ) -> size_t {
	size_t offset = buffer.size();
	buffer.resize( offset + v.size() * sizeof( float ) * 3 );
	// simd_float3 is a SIMD-register type: sizeof/alignof is 16 (a padded 4th lane), not the
	// tightly-packed 12 bytes a plain float[3] would occupy — confirmed via sizeof/alignof and
	// a stride check on a real array, not assumed. A single bulk memcpy of v.size()*12 bytes
	// from v.data() would therefore read across the wrong element boundaries and scramble
	// every vertex past the first (caught by decoding a real exported .gltf's buffer and
	// diffing it against the source mesh data — components came out shuffled between
	// vertices). Copy each vertex's 3 floats individually instead, skipping the padding lane.
	for ( size_t i = 0; i < v.size(); ++i )
		std::memcpy( buffer.data() + offset + i * sizeof( float ) * 3, &v[ i ], sizeof( float ) * 3 );
	return offset;
};
auto appendIndices = [ &buffer ]( const std::vector<uint32_t>& v ) -> size_t {
	size_t offset = buffer.size();
	buffer.resize( offset + v.size() * sizeof( uint32_t ) );
	if ( !v.empty() )
		std::memcpy( buffer.data() + offset, v.data(), v.size() * sizeof( uint32_t ) );
	return offset;
};
```
`Export/SceneExporter.cpp:256-276`

This is not the module's original code, and the comment inside `appendVec3s` is the record of a real,
silent-corruption bug rather than a design note written in advance. The first version bulk-`memcpy`'d
`v.size() * 12` bytes straight out of a `std::vector<simd_float3>` — but `simd_float3` is a
SIMD-register type with `sizeof`/`alignof` 16, not 12, so consecutive elements sit 16 bytes apart in
memory, not 12; the bulk copy read across the wrong element boundaries and scrambled every vertex
past the first one in each mesh. `exportSceneAsOBJ()` (§5) was never affected, since it writes each
`float` field individually via `operator<<` rather than a bulk `memcpy` — which is exactly why the
two formats' bounding boxes started disagreeing and gave the bug away. It was caught not by
inspection but by decoding a real exported `.gltf`'s base64 buffer byte-for-byte and diffing it
against the source `MeshData`: the ground sphere's true bounds are `(-1000,-2000,-1000)`–`(1000,0,1000)`,
but the corrupted buffer decoded to `(-2000,-2000,-2000)`–`(1000,1000,924)` — components shuffled
between vertices, not merely off by rounding noise. The fix, shown above, copies each vertex's 3
floats individually, skipping the padding lane — and is now a permanent regression test rather than
just an ad hoc harness (`RayTracerBenchTests/SceneImportExportTests.cpp`, Chapter 11).

The per-entity loop calls `buildEntityMesh()` exactly once per entity (skipping any entity whose
mesh comes back empty — `buildEntityMesh()` returns an empty `MeshData` for a shape it doesn't
recognize, though in the current scene every entity is a sphere or a pyramid and always produces
geometry), computes the mesh's axis-aligned bounding box for glTF's required accessor
`min`/`max` fields, and appends three glTF `bufferView`/`accessor` pairs per mesh — one each for
positions, normals, and indices:

```cpp
MeshData mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
if ( mesh.positions.empty() )
	continue;

simd_float3 minV = mesh.positions[ 0 ];
simd_float3 maxV = mesh.positions[ 0 ];
for ( const simd_float3& p : mesh.positions )
{
	minV.x = std::min( minV.x, p.x );
	/* ... analogous min/max for y, z, and maxV — elided for length */
}

size_t posOffset = appendVec3s( mesh.positions );
int    posBufferView = bufferViewCount++;
bufferViewsJson << ( posBufferView ? "," : "" ) << "{\"buffer\":0,\"byteOffset\":" << posOffset
				<< ",\"byteLength\":" << ( mesh.positions.size() * sizeof( float ) * 3 ) << ",\"target\":34962}";
int posAccessor = accessorCount++;
accessorsJson << ( posAccessor ? "," : "" ) << "{\"bufferView\":" << posBufferView
			  << ",\"componentType\":5126,\"count\":" << mesh.positions.size() << ",\"type\":\"VEC3\",\"min\":[" << minV.x << ","
			  << minV.y << "," << minV.z << "],\"max\":[" << maxV.x << "," << maxV.y << "," << maxV.z << "]}";
```
`Export/SceneExporter.cpp:302-325`

(Normals and indices follow the identical pattern at `Export/SceneExporter.cpp:327-341`, using
`componentType` 5126 (`GL_FLOAT`) for normals and 5125 (`GL_UNSIGNED_INT`) for indices, and
`target` 34963 (`ELEMENT_ARRAY_BUFFER`) instead of 34962 (`ARRAY_BUFFER`) since indices are consumed
differently by a GPU than vertex attributes are — these are the standard glTF/OpenGL enum values,
not project-specific choices.) Each mesh then contributes one `mesh` object referencing its three
accessors and its material index, and one `node` object referencing that mesh
(`Export/SceneExporter.cpp:343-349`) — a flat one-node-per-entity scene graph, with no shared
transform hierarchy, since `buildEntityMesh()` already bakes each entity's `TransformGPU` into
world-space vertex positions (Chapter 6) rather than leaving it as a node-local transform for the
glTF consumer to apply.

Once every entity has been folded in, the accumulated `buffer` is base64-encoded once and embedded
as the sole entry in the top-level `"buffers"` array:

```cpp
std::string base64 = base64Encode( buffer.data(), buffer.size() );

std::ostringstream json;
json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"RayTracerBench SceneExporter\"},";
/* ... scene/nodes/meshes/materials/accessors/bufferViews assembled from the ostringstreams above ... */
json << "\"buffers\":[{\"byteLength\":" << buffer.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64 << "\"}]";
json << "}";
```
`Export/SceneExporter.cpp:362-376`

The JSON is built with hand-rolled `std::ostringstream` concatenation rather than a JSON library —
consistent with this project's preference (seen already in `Core/` and `CPU/`) for standing on
plain standard-library and system facilities rather than pulling in third-party dependencies for a
narrowly-scoped, fully-controlled output shape. The whole string is then written to a single
`.gltf` file with no companion `.bin` (`Export/SceneExporter.cpp:378-391`).

## §5. `exportSceneAsOBJ()`: a text format with a companion `.mtl`

OBJ, unlike glTF, has no notion of an embedded material or binary payload at all — Wavefront's
format is a plain-text vertex/face list that refers to materials by name via `usemtl`, with the
actual material definitions living in a separate `.mtl` file named by an `mtllib` directive. The
exporter follows that convention directly, writing the `.mtl` first:

```cpp
std::ofstream mtlFile( mtlPath );
/* ... */
mtlFile << "# RayTracerBench scene export - materials\n\n";
for ( size_t m = 0; m < scene.materials.size(); ++m )
{
	MTLFields f = mtlFieldsFor( scene.materials[ m ] );
	mtlFile << "newmtl Material_" << m << "\n";
	mtlFile << "Kd " << f.kd.x << " " << f.kd.y << " " << f.kd.z << "\n";
	mtlFile << "Ks " << f.ks.x << " " << f.ks.y << " " << f.ks.z << "\n";
	mtlFile << "Ns " << f.ns << "\n";
	mtlFile << "d " << f.d << "\n";
	mtlFile << "illum " << f.illum << "\n\n";
}
```
`Export/SceneExporter.cpp:166-187`

...then the `.obj` itself, which starts with the `mtllib` reference and, per entity, emits an `o`
(object) name, its vertex positions (`v`), its vertex normals (`vn`), a `usemtl` directive naming
that entity's material, and a triangle-list of `f` (face) lines:

```cpp
objFile << "# RayTracerBench scene export\n";
objFile << "mtllib " << stem << ".mtl\n\n";

const size_t entityCount = scene.transforms.size();
uint32_t     vertexOffset = 0; // OBJ vertex/normal indices are 1-based and file-global

for ( size_t e = 0; e < entityCount; ++e )
{
	MeshData mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
	if ( mesh.positions.empty() )
		continue;

	const char* shapeName = scene.shapes[ e ].type == SHAPE_SPHERE ? "Sphere" : "Pyramid";
	objFile << "o Entity_" << e << "_" << shapeName << "\n";

	for ( const simd_float3& p : mesh.positions )
		objFile << "v " << p.x << " " << p.y << " " << p.z << "\n";
	for ( const simd_float3& n : mesh.normals )
		objFile << "vn " << n.x << " " << n.y << " " << n.z << "\n";

	objFile << "usemtl Material_" << scene.shapes[ e ].materialIndex << "\n";
	for ( size_t i = 0; i + 2 < mesh.indices.size(); i += 3 )
	{
		uint32_t a = mesh.indices[ i ] + 1 + vertexOffset;
		uint32_t b = mesh.indices[ i + 1 ] + 1 + vertexOffset;
		uint32_t c = mesh.indices[ i + 2 ] + 1 + vertexOffset;
		objFile << "f " << a << "//" << a << " " << b << "//" << b << " " << c << "//" << c << "\n";
	}

	vertexOffset += (uint32_t)mesh.positions.size();
	objFile << "\n";
}
```
`Export/SceneExporter.cpp:196-227`

The `vertexOffset` bookkeeping is the one piece of OBJ-specific plumbing worth calling out: OBJ
vertex and normal indices are 1-based (hence the `+ 1`) *and* file-global, not per-object — every
`v`/`vn` line in the whole file shares one running index space, so each subsequent entity's face
indices have to be offset by the total vertex count of every entity written before it, not just
reset to zero per object the way `buildEntityMesh()`'s own per-entity `MeshData::indices` are. The
`a//a` face syntax (`vertex//normal`, no texture-coordinate slot) reflects that this exporter never
generates UVs — `MeshData` (Chapter 6) carries only positions, normals, and indices, no texcoords,
consistent with there being no texture-mapped materials anywhere in this scene.

## §6. Material mapping: the same dielectric approximation in both formats

Both exporters need to translate `Core/ShaderTypes.h`'s `MaterialGPU` — a tagged
`MAT_LAMBERTIAN`/`MAT_METAL`/`MAT_DIELECTRIC` struct, the same one `RayTraceCore.h`'s `scatter()`
switches on at render time (Chapter 2) — into each format's own, differently-shaped native material
model. glTF uses physically-based metallic-roughness (`baseColorFactor`, `metallicFactor`,
`roughnessFactor`, plus an alpha channel and `alphaMode`); OBJ/MTL uses the older Phong-style
`Kd`/`Ks`/`Ns`/`d`/`illum` fields (diffuse color, specular color, specular exponent, dissolve/opacity,
and an illumination-model index). Two small free functions do the mapping, one per format:

```cpp
// Maps a MaterialGPU onto glTF's PBR metallic-roughness parameters.
GLTFMaterialFields gltfMaterialFor( const MaterialGPU& mat )
{
	switch ( mat.type )
	{
		case MAT_LAMBERTIAN:
			return GLTFMaterialFields{ mat.albedo, 0.0f, 1.0f, 1.0f };
		case MAT_METAL:
			// fuzz's range here is [0, 0.5] (see Scene.cpp's fuzzDist) — doubled to fill
			// roughness's full [0,1] range.
			return GLTFMaterialFields{ mat.albedo, 1.0f, std::min( mat.fuzz * 2.0f, 1.0f ), 1.0f };
		case MAT_DIELECTRIC:
			// No KHR_materials_transmission here (deliberately, to keep this a plain,
			// universally-loadable core-spec glTF file) — approximated as a clear, glossy,
			// partly-transparent surface instead of true refraction.
			return GLTFMaterialFields{ simd_make_float3( 1.0f, 1.0f, 1.0f ), 0.0f, 0.05f, 0.35f };
	}
	return GLTFMaterialFields{ simd_make_float3( 1.0f, 1.0f, 1.0f ), 0.0f, 1.0f, 1.0f };
}
```
`Export/SceneExporter.cpp:51-69`

```cpp
// Maps a MaterialGPU onto Wavefront MTL's Kd/Ks/Ns/d/illum fields.
MTLFields mtlFieldsFor( const MaterialGPU& mat )
{
	switch ( mat.type )
	{
		case MAT_LAMBERTIAN:
			return MTLFields{ mat.albedo, simd_make_float3( 0.05f, 0.05f, 0.05f ), 8.0f, 1.0f, 2 };
		case MAT_METAL:
		{
			// Sharper specular highlight (higher Ns) for lower fuzz, mirroring fuzz's role as
			// a roughness knob in RayTraceCore.h's scatter().
			float roughness = std::min( mat.fuzz * 2.0f, 1.0f );
			return MTLFields{ mat.albedo * 0.15f, mat.albedo, 8.0f + ( 1.0f - roughness ) * 300.0f, 1.0f, 3 };
		}
		case MAT_DIELECTRIC:
			// illum 4: "Transparency: glass on, reflection: ray trace on" — the closest
			// standard MTL illumination model to a dielectric surface.
			return MTLFields{ simd_make_float3( 1.0f, 1.0f, 1.0f ), simd_make_float3( 1.0f, 1.0f, 1.0f ), 96.0f, 0.35f, 4 };
	}
	return MTLFields{ simd_make_float3( 1.0f, 1.0f, 1.0f ), simd_make_float3( 0.0f, 0.0f, 0.0f ), 8.0f, 1.0f, 2 };
}
```
`Export/SceneExporter.cpp:80-100`

For lambertian and metal, the two mappings are close analogues of each other: lambertian gets the
material's own `albedo` as its diffuse/base color with full roughness and no metallic contribution
in glTF (`metallicFactor` 0, `roughnessFactor` 1), and a small, fixed 5% specular tint in MTL; metal
gets `albedo` as base color with `metallicFactor` 1 in glTF, and `albedo` itself as the MTL
specular color with a dimmed 15% diffuse tint. Both formats derive their roughness-like knob the
same way — `fuzz` doubled to fill `[0,1]`, since `Scene.cpp`'s `fuzzDist` only ever produces values
in `[0, 0.5]` — and MTL additionally converts that into a specular exponent (`Ns`) that's *larger*
for *lower* fuzz, since a sharper (higher-exponent) Phong highlight is the classical-shading
equivalent of a smoother PBR surface.

`MAT_DIELECTRIC` is where the interesting decision sits, and it's made identically in both
functions: neither glTF's core specification nor OBJ/MTL has any real model of glass or light
refraction. glTF has an extension for it — `KHR_materials_transmission` — but the exporter
deliberately does not use it, so that the `.gltf` file stays plain core-spec glTF that any
glTF-capable viewer can load, rather than one that silently renders wrong (opaque, or without the
extension's intended look) in a viewer that doesn't implement that particular extension. Both
functions instead approximate a dielectric the same conceptual way: a clear (white) base color, a
low roughness/high specular exponent for a glossy look, and partial transparency — `alpha`/`d` of
`0.35` in both. This is stated candidly as an approximation, not a claim of physical equivalence:
the exported mesh will look like a glossy, translucent object in a generic 3D viewer, not like the
refracting glass sphere the ray tracer actually renders. Anyone comparing the exported `.gltf`/`.obj`
against the app's own raytraced preview PNG (§8) should expect the dielectric spheres in particular
to look different between the two — that's the nature of the approximation, not a bug in either
path.

## §7. `ensureSavedScenesDirectoryPath()`: a public seam for the preview PNG

`SceneExporter.cpp` already has a private `savedScenesDirectory()`/`ensureSavedScenesDirectory()`
pair (`Export/SceneExporter.cpp:20-34`) that both `exportSceneAsGLTF()` and `exportSceneAsOBJ()`
use internally to find and create `SavedScenes/`. When the preview-PNG feature (§8) was added,
`AppDelegate::saveScene()` needed to write a `.png` into that exact same directory, using the exact
same stem, right after the `.gltf`/`.obj` succeeds. Rather than have `AppDelegate.cpp` re-derive
`executableDirectory() / "SavedScenes"` and re-implement the create-if-missing logic itself — which
would risk the two call sites drifting apart if the directory name or resolution logic ever changed
— the same internal helper was exposed as one small public function:

```cpp
// The directory every export writes into — <executableDirectory()>/SavedScenes/ — created if it
// doesn't already exist yet. Exposed so callers can place a companion file (e.g. a preview image)
// alongside a 3D export in the exact same location, using the exact same stem. Returns an empty
// string if the directory couldn't be created.
std::string ensureSavedScenesDirectoryPath();
```
`Export/SceneExporter.hpp:28-32`

```cpp
// Creates (if needed) and returns the shared SavedScenes/ directory, so a caller writing a
// companion file (e.g. a preview image) alongside a 3D export can use the exact same location
// without duplicating the directory-resolution/creation logic above.
std::string ensureSavedScenesDirectoryPath()
{
	std::filesystem::path dir = savedScenesDirectory();
	std::string           error = ensureSavedScenesDirectory( dir );
	if ( !error.empty() )
		return "";
	return dir.string();
}
```
`Export/SceneExporter.cpp:139-146`

It's a small function, but it's a real API-design decision: `AppDelegate` (Chapter 8) calls this
one function to get a directory it can trust is already created, rather than needing to know
anything about `_NSGetExecutablePath` or the `"SavedScenes"` literal itself. The empty-string
sentinel on failure lets the caller treat "couldn't create the directory" and "successfully
resolved it" as a simple truthy/falsy check without a separate error-code out-parameter.

## §8. `ImageWriter`: `writePNG()` via CoreGraphics/ImageIO

The preview image is written by a small, separate module rather than folded into
`SceneExporter.cpp` itself, and the header explains both what it does and, implicitly, why it's
kept apart:

```cpp
// Writes an RGBA8, row-major, row-0-is-top pixel buffer (the same layout CPURenderer.hpp
// produces) to `path` as a PNG file, via CoreGraphics/ImageIO — real system C APIs, so this needs
// no third-party image library and no Objective-C. Returns false (rather than aborting) on
// failure, since a failed preview image shouldn't prevent the 3D export it accompanies from being
// reported as successful.
bool writePNG( const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height );
```
`Export/ImageWriter.hpp:6-11`

The implementation is a direct, unglamorous use of CoreGraphics to build a `CGImageRef` around the
caller's pixel buffer, and ImageIO to encode and write it:

```cpp
bool writePNG( const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height )
{
	CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider = CGDataProviderCreateWithData( nullptr, rgba, (size_t)width * height * 4, nullptr );

	CGImageRef image = CGImageCreate( width, height, 8, 32, (size_t)width * 4, colorSpace,
		kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, provider, nullptr, false, kCGRenderingIntentDefault );

	CGDataProviderRelease( provider );
	CGColorSpaceRelease( colorSpace );

	if ( !image )
		return false;

	CFURLRef url = CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false );
	CGImageDestinationRef destination = CGImageDestinationCreateWithURL( url, CFSTR( "public.png" ), 1, nullptr );
	CFRelease( url );

	if ( !destination )
	{
		CGImageRelease( image );
		return false;
	}

	CGImageDestinationAddImage( destination, image, nullptr );
	bool ok = CGImageDestinationFinalize( destination );

	CFRelease( destination );
	CGImageRelease( image );

	return ok;
}
```
`Export/ImageWriter.cpp:11-42`

`CGDataProviderCreateWithData` is given `rgba` directly with a null deallocator callback — the
comment above it in the source notes this "borrows the pointer for the duration of this call"
rather than copying it, so the caller's buffer must outlive the `CGImageCreate`/finalize sequence,
which it does here since it's all synchronous. `"public.png"` is passed as a raw UTI string literal
rather than linking the CoreServices/UniformTypeIdentifiers framework just to get the
`kUTTypePNG`/`UTTypePNG` constant symbolically — a small dependency-avoidance choice in the same
spirit as writing the glTF JSON by hand instead of pulling in a JSON library.

This module needs the CoreGraphics and ImageIO frameworks, which is precisely why it is *not* part
of the framework-free `RayTracerCore` static library that `EntityMesh.cpp` and `SceneExporter.cpp`
live in. Linking `ImageWriter.cpp` into `RayTracerCore` would force every consumer of that library —
including, notably, `RayTracerBenchTests` (Chapter 11), which is meant to test the shared core in
isolation without pulling in AppKit/CoreGraphics — to also link those two frameworks, purely to
support a feature (`writePNG`) that tests have no reason to exercise. Instead, `ImageWriter.cpp` is
linked only into the `RayTracerBench` app target itself, alongside the AppKit/Metal/QuartzCore code
that already requires a full framework set. `EntityMesh` and `SceneExporter` stay framework-free
because they only ever touch plain data (`SceneDescription`, `MeshData`, `std::string`) and the C++
standard library — no CoreGraphics types appear anywhere in their signatures.

## §9. The preview PNG as a whole, and what it cost `AppDelegate`

Putting §4/§5 and §8 together: every successful `.gltf` or `.obj` export is immediately followed by
a same-named `.png` written into the identical `SavedScenes/` directory (via
`ensureSavedScenesDirectoryPath()`, §7). That PNG is not a placeholder, an icon, or a thumbnail
render at reduced quality — it's a full `renderCPU()` invocation (Chapter 4) of the *same*
`SceneDescription` at the *current* controls settings, i.e. exactly the image a "Render CPU" click
would have produced for that seed. The point is that a saved `.gltf`/`.obj` file, viewed outside
this app in a generic 3D tool, would otherwise show the material approximations from §6 with no way
to compare them against what the ray tracer actually produced; the companion PNG gives a real,
correct-quality reference image sitting right next to the exported geometry.

This had a real architectural consequence, recorded directly in `AppDelegate.cpp`'s comment on
`saveScene()`:

```cpp
// Builds a scene from the current controls settings (geometry depends only on seed/width/
// floating — samplesPerPixel/maxDepth affect rendering, not the exported geometry), writes it via
// the requested exporter, and — on success — also renders it (at the current settings, exactly
// like "Render CPU" would) and writes a same-named PNG preview alongside it, so the 3D export has
// a quick-look image without needing a model viewer. Runs on a background thread like the render
// actions above: once a full CPU render is part of this, it's no longer the fast, main-thread-only
// operation it was when it only wrote geometry.
void AppDelegate::saveScene( bool asGLTF )
```
`App/AppDelegate.cpp:379-386`

Before the preview image existed, `saveScene()` only had to build a scene and write geometry to
disk — cheap enough to do synchronously on the main thread without the UI stalling noticeably.
Adding a full CPU render (`renderCPU()` on a scene the size of the default ~490-entity layout, at
whatever samples-per-pixel/max-depth the controls currently specify) is no longer cheap, so
`saveScene()` was moved onto a background `std::thread`, mirroring `startCPURender`/
`startGPURender`/`startCompare`'s existing pattern exactly: settings are read synchronously on the
main thread before the thread is spawned, the thread does the (now much heavier) work, and the
result — including which files were written and whether the PNG succeeded — is marshaled back to
the main thread via `dispatch_async` to show an `NS::Alert` and re-enable the controls. Chapter 8
covers `AppDelegate`'s threading pattern (including this exact function's body) in full; the point
to take from this chapter is only that the *reason* it had to change is the preview PNG's real
render.

## §10. How this was verified

CLAUDE.md's project-status notes record two distinct verification passes for this feature, and it's
worth stating plainly what each one checked, since neither is a stand-in for the other.

The first targeted the exporters' actual output correctness. A standalone harness exported the
real default scene — `buildDefaultScene()`'s full ~490-entity layout, not a toy scene built just
for the test — to both formats, and the results were checked with tools outside this codebase
entirely: `assimp info` (a widely-used 3D-asset inspection tool) and Python's `trimesh` library.
That check confirmed the glTF and OBJ exports agree with each other on triangle counts, that
material colors and material counts came through correctly, and that the glTF JSON is well-formed
per an independent parser — not merely "the file was written without a C++ exception," but "a
third-party tool that has no knowledge of this project's internals can open it and agrees with what
it should contain."

The second targeted the *integration* — the real button-click path, not just the exporter functions
called directly. A temporary, environment-variable-gated hook was added to the app, the app was
actually built and run, and `pkill` was used to terminate it afterward (rather than an in-process
auto-terminate) specifically because the save flow is asynchronous, per §9, and has no natural
"done" signal a test harness could wait on synchronously. That run confirmed a real `.gltf`/`.obj`+
`.mtl` and a real, correctly-rendered `.png` land side by side with the exact same stem, next to the
*actual* running binary — exercising `executableDirectory()` (§2) for real rather than assuming it
resolves correctly, and spot-checking the PNG visually rather than only checking that a file with
the right name exists. The hook itself was then reverted rather than left in the committed source,
consistent with this project's general rule of not shipping ad hoc test scaffolding — the same
technique used elsewhere in this codebase (the main-thread heartbeat check for background
rendering, the offscreen GPU-texture readback test) whenever a claim needed a live, running-app
check that couldn't be made through a unit test alone.

## §11. Reverse: importing a scene this app itself wrote

Everything above turns a `SceneDescription` into a file. `Export/SceneImporter.hpp/.cpp`, added
later at user request (a "Load Scene" button, covered in Chapter 9), goes the other way — but
deliberately not as a general-purpose glTF/OBJ importer:

```cpp
// Reconstructs objects/positions/materials from a .gltf/.obj this app's own SceneExporter.hpp
// previously wrote — the inverse of exportSceneAsGLTF()/exportSceneAsOBJ(). Camera and render
// params (width/aspectRatio/samplesPerPixel/maxDepth/seed) are NOT part of what gets exported (see
// SceneExporter.hpp's header comment — geometry only), so the caller supplies them here exactly
// like buildDefaultScene()'s equivalent parameters; only the entity/material component arrays come
// from the file.
//
// Reconstruction is exact (up to the exported file's own text/float precision) for entities this
// app generated, by exploiting the fact that buildEntityMesh() always produces one of exactly two
// recognizable shapes: a 117-vertex shared-vertex UV sphere (see EntityMesh.cpp's
// kSphereLatSegments/kSphereLonSegments) or an 18-vertex unshared-vertex faceted pyramid — see
// SceneImporter.cpp's reconstructSphere()/reconstructPyramid() for the geometry this decodes back
// out of each. A mesh matching neither vertex-count signature (i.e. not something this app wrote)
// is reported as an error rather than silently guessed at.
SceneImportResult importSceneFromGLTF( const std::string& path, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, unsigned seed );
```
`Export/SceneImporter.hpp:16-30`

This "round-trip reader, not a general importer" framing is what makes exact reconstruction
tractable at all. A sphere's 117 vertices come from an 8-latitude×12-longitude UV tessellation
(Chapter 6) whose bounding box touches `center ± radius` exactly on every axis — so
`reconstructSphere()` recovers center and radius from nothing but the mesh's own axis-aligned
bounding box, independent of vertex order:

```cpp
// A UV sphere tessellated at 30°-longitude steps (kSphereLonSegments=12) always samples
// phi=0/90/180/270 exactly, and the latitude range always reaches the true poles — so its
// bounding box touches center±radius exactly on every axis (see EntityMesh.cpp's
// buildSphereMesh()). That makes the bounding-box center/half-extent recover the original
// center/radius exactly (to the file's own float precision) independent of vertex order —
// unlike the pyramid case below, which does depend on order.
void reconstructSphere( const std::vector<simd_float3>& positions, simd_float3& outCenter, float& outRadius )
{
	simd_float3 minV = positions[ 0 ];
	simd_float3 maxV = positions[ 0 ];
	for ( const simd_float3& p : positions )
	{
		minV.x = std::min( minV.x, p.x );
		minV.y = std::min( minV.y, p.y );
		minV.z = std::min( minV.z, p.z );
		maxV.x = std::max( maxV.x, p.x );
		maxV.y = std::max( maxV.y, p.y );
		maxV.z = std::max( maxV.z, p.z );
	}
	outCenter = simd_make_float3( ( minV.x + maxV.x ) * 0.5f, ( minV.y + maxV.y ) * 0.5f, ( minV.z + maxV.z ) * 0.5f );
	outRadius = ( ( maxV.x - minV.x ) + ( maxV.y - minV.y ) + ( maxV.z - minV.z ) ) / 6.0f; // average of the 3 axis half-extents
}
```
`Export/SceneImporter.cpp:298-319`

The pyramid case is different, and the code says so plainly: a pyramid's 18 vertices don't have a
shape-derived invariant like the sphere's bounding box, so reconstruction instead depends on
`buildPyramidMesh()`'s *fixed emission order* (4 side triangles ending in the apex, then 2 base
triangles) — knowing exactly which of the 18 positions is the apex and which are which base corner
gives a closed-form inverse for position, orientation basis, half-width, and height:

```cpp
// buildPyramidMesh() writes its 18 vertices in a FIXED order: 4 side triangles (each
// corners[i], corners[i+1], apex), then 2 base triangles (corners[0],corners[2],corners[1] and
// corners[0],corners[3],corners[2]) — so, unlike the sphere above, this reconstruction relies
// on that exact emission order rather than just the mesh's overall shape:
//   positions[0]=corner0  positions[1]=corner1  positions[2]=apex
//   positions[4]=corner2 (from triangle 1)      positions[9]=corner3 (from triangle 3)
// Local corners are c0=(hw,0,hw), c1=(hw,0,-hw), c2=(-hw,0,-hw), c3=(-hw,0,hw), apex=(0,h,0), so
// their centroid is the Transform's position; (apex-position) is height*up; (c0-c1) is
// 2*hw*forward; (c0-c3) is 2*hw*right — giving an exact closed-form inverse.
void reconstructPyramid( const std::vector<simd_float3>& positions, TransformGPU& outTransform, float& outBaseHalfWidth, float& outHeight )
```
`Export/SceneImporter.cpp:321-330`

`addReconstructedEntity()` is the one place both readers (glTF and OBJ) dispatch on vertex count to
decide which reconstruction to run — and the one place a file that doesn't match either signature is
rejected rather than guessed at:

```cpp
bool addReconstructedEntity( SceneDescription& scene, const std::vector<simd_float3>& positions, int materialIndex )
{
	if ( positions.size() == (size_t)( kSphereLatSegments + 1 ) * (size_t)( kSphereLonSegments + 1 ) )
	{
		/* ... reconstructSphere(), push a SHAPE_SPHERE entity ... */
		return true;
	}
	if ( positions.size() == kPyramidVertexCount )
	{
		/* ... reconstructPyramid(), push a SHAPE_PYRAMID entity ... */
		return true;
	}
	return false;
}
```
`Export/SceneImporter.cpp:359-388`

Materials are reconstructed by reversing §6's `gltfMaterialFor()`/`mtlFieldsFor()` — a fuzzy
threshold on alpha/metallic for glTF, an exact `illum`-value dispatch for MTL (illum 2/3/4 map
one-to-one back to lambertian/metal/dielectric, so that half is not a guess):

```cpp
//=============================================================================================
// Material reconstruction — the reverse of SceneExporter.cpp's gltfMaterialFor()/
// mtlFieldsFor(). Only lossy where the export itself already was: MAT_DIELECTRIC's index of
// refraction and exact albedo aren't encoded by either format, but this app's own
// buildDefaultScene() (Scene.cpp) always constructs dielectrics with ir=1.5 and albedo=(0,0,0),
// so defaulting to those values reconstructs this app's own scenes exactly, even though a
// hand-authored dielectric material from another tool wouldn't round-trip losslessly.
//=============================================================================================
MaterialGPU materialFromGLTF( simd_float3 baseColor, float metallic, float roughness, float alpha )
{
	if ( alpha < 1.0f )
		return MaterialGPU{ MAT_DIELECTRIC, simd_make_float3( 0.0f, 0.0f, 0.0f ), 0.0f, 1.5f };
	if ( metallic >= 0.5f )
		return MaterialGPU{ MAT_METAL, baseColor, std::max( 0.0f, std::min( roughness / 2.0f, 1.0f ) ), 0.0f };
	return MaterialGPU{ MAT_LAMBERTIAN, baseColor, 0.0f, 0.0f };
}
```
`Export/SceneImporter.cpp:262-277`

The one genuinely lossy case — `MAT_DIELECTRIC`'s index of refraction and exact albedo aren't
encoded by either export format at all (§6) — is a no-op in practice rather than a real gap,
because `buildDefaultScene()` (Chapter 1) always constructs dielectrics with `ir=1.5` and
`albedo=(0,0,0)`, exactly the defaults both `materialFromGLTF()` and `materialFromMTL()` fall back
to. Camera and render parameters were never part of either export format (§1 — geometry only), so
`fillCameraAndParams()` borrows them from a throwaway `buildDefaultScene()` call at the caller's
current settings, the same technique `AppDelegate::sceneForRender()` (Chapter 8) uses for a
*loaded* scene on every subsequent render.

The glTF path needed a minimal hand-rolled JSON parser (`JsonValue`/`JsonParser`,
`Export/SceneImporter.cpp:24-253` — objects, arrays, strings, numbers, booleans, null; not a
validating parser, since its only job is reading files this app's own exporter wrote) and a base64
*decoder* to invert §4's encoder — both are why `Export/Base64.hpp/.cpp` exists as its own shared
module rather than the encoder staying a private helper inside `SceneExporter.cpp` (§4). The decoder
is the mirror image of the encoder — four characters in, up to three bytes out, with `=` padding (or
an early end of string) marking where real data stops:

```cpp
std::vector<uint8_t> base64Decode( const std::string& text )
{
	std::vector<uint8_t> out;
	out.reserve( ( text.size() / 4 ) * 3 );

	int      buf[ 4 ];
	int      bufLen = 0;
	for ( char c : text )
	{
		if ( c == '=' || c == '\0' )
			break; // padding (or an accidental embedded NUL) marks the end of real data
		int v = decodeTable( (unsigned char)c );
		if ( v < 0 )
			continue; // skip anything outside the alphabet rather than fail on it
		buf[ bufLen++ ] = v;
		if ( bufLen == 4 )
		{
			out.push_back( (uint8_t)( ( buf[ 0 ] << 2 ) | ( buf[ 1 ] >> 4 ) ) );
			out.push_back( (uint8_t)( ( buf[ 1 ] << 4 ) | ( buf[ 2 ] >> 2 ) ) );
			out.push_back( (uint8_t)( ( buf[ 2 ] << 6 ) | buf[ 3 ] ) );
			bufLen = 0;
		}
	}
	/* ... leftover partial group (2 or 3 digits recover 1 or 2 trailing bytes) — the inverse of
	       base64Encode()'s rem==1/2 padding cases ... */
	return out;
}
```
`Export/Base64.cpp:53-89`

Verified end-to-end rather than assumed: a round-trip harness exported the real ~490-entity default
scene, ran it back through both importers, and diffed every entity's reconstructed
`Transform`/`Shape`/`Material` against the original — worst-case drift around 1e-6, at float32
text-precision noise, not a structural error — and this comparison is now
`RayTracerBenchTests/SceneImportExportTests.cpp`'s permanent regression coverage (Chapter 11), not
just an ad hoc harness.

---

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) supplies the `SceneDescription` and
  `MaterialGPU` this chapter reads: `scene.transforms`/`scene.shapes`/`scene.materials` are walked
  directly by both exporters, and `MAT_LAMBERTIAN`/`MAT_METAL`/`MAT_DIELECTRIC` are exactly the tags
  §6's material-mapping functions (and §11's reverse of them) switch on.
- **Chapter 6** (`Export/EntityMesh.hpp/.cpp`) is this chapter's geometry source in both directions:
  every mesh written into a `.gltf`/`.obj` file comes from one call to `buildEntityMesh()` per
  entity (§4/§5), and §11's importer only works *because* that function's output has exactly two
  recognizable vertex-count signatures to decode back out of.
- **Chapter 4** (`CPU/CPURenderer.hpp/.cpp`) supplies `renderCPU()`, the function §9's preview PNG
  is built from — the same rendering path a "Render CPU" click uses, run here as a side effect of a
  successful export rather than as a user-initiated render.
- **Chapter 8** (`App/AppDelegate.hpp/.cpp`) is where `saveScene()` (this chapter, §9) and
  `loadScene()`/`sceneForRender()` (§11) actually live and run: their background-`std::thread`
  structure and `dispatch_async` marshaling are covered there in full — this chapter only explains
  *why* each one is shaped the way it is.
- **Chapter 9** (`App/ControlsPanel.hpp/.cpp`) owns the "Save glTF"/"Save OBJ"/"Load Scene" buttons
  that trigger `saveScene()`/`loadScene()`, and `currentSettings()`, which supplies the
  seed/width/floating/spp/maxDepth values this chapter's `sceneFilenameStem()`,
  `buildDefaultScene()`, and §11's importer calls all consume.
- **Chapter 11** (`RayTracerBenchTests/SceneImportExportTests.cpp`) is the permanent regression
  coverage for §11's round-trip claim, and for the SIMD-padding and min/max-precision bugs §4
  documents.
