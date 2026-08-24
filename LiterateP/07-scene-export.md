# Chapter 7: Scene Export — glTF, OBJ, and Preview Images

**Abstract.** The renderers turn a `SceneDescription` into pixels; this chapter covers the code
that instead turns it into files a 3D modeling tool can open. `Export/SceneExporter.hpp/.cpp`
walks the same entity arrays Chapter 1 describes, calls Chapter 6's `buildEntityMesh()` per
entity, and serializes the result as either a single self-contained `.gltf` file or an
`.obj`+`.mtl` pair, with a from-scratch material mapping into each format's native shading model.
Alongside the 3D file, `Export/ImageWriter.hpp/.cpp` writes a same-named PNG — a real render at the
current settings, not a placeholder — which is why `AppDelegate::saveScene()` (Chapter 8) had to
become a background-thread operation instead of the fast, synchronous, main-thread call it started
out as. This chapter closes with the verification story CLAUDE.md records for this feature: an
external-tool check of a real ~490-entity export, and a temporary env-var-gated hook run through
the built app and then reverted.

Files covered: `Export/SceneExporter.hpp`, `Export/SceneExporter.cpp`, `Export/ImageWriter.hpp`,
`Export/ImageWriter.cpp`.

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
`Export/SceneExporter.cpp:141-156`

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
`Export/SceneExporter.cpp:158-172`

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
a companion `.bin`. The comment on the base64 helper states the reasoning:

```cpp
// Standard base64 alphabet (RFC 4648), used to embed the glTF export's binary buffer as a
// data: URI rather than writing a companion .bin file — keeps a glTF export to one file.
std::string base64Encode( const uint8_t* data, size_t len )
```
`Export/SceneExporter.cpp:18-20`

The function itself is a plain three-bytes-in/four-characters-out RFC 4648 encoder with the usual
`=`/`==` padding for a 1- or 2-byte remainder (`Export/SceneExporter.cpp:20-56`) — nothing
glTF-specific, just infrastructure for what follows.

`exportSceneAsGLTF()`'s job is to walk every entity, hand its `TransformGPU`/`ShapeGPU` pair to
Chapter 6's `buildEntityMesh()`, and fold the resulting `MeshData` into one shared binary buffer
plus the JSON structures (`bufferViews`, `accessors`, `meshes`, `nodes`) that describe how to slice
that buffer back into typed arrays. Two small lambdas do the buffer-appending, each returning the
byte offset the just-appended data starts at (which becomes each `bufferView`'s `byteOffset`):

```cpp
auto appendVec3s = [ &buffer ]( const std::vector<simd_float3>& v ) -> size_t {
	size_t offset = buffer.size();
	buffer.resize( offset + v.size() * sizeof( float ) * 3 );
	if ( !v.empty() )
		std::memcpy( buffer.data() + offset, v.data(), v.size() * sizeof( float ) * 3 );
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
`Export/SceneExporter.cpp:289-302`

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
`Export/SceneExporter.cpp:317-340`

(Normals and indices follow the identical pattern at `Export/SceneExporter.cpp:342-356`, using
`componentType` 5126 (`GL_FLOAT`) for normals and 5125 (`GL_UNSIGNED_INT`) for indices, and
`target` 34963 (`ELEMENT_ARRAY_BUFFER`) instead of 34962 (`ARRAY_BUFFER`) since indices are consumed
differently by a GPU than vertex attributes are — these are the standard glTF/OpenGL enum values,
not project-specific choices.) Each mesh then contributes one `mesh` object referencing its three
accessors and its material index, and one `node` object referencing that mesh
(`Export/SceneExporter.cpp:358-364`) — a flat one-node-per-entity scene graph, with no shared
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
`Export/SceneExporter.cpp:376-390`

The JSON is built with hand-rolled `std::ostringstream` concatenation rather than a JSON library —
consistent with this project's preference (seen already in `Core/` and `CPU/`) for standing on
plain standard-library and system facilities rather than pulling in third-party dependencies for a
narrowly-scoped, fully-controlled output shape. The whole string is then written to a single
`.gltf` file with no companion `.bin` (`Export/SceneExporter.cpp:392-399`).

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
`Export/SceneExporter.cpp:204-221`

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
`Export/SceneExporter.cpp:229-260`

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
`Export/SceneExporter.cpp:89-107`

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
`Export/SceneExporter.cpp:119-138`

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
pair (`Export/SceneExporter.cpp:59-72`) that both `exportSceneAsGLTF()` and `exportSceneAsOBJ()`
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
`Export/SceneExporter.cpp:174-184`

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
`App/AppDelegate.cpp:296-303`

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

---

## Where this connects

- **Chapter 1** (`Core/ShaderTypes.h`, `Core/Scene.hpp/.cpp`) supplies the `SceneDescription` and
  `MaterialGPU` this chapter reads: `scene.transforms`/`scene.shapes`/`scene.materials` are walked
  directly by both exporters, and `MAT_LAMBERTIAN`/`MAT_METAL`/`MAT_DIELECTRIC` are exactly the tags
  §6's material-mapping functions switch on.
- **Chapter 6** (`Export/EntityMesh.hpp/.cpp`) is this chapter's geometry source: every mesh
  written into a `.gltf` or `.obj` file comes from one call to `buildEntityMesh()` per entity: §4
  and §5's loops are, in effect, "for each entity, ask Chapter 6 for its mesh, then serialize it."
- **Chapter 4** (`CPU/CPURenderer.hpp/.cpp`) supplies `renderCPU()`, the function §9's preview PNG
  is built from — the same rendering path a "Render CPU" click uses, run here as a side effect of a
  successful export rather than as a user-initiated render.
- **Chapter 8** (`App/AppDelegate.hpp/.cpp`) is where `saveScene()` actually lives and runs: its
  background-`std::thread` structure, and the `dispatch_async` marshaling back to the main thread,
  are covered there in full — this chapter only explains *why* that structure became necessary.
- **Chapter 9** (`App/ControlsPanel.hpp/.cpp`) owns the "Save glTF"/"Save OBJ" buttons that
  trigger `saveScene()`, and `currentSettings()`, which supplies the seed/width/floating/spp/
  maxDepth values this chapter's `sceneFilenameStem()` and `buildDefaultScene()` calls consume.
