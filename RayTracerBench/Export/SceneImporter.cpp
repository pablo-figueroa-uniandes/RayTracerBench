#include "SceneImporter.hpp"

#include "Base64.hpp"
#include "EntityMesh.hpp" // kSphereLatSegments/kSphereLonSegments — the sphere vertex-count signature

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace
{
	//=============================================================================================
	// A minimal JSON parser — just enough to read the glTF documents SceneExporter.hpp's own
	// exportSceneAsGLTF() writes (and any other reasonably well-formed JSON): objects, arrays,
	// strings, numbers, booleans, null. Not a validating parser (tolerant of trailing content,
	// doesn't reject malformed \u escapes) — its only job here is reading files this app itself
	// wrote, not arbitrary third-party JSON.
	//=============================================================================================
	struct JsonValue
	{
		enum class Type
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object
		};

		Type                                            type = Type::Null;
		bool                                             boolValue = false;
		double                                           numberValue = 0.0;
		std::string                                      stringValue;
		std::vector<JsonValue>                           arrayValue;
		std::vector<std::pair<std::string, JsonValue>>   objectValue;

		const JsonValue* find( const std::string& key ) const
		{
			for ( const auto& kv : objectValue )
				if ( kv.first == key )
					return &kv.second;
			return nullptr;
		}

		double numberOr( const std::string& key, double fallback ) const
		{
			const JsonValue* v = find( key );
			return ( v && v->type == Type::Number ) ? v->numberValue : fallback;
		}
	};

	class JsonParser
	{
		public:
			explicit JsonParser( const std::string& text ) : _text( text ) {}

			bool parse( JsonValue& out )
			{
				skipWhitespace();
				return parseValue( out );
			}

		private:
			const std::string& _text;
			size_t              _pos = 0;

			void skipWhitespace()
			{
				while ( _pos < _text.size() && ( _text[ _pos ] == ' ' || _text[ _pos ] == '\t' || _text[ _pos ] == '\n' || _text[ _pos ] == '\r' ) )
					++_pos;
			}

			bool parseValue( JsonValue& out )
			{
				skipWhitespace();
				if ( _pos >= _text.size() )
					return false;
				char c = _text[ _pos ];
				if ( c == '{' ) return parseObject( out );
				if ( c == '[' ) return parseArray( out );
				if ( c == '"' ) return parseString( out );
				if ( c == 't' || c == 'f' ) return parseBool( out );
				if ( c == 'n' ) return parseNull( out );
				return parseNumber( out );
			}

			bool parseObject( JsonValue& out )
			{
				out.type = JsonValue::Type::Object;
				++_pos; // '{'
				skipWhitespace();
				if ( _pos < _text.size() && _text[ _pos ] == '}' )
				{
					++_pos;
					return true;
				}
				while ( true )
				{
					skipWhitespace();
					JsonValue keyValue;
					if ( _pos >= _text.size() || _text[ _pos ] != '"' || !parseString( keyValue ) )
						return false;
					skipWhitespace();
					if ( _pos >= _text.size() || _text[ _pos ] != ':' )
						return false;
					++_pos; // ':'
					JsonValue value;
					if ( !parseValue( value ) )
						return false;
					out.objectValue.emplace_back( std::move( keyValue.stringValue ), std::move( value ) );
					skipWhitespace();
					if ( _pos < _text.size() && _text[ _pos ] == ',' )
					{
						++_pos;
						continue;
					}
					if ( _pos < _text.size() && _text[ _pos ] == '}' )
					{
						++_pos;
						return true;
					}
					return false;
				}
			}

			bool parseArray( JsonValue& out )
			{
				out.type = JsonValue::Type::Array;
				++_pos; // '['
				skipWhitespace();
				if ( _pos < _text.size() && _text[ _pos ] == ']' )
				{
					++_pos;
					return true;
				}
				while ( true )
				{
					JsonValue value;
					if ( !parseValue( value ) )
						return false;
					out.arrayValue.push_back( std::move( value ) );
					skipWhitespace();
					if ( _pos < _text.size() && _text[ _pos ] == ',' )
					{
						++_pos;
						continue;
					}
					if ( _pos < _text.size() && _text[ _pos ] == ']' )
					{
						++_pos;
						return true;
					}
					return false;
				}
			}

			bool parseString( JsonValue& out )
			{
				out.type = JsonValue::Type::String;
				++_pos; // opening '"'
				std::string result;
				while ( _pos < _text.size() && _text[ _pos ] != '"' )
				{
					char c = _text[ _pos ];
					if ( c == '\\' && _pos + 1 < _text.size() )
					{
						++_pos;
						char e = _text[ _pos ];
						switch ( e )
						{
							case '"': result.push_back( '"' ); break;
							case '\\': result.push_back( '\\' ); break;
							case '/': result.push_back( '/' ); break;
							case 'b': result.push_back( '\b' ); break;
							case 'f': result.push_back( '\f' ); break;
							case 'n': result.push_back( '\n' ); break;
							case 'r': result.push_back( '\r' ); break;
							case 't': result.push_back( '\t' ); break;
							case 'u':
								// Skip the 4 hex digits — this app's own JSON output never emits
								// \uXXXX escapes (every string is plain ASCII), so a full UTF-16
								// decode isn't needed to round-trip our own files.
								_pos += 4;
								break;
							default: result.push_back( e ); break;
						}
						++_pos;
					}
					else
					{
						result.push_back( c );
						++_pos;
					}
				}
				if ( _pos >= _text.size() )
					return false;
				++_pos; // closing '"'
				out.stringValue = std::move( result );
				return true;
			}

			bool parseBool( JsonValue& out )
			{
				if ( _text.compare( _pos, 4, "true" ) == 0 )
				{
					out.type = JsonValue::Type::Bool;
					out.boolValue = true;
					_pos += 4;
					return true;
				}
				if ( _text.compare( _pos, 5, "false" ) == 0 )
				{
					out.type = JsonValue::Type::Bool;
					out.boolValue = false;
					_pos += 5;
					return true;
				}
				return false;
			}

			bool parseNull( JsonValue& out )
			{
				if ( _text.compare( _pos, 4, "null" ) == 0 )
				{
					out.type = JsonValue::Type::Null;
					_pos += 4;
					return true;
				}
				return false;
			}

			bool parseNumber( JsonValue& out )
			{
				size_t start = _pos;
				if ( _pos < _text.size() && ( _text[ _pos ] == '-' || _text[ _pos ] == '+' ) )
					++_pos;
				while ( _pos < _text.size() &&
						( std::isdigit( (unsigned char)_text[ _pos ] ) || _text[ _pos ] == '.' || _text[ _pos ] == 'e' || _text[ _pos ] == 'E' ||
							_text[ _pos ] == '+' || _text[ _pos ] == '-' ) )
					++_pos;
				if ( _pos == start )
					return false;
				out.type = JsonValue::Type::Number;
				out.numberValue = std::strtod( _text.c_str() + start, nullptr );
				return true;
			}
	};

	// Every pyramid mesh buildPyramidMesh() (EntityMesh.cpp) writes is exactly 6 flat triangles (4
	// side faces + 2 base triangles), each with its own 3 unshared vertices — 18 total, always,
	// regardless of the pyramid's dimensions/orientation. Unlike the sphere's tessellation density,
	// this isn't a tunable constant, so it's not worth exposing from EntityMesh.hpp the way
	// kSphereLatSegments/kSphereLonSegments are.
	constexpr size_t kPyramidVertexCount = 18;

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

	// illum 2/3/4 (lambertian/metal/dielectric — see mtlFieldsFor()) are unique per material type,
	// so — unlike the glTF path's alpha/metallic thresholds — this dispatch is exact, not fuzzy. Ks
	// recovers a metal's albedo exactly too: mtlFieldsFor() writes Ks = mat.albedo verbatim.
	MaterialGPU materialFromMTL( simd_float3 kd, simd_float3 ks, float ns, int illum )
	{
		if ( illum == 4 )
			return MaterialGPU{ MAT_DIELECTRIC, simd_make_float3( 0.0f, 0.0f, 0.0f ), 0.0f, 1.5f };
		if ( illum == 3 )
		{
			float roughness = std::max( 0.0f, std::min( 1.0f, 1.0f - ( ns - 8.0f ) / 300.0f ) );
			return MaterialGPU{ MAT_METAL, ks, roughness / 2.0f, 0.0f };
		}
		return MaterialGPU{ MAT_LAMBERTIAN, kd, 0.0f, 0.0f };
	}

	//=============================================================================================
	// Geometry reconstruction — the reverse of EntityMesh.cpp's buildSphereMesh()/buildPyramidMesh().
	//=============================================================================================

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
	{
		simd_float3 corner0 = positions[ 0 ];
		simd_float3 corner1 = positions[ 1 ];
		simd_float3 apex = positions[ 2 ];
		simd_float3 corner2 = positions[ 4 ];
		simd_float3 corner3 = positions[ 9 ];

		simd_float3 position = ( corner0 + corner1 + corner2 + corner3 ) * 0.25f;
		simd_float3 toApex = apex - position;
		float       height = simd_length( toApex );
		simd_float3 up = height > 0.0f ? toApex / height : simd_make_float3( 0.0f, 1.0f, 0.0f );

		simd_float3 forwardUnnormalized = corner0 - corner1;
		simd_float3 rightUnnormalized = corner0 - corner3;
		float       forwardLen = simd_length( forwardUnnormalized );
		float       rightLen = simd_length( rightUnnormalized );

		outTransform.position = position;
		outTransform.up = up;
		outTransform.forward = forwardLen > 0.0f ? forwardUnnormalized / forwardLen : simd_make_float3( 0.0f, 0.0f, 1.0f );
		outTransform.right = rightLen > 0.0f ? rightUnnormalized / rightLen : simd_make_float3( 1.0f, 0.0f, 0.0f );
		outBaseHalfWidth = ( forwardLen + rightLen ) / 4.0f; // average of both independent estimates
		outHeight = height;
	}

	// Pushes one reconstructed entity from a mesh's raw position list — shared by both the glTF and
	// OBJ readers below, so the sphere/pyramid vertex-count dispatch lives in exactly one place.
	// Returns false (and leaves `scene` untouched) if `positions` matches neither known signature.
	bool addReconstructedEntity( SceneDescription& scene, const std::vector<simd_float3>& positions, int materialIndex )
	{
		if ( positions.size() == (size_t)( kSphereLatSegments + 1 ) * (size_t)( kSphereLonSegments + 1 ) )
		{
			simd_float3 center;
			float       radius;
			reconstructSphere( positions, center, radius );

			TransformGPU transform;
			transform.position = center;
			transform.right = simd_make_float3( 1.0f, 0.0f, 0.0f );
			transform.up = simd_make_float3( 0.0f, 1.0f, 0.0f );
			transform.forward = simd_make_float3( 0.0f, 0.0f, 1.0f );

			scene.transforms.push_back( transform );
			scene.shapes.push_back( ShapeGPU{ SHAPE_SPHERE, radius, 0.0f, 0.0f, materialIndex } );
			return true;
		}
		if ( positions.size() == kPyramidVertexCount )
		{
			TransformGPU transform;
			float        baseHalfWidth, height;
			reconstructPyramid( positions, transform, baseHalfWidth, height );

			scene.transforms.push_back( transform );
			scene.shapes.push_back( ShapeGPU{ SHAPE_PYRAMID, 0.0f, baseHalfWidth, height, materialIndex } );
			return true;
		}
		return false;
	}

	// Camera/render params aren't part of what either export format stores (see
	// SceneExporter.hpp) — buildDefaultScene()'s camera setup is a fixed function of only
	// seed/width/aspectRatio/samplesPerPixel/maxDepth, so building a throwaway default scene and
	// borrowing its camera/params is simpler and less error-prone than hand-duplicating
	// Scene.cpp's private makeCamera().
	void fillCameraAndParams( SceneDescription& scene, unsigned seed, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth )
	{
		SceneDescription cameraSource = buildDefaultScene( seed, width, aspectRatio, samplesPerPixel, maxDepth, false );
		scene.camera = cameraSource.camera;
		scene.params = cameraSource.params;
	}

	std::string trimTrailing( std::string s )
	{
		while ( !s.empty() && ( s.back() == '\r' || s.back() == ' ' || s.back() == '\t' ) )
			s.pop_back();
		return s;
	}
}

