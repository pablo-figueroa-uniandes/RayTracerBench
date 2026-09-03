#include "SceneExporter.hpp"

#include "Base64.hpp"
#include "EntityMesh.hpp"

#include <mach-o/dyld.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
	// Where every export is written: a SavedScenes/ subdirectory next to the running executable.
	std::filesystem::path savedScenesDirectory()
	{
		return std::filesystem::path( executableDirectory() ) / "SavedScenes";
	}

	// Creates SavedScenes/ if needed; returns an error message on failure, or empty on success.
	std::string ensureSavedScenesDirectory( const std::filesystem::path& dir )
	{
		std::error_code ec;
		std::filesystem::create_directories( dir, ec );
		if ( ec )
			return "Failed to create output directory " + dir.string() + ": " + ec.message();
		return "";
	}

	//---------------------------------------------------------------------------------------------
	// Material mapping. Neither export format's core spec has a native "glass" model, so
	// MAT_DIELECTRIC is approximated the same conceptual way in both: a clear, smooth,
	// partially-transparent surface, documented as an approximation rather than presented as
	// physically equivalent to the raytraced result.
	//---------------------------------------------------------------------------------------------

	struct GLTFMaterialFields
	{
		simd_float3 baseColor;
		float       metallic;
		float       roughness;
		float       alpha;
	};

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

	struct MTLFields
	{
		simd_float3 kd;
		simd_float3 ks;
		float       ns;
		float       d;
		int         illum;
	};

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
}

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

// Writes every entity's mesh as an "o"/"v"/"vn"/"usemtl"/"f" block into stem.obj, plus the
// materials into a companion stem.mtl — see the header comment for the output location.
SceneExportResult exportSceneAsOBJ( const SceneDescription& scene, const std::string& stem )
{
	SceneExportResult result;
	result.ok = false;

	std::filesystem::path dir = savedScenesDirectory();
	std::string           dirError = ensureSavedScenesDirectory( dir );
	if ( !dirError.empty() )
	{
		result.message = dirError;
		return result;
	}

	std::filesystem::path objPath = dir / ( stem + ".obj" );
	std::filesystem::path mtlPath = dir / ( stem + ".mtl" );

	std::ofstream mtlFile( mtlPath );
	if ( !mtlFile )
	{
		result.message = "Failed to open " + mtlPath.string();
		return result;
	}
	// max_digits10 for float (9) — the default stream precision (6 significant digits) would lose
	// enough precision on round-trip that a loaded scene's geometry/material fields would visibly
	// drift from the original (see the analogous fix and reasoning in exportSceneAsGLTF() below).
	mtlFile << std::setprecision( 9 );
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
	mtlFile.close();

	std::ofstream objFile( objPath );
	if ( !objFile )
	{
		result.message = "Failed to open " + objPath.string();
		return result;
	}
	objFile << std::setprecision( 9 );
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
	objFile.close();

	result.ok = true;
	result.writtenFilePaths = { objPath.string(), mtlPath.string() };
	result.message = "Wrote " + objPath.string() + "\nand " + mtlPath.string();
	return result;
}

// Assembles every entity's mesh data into one shared binary buffer, describes it with glTF
// accessors/bufferViews/meshes/nodes, and writes the whole thing (JSON plus a base64-embedded
// buffer) as a single stem.gltf file — see the header comment for the output location.
SceneExportResult exportSceneAsGLTF( const SceneDescription& scene, const std::string& stem )
{
	SceneExportResult result;
	result.ok = false;

	std::filesystem::path dir = savedScenesDirectory();
	std::string           dirError = ensureSavedScenesDirectory( dir );
	if ( !dirError.empty() )
	{
		result.message = dirError;
		return result;
	}

	std::filesystem::path gltfPath = dir / ( stem + ".gltf" );

	std::vector<uint8_t> buffer; // one shared binary blob for every mesh's positions/normals/indices

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

	std::ostringstream bufferViewsJson;
	std::ostringstream accessorsJson;
	std::ostringstream meshesJson;
	std::ostringstream nodesJson;

	// The default stream precision (6 significant digits) isn't nearly enough: accessor min/max
	// values written at 6 digits routinely round to a bound that's numerically inside the real
	// data range, which glTF's spec (and validators) treat as invalid ("declared min/max must
	// bound the actual data") — confirmed by decoding a real export's buffer and comparing against
	// its declared min/max, which disagreed on ~1469 of ~2946 bound components before this. Nor is
	// max_digits10 for float32 (9) quite enough: JSON numbers are parsed back as double, and a
	// 9-digit decimal that round-trips correctly *as a float* can still land a hair off the exact
	// widened-to-double value once parsed at double precision, leaving a ~1e-8 residual — confirmed
	// by the same decode-and-compare check, which still found violations at 9 digits.
	// max_digits10 for double (17) closes that gap completely.
	accessorsJson << std::setprecision( 17 );
	int  bufferViewCount = 0;
	int  accessorCount = 0;
	int  nodeCount = 0;
	bool firstMesh = true;

	const size_t entityCount = scene.transforms.size();
	for ( size_t e = 0; e < entityCount; ++e )
	{
		MeshData mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
		if ( mesh.positions.empty() )
			continue;

		simd_float3 minV = mesh.positions[ 0 ];
		simd_float3 maxV = mesh.positions[ 0 ];
		for ( const simd_float3& p : mesh.positions )
		{
			minV.x = std::min( minV.x, p.x );
			minV.y = std::min( minV.y, p.y );
			minV.z = std::min( minV.z, p.z );
			maxV.x = std::max( maxV.x, p.x );
			maxV.y = std::max( maxV.y, p.y );
			maxV.z = std::max( maxV.z, p.z );
		}

		size_t posOffset = appendVec3s( mesh.positions );
		int    posBufferView = bufferViewCount++;
		bufferViewsJson << ( posBufferView ? "," : "" ) << "{\"buffer\":0,\"byteOffset\":" << posOffset
						<< ",\"byteLength\":" << ( mesh.positions.size() * sizeof( float ) * 3 ) << ",\"target\":34962}";
		int posAccessor = accessorCount++;
		accessorsJson << ( posAccessor ? "," : "" ) << "{\"bufferView\":" << posBufferView
					  << ",\"componentType\":5126,\"count\":" << mesh.positions.size() << ",\"type\":\"VEC3\",\"min\":[" << minV.x << ","
					  << minV.y << "," << minV.z << "],\"max\":[" << maxV.x << "," << maxV.y << "," << maxV.z << "]}";

		size_t normOffset = appendVec3s( mesh.normals );
		int    normBufferView = bufferViewCount++;
		bufferViewsJson << ",{\"buffer\":0,\"byteOffset\":" << normOffset
						<< ",\"byteLength\":" << ( mesh.normals.size() * sizeof( float ) * 3 ) << ",\"target\":34962}";
		int normAccessor = accessorCount++;
		accessorsJson << ",{\"bufferView\":" << normBufferView << ",\"componentType\":5126,\"count\":" << mesh.normals.size()
					  << ",\"type\":\"VEC3\"}";

		size_t idxOffset = appendIndices( mesh.indices );
		int    idxBufferView = bufferViewCount++;
		bufferViewsJson << ",{\"buffer\":0,\"byteOffset\":" << idxOffset << ",\"byteLength\":" << ( mesh.indices.size() * sizeof( uint32_t ) )
						<< ",\"target\":34963}";
		int idxAccessor = accessorCount++;
		accessorsJson << ",{\"bufferView\":" << idxBufferView << ",\"componentType\":5125,\"count\":" << mesh.indices.size()
					  << ",\"type\":\"SCALAR\"}";

		meshesJson << ( firstMesh ? "" : "," ) << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << posAccessor
				   << ",\"NORMAL\":" << normAccessor << "},\"indices\":" << idxAccessor
				   << ",\"material\":" << scene.shapes[ e ].materialIndex << "}]}";
		nodesJson << ( firstMesh ? "" : "," ) << "{\"mesh\":" << nodeCount << "}";

		++nodeCount;
		firstMesh = false;
	}

	std::ostringstream materialsJson;
	materialsJson << std::setprecision( 17 );
	for ( size_t m = 0; m < scene.materials.size(); ++m )
	{
		GLTFMaterialFields f = gltfMaterialFor( scene.materials[ m ] );
		materialsJson << ( m ? "," : "" ) << "{\"name\":\"Material_" << m << "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" << f.baseColor.x
					  << "," << f.baseColor.y << "," << f.baseColor.z << "," << f.alpha << "],\"metallicFactor\":" << f.metallic
					  << ",\"roughnessFactor\":" << f.roughness << "}" << ( f.alpha < 1.0f ? ",\"alphaMode\":\"BLEND\"" : "" ) << "}";
	}

	std::string base64 = base64Encode( buffer.data(), buffer.size() );

	std::ostringstream json;
	json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"RayTracerBench SceneExporter\"},";
	json << "\"scene\":0,\"scenes\":[{\"nodes\":[";
	for ( int i = 0; i < nodeCount; ++i )
		json << ( i ? "," : "" ) << i;
	json << "]}],";
	json << "\"nodes\":[" << nodesJson.str() << "],";
	json << "\"meshes\":[" << meshesJson.str() << "],";
	json << "\"materials\":[" << materialsJson.str() << "],";
	json << "\"accessors\":[" << accessorsJson.str() << "],";
	json << "\"bufferViews\":[" << bufferViewsJson.str() << "],";
	json << "\"buffers\":[{\"byteLength\":" << buffer.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64 << "\"}]";
	json << "}";

	std::ofstream file( gltfPath );
	if ( !file )
	{
		result.message = "Failed to open " + gltfPath.string();
		return result;
	}
	file << json.str();
	file.close();

	result.ok = true;
	result.writtenFilePaths = { gltfPath.string() };
	result.message = "Wrote " + gltfPath.string();
	return result;
}