SceneImportResult importSceneFromGLTF( const std::string& path, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, unsigned seed )
{
	SceneImportResult result;
	result.ok = false;

	std::ifstream file( path, std::ios::binary );
	if ( !file )
	{
		result.message = "Failed to open " + path;
		return result;
	}
	std::ostringstream fileContents;
	fileContents << file.rdbuf();
	std::string text = fileContents.str();

	JsonValue  root;
	JsonParser parser( text );
	if ( !parser.parse( root ) || root.type != JsonValue::Type::Object )
	{
		result.message = "Failed to parse glTF JSON in " + path;
		return result;
	}

	const JsonValue* buffers = root.find( "buffers" );
	if ( !buffers || buffers->arrayValue.empty() )
	{
		result.message = "glTF has no buffers: " + path;
		return result;
	}
	const JsonValue* uriValue = buffers->arrayValue[ 0 ].find( "uri" );
	if ( !uriValue || uriValue->type != JsonValue::Type::String )
	{
		result.message = "glTF buffer has no embedded data: URI — external .bin buffers aren't supported "
						  "(only files this app's own exporter wrote, which always embeds the buffer, are): " +
			path;
		return result;
	}
	size_t commaPos = uriValue->stringValue.find( ',' );
	if ( commaPos == std::string::npos )
	{
		result.message = "Malformed data: URI in " + path;
		return result;
	}
	std::vector<uint8_t> raw = base64Decode( uriValue->stringValue.substr( commaPos + 1 ) );

	const JsonValue* accessors = root.find( "accessors" );
	const JsonValue* bufferViews = root.find( "bufferViews" );
	const JsonValue* meshes = root.find( "meshes" );
	const JsonValue* materialsJson = root.find( "materials" );
	if ( !accessors || !bufferViews || !meshes )
	{
		result.message = "glTF is missing accessors/bufferViews/meshes: " + path;
		return result;
	}

	// Reads one VEC3 float accessor's data straight out of the decoded buffer.
	auto readPositions = [ & ]( int accessorIndex ) -> std::vector<simd_float3> {
		std::vector<simd_float3> out;
		if ( accessorIndex < 0 || (size_t)accessorIndex >= accessors->arrayValue.size() )
			return out;
		const JsonValue& acc = accessors->arrayValue[ accessorIndex ];
		int              bvIndex = (int)acc.numberOr( "bufferView", -1.0 );
		if ( bvIndex < 0 || (size_t)bvIndex >= bufferViews->arrayValue.size() )
			return out;
		const JsonValue& bv = bufferViews->arrayValue[ bvIndex ];
		size_t           byteOffset = (size_t)bv.numberOr( "byteOffset", 0.0 );
		size_t           count = (size_t)acc.numberOr( "count", 0.0 );
		out.reserve( count );
		for ( size_t i = 0; i < count; ++i )
		{
			size_t off = byteOffset + i * 3 * sizeof( float );
			if ( off + 12 > raw.size() )
				break;
			float x, y, z;
			std::memcpy( &x, raw.data() + off, 4 );
			std::memcpy( &y, raw.data() + off + 4, 4 );
			std::memcpy( &z, raw.data() + off + 8, 4 );
			out.push_back( simd_make_float3( x, y, z ) );
		}
		return out;
	};

	// Remaps a glTF material index to this scene's materials[] index the first time it's seen, so
	// a material genuinely shared by multiple meshes stays shared here too (rather than being
	// duplicated once per entity).
	std::vector<int> materialRemap( materialsJson ? materialsJson->arrayValue.size() : 0, -1 );

	int skippedMeshCount = 0;
	for ( const JsonValue& mesh : meshes->arrayValue )
	{
		const JsonValue* primitivesValue = mesh.find( "primitives" );
		if ( !primitivesValue || primitivesValue->arrayValue.empty() )
			continue;
		const JsonValue& primitive = primitivesValue->arrayValue[ 0 ];
		const JsonValue* attributesValue = primitive.find( "attributes" );
		const JsonValue* positionAccessorValue = attributesValue ? attributesValue->find( "POSITION" ) : nullptr;
		if ( !positionAccessorValue )
			continue;

		std::vector<simd_float3> positions = readPositions( (int)positionAccessorValue->numberValue );

		int          materialIndex = (int)primitive.numberOr( "material", -1.0 );
		MaterialGPU  material{ MAT_LAMBERTIAN, simd_make_float3( 0.8f, 0.8f, 0.8f ), 0.0f, 0.0f };
		if ( materialsJson && materialIndex >= 0 && (size_t)materialIndex < materialsJson->arrayValue.size() )
		{
			const JsonValue& matJson = materialsJson->arrayValue[ materialIndex ];
			const JsonValue* pbr = matJson.find( "pbrMetallicRoughness" );
			simd_float3      baseColor = simd_make_float3( 1.0f, 1.0f, 1.0f );
			float            metallic = 0.0f, roughness = 1.0f, alpha = 1.0f;
			if ( pbr )
			{
				const JsonValue* baseColorValue = pbr->find( "baseColorFactor" );
				if ( baseColorValue && baseColorValue->arrayValue.size() >= 4 )
				{
					baseColor = simd_make_float3(
						(float)baseColorValue->arrayValue[ 0 ].numberValue, (float)baseColorValue->arrayValue[ 1 ].numberValue,
						(float)baseColorValue->arrayValue[ 2 ].numberValue );
					alpha = (float)baseColorValue->arrayValue[ 3 ].numberValue;
				}
				metallic = (float)pbr->numberOr( "metallicFactor", 0.0 );
				roughness = (float)pbr->numberOr( "roughnessFactor", 1.0 );
			}
			material = materialFromGLTF( baseColor, metallic, roughness, alpha );
		}

		int resolvedMaterialIndex;
		if ( materialIndex >= 0 && (size_t)materialIndex < materialRemap.size() && materialRemap[ materialIndex ] >= 0 )
		{
			resolvedMaterialIndex = materialRemap[ materialIndex ];
		}
		else
		{
			resolvedMaterialIndex = (int)result.scene.materials.size();
			result.scene.materials.push_back( material );
			if ( materialIndex >= 0 && (size_t)materialIndex < materialRemap.size() )
				materialRemap[ materialIndex ] = resolvedMaterialIndex;
		}

		if ( !addReconstructedEntity( result.scene, positions, resolvedMaterialIndex ) )
			++skippedMeshCount;
	}

	if ( result.scene.transforms.empty() )
	{
		result.message = "No recognizable entities found in " + path + " (not a scene this app exported?)";
		return result;
	}

	fillCameraAndParams( result.scene, seed, width, aspectRatio, samplesPerPixel, maxDepth );

	result.ok = true;
	result.message = "Loaded " + std::to_string( result.scene.transforms.size() ) + " entities from " + path;
	if ( skippedMeshCount > 0 )
		result.message += " (" + std::to_string( skippedMeshCount ) + " unrecognized mesh(es) skipped)";
	return result;
}

SceneImportResult importSceneFromOBJ( const std::string& path, uint32_t width, float aspectRatio, uint32_t samplesPerPixel, uint32_t maxDepth, unsigned seed )
{
	SceneImportResult result;
	result.ok = false;

	std::ifstream objFile( path );
	if ( !objFile )
	{
		result.message = "Failed to open " + path;
		return result;
	}

	std::vector<std::string> objLines;
	{
		std::string line;
		while ( std::getline( objFile, line ) )
			objLines.push_back( line );
	}

	// Locate the companion .mtl via the .obj's own "mtllib" line — matches how exportSceneAsOBJ()
	// names it (mtllib <stem>.mtl, written into the same directory as the .obj).
	std::string mtlName;
	for ( const std::string& line : objLines )
	{
		if ( line.rfind( "mtllib ", 0 ) == 0 )
		{
			mtlName = trimTrailing( line.substr( 7 ) );
			break;
		}
	}
	if ( mtlName.empty() )
	{
		result.message = "No mtllib line found in " + path;
		return result;
	}
	std::filesystem::path mtlPath = std::filesystem::path( path ).parent_path() / mtlName;

	std::ifstream mtlFile( mtlPath );
	if ( !mtlFile )
	{
		result.message = "Failed to open companion material file " + mtlPath.string();
		return result;
	}

	std::unordered_map<std::string, int> materialNameToIndex;
	{
		std::string line;
		std::string currentName;
		simd_float3 kd = simd_make_float3( 0.8f, 0.8f, 0.8f );
		simd_float3 ks = simd_make_float3( 0.0f, 0.0f, 0.0f );
		float       ns = 8.0f;
		int         illum = 2;
		bool        havePending = false;

		auto flush = [ & ]() {
			if ( !havePending )
				return;
			materialNameToIndex[ currentName ] = (int)result.scene.materials.size();
			result.scene.materials.push_back( materialFromMTL( kd, ks, ns, illum ) );
			havePending = false;
		};

		while ( std::getline( mtlFile, line ) )
		{
			std::istringstream lineStream( line );
			std::string        token;
			lineStream >> token;
			if ( token == "newmtl" )
			{
				flush();
				lineStream >> currentName;
				kd = simd_make_float3( 0.8f, 0.8f, 0.8f );
				ks = simd_make_float3( 0.0f, 0.0f, 0.0f );
				ns = 8.0f;
				illum = 2;
				havePending = true;
			}
			else if ( token == "Kd" )
			{
				float x, y, z;
				lineStream >> x >> y >> z;
				kd = simd_make_float3( x, y, z );
			}
			else if ( token == "Ks" )
			{
				float x, y, z;
				lineStream >> x >> y >> z;
				ks = simd_make_float3( x, y, z );
			}
			else if ( token == "Ns" )
				lineStream >> ns;
			else if ( token == "illum" )
				lineStream >> illum;
		}
		flush();
	}

	if ( result.scene.materials.empty() )
	{
		result.message = "No materials found in " + mtlPath.string();
		return result;
	}

	int                       skippedMeshCount = 0;
	std::vector<simd_float3>  currentPositions;
	int                       currentMaterialIndex = 0;
	bool                      haveEntity = false;

	auto finalizeEntity = [ & ]() {
		if ( !haveEntity || currentPositions.empty() )
			return;
		if ( !addReconstructedEntity( result.scene, currentPositions, currentMaterialIndex ) )
			++skippedMeshCount;
		currentPositions.clear();
	};

	for ( const std::string& line : objLines )
	{
		if ( line.rfind( "o ", 0 ) == 0 )
		{
			finalizeEntity();
			haveEntity = true;
		}
		else if ( line.rfind( "v ", 0 ) == 0 )
		{
			std::istringstream lineStream( line.substr( 2 ) );
			float              x, y, z;
			lineStream >> x >> y >> z;
			currentPositions.push_back( simd_make_float3( x, y, z ) );
		}
		else if ( line.rfind( "usemtl ", 0 ) == 0 )
		{
			std::string name = trimTrailing( line.substr( 7 ) );
			auto        it = materialNameToIndex.find( name );
			currentMaterialIndex = ( it != materialNameToIndex.end() ) ? it->second : 0;
		}
	}
	finalizeEntity();

	if ( result.scene.transforms.empty() )
	{
		result.message = "No recognizable entities found in " + path + " (not a scene this app exported?)";
		return result;
	}

	fillCameraAndParams( result.scene, seed, width, aspectRatio, samplesPerPixel, maxDepth );

	result.ok = true;
	result.message = "Loaded " + std::to_string( result.scene.transforms.size() ) + " entities from " + path;
	if ( skippedMeshCount > 0 )
		result.message += " (" + std::to_string( skippedMeshCount ) + " unrecognized mesh(es) skipped)";
	return result;
}
