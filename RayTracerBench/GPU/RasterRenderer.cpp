#include "RasterRenderer.hpp"

#include "CameraMath.hpp"
#include "../Export/EntityMesh.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	// RT_SHADERS_DIR is injected by CMakeLists.txt as the absolute path to RayTracerBench/Shaders —
	// same technique ImageDisplayView.cpp's readShaderSource() already uses for Blit.metal, and for
	// the same reason (Raster.metal has no local #includes, so it needs no xcrun/.metallib build
	// step the way Raytracer.metal does).
	std::string readShaderSource( const char* fileName )
	{
		std::string       path = std::string( RT_SHADERS_DIR ) + "/" + fileName;
		std::ifstream     file( path );
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}

	// KEEP IN SYNC with the identical struct in Shaders/Raster.metal.
	struct RasterVertex
	{
		simd_float3 position;
		simd_float3 normal;
		simd_float3 color;
	};

	// KEEP IN SYNC with the identical struct in Shaders/Raster.metal.
	struct Uniforms
	{
		simd_float4x4 mvp;
	};

	// A flat display color per material — separate from SceneExporter.cpp's gltfMaterialFor()/
	// mtlFieldsFor(), which need full PBR/MTL field sets this rasterizer's flat-Lambertian shading
	// has no use for. MAT_DIELECTRIC is approximated the same way the exporters already document
	// (a clear surface, not modeled) — near-white rather than its near-black albedo (which is only
	// meaningful for refraction, not a flat display color).
	simd_float3 colorForMaterial( const MaterialGPU& mat )
	{
		if ( mat.type == MAT_DIELECTRIC )
			return simd_make_float3( 1.0f, 1.0f, 1.0f );
		return mat.albedo;
	}

}

// Compiles Raster.metal from source, builds the render pipeline + depth-stencil state, and creates
// the command queue. Aborts (rather than leaving a half-constructed renderer) if any step fails.
RasterRenderer::RasterRenderer( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
	, _pVertexBuffer( nullptr )
	, _pIndexBuffer( nullptr )
	, _indexCount( 0 )
	, _pColorTexture( nullptr )
	, _pDepthTexture( nullptr )
	, _textureWidth( 0 )
	, _textureHeight( 0 )
{
	using NS::StringEncoding::UTF8StringEncoding;

	std::string source = readShaderSource( "Raster.metal" );

	NS::Error*    pError = nullptr;
	MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
	if ( !pLibrary )
	{
		std::fprintf( stderr, "RasterRenderer: failed to compile Raster.metal: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	MTL::Function* pVertexFn = pLibrary->newFunction( NS::String::string( "rasterVertex", UTF8StringEncoding ) );
	MTL::Function* pFragmentFn = pLibrary->newFunction( NS::String::string( "rasterFragment", UTF8StringEncoding ) );

	MTL::RenderPipelineDescriptor* pPipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pPipelineDesc->setVertexFunction( pVertexFn );
	pPipelineDesc->setFragmentFunction( pFragmentFn );
	pPipelineDesc->colorAttachments()->object( 0 )->setPixelFormat( MTL::PixelFormatRGBA8Unorm );
	pPipelineDesc->setDepthAttachmentPixelFormat( MTL::PixelFormatDepth32Float );

	_pPipelineState = _pDevice->newRenderPipelineState( pPipelineDesc, &pError );
	if ( !_pPipelineState )
	{
		std::fprintf( stderr, "RasterRenderer: failed to create render pipeline state: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	pVertexFn->release();
	pFragmentFn->release();
	pPipelineDesc->release();
	pLibrary->release();

	MTL::DepthStencilDescriptor* pDepthDesc = MTL::DepthStencilDescriptor::alloc()->init();
	pDepthDesc->setDepthCompareFunction( MTL::CompareFunctionLess );
	pDepthDesc->setDepthWriteEnabled( true );
	_pDepthStencilState = _pDevice->newDepthStencilState( pDepthDesc );
	pDepthDesc->release();

	_pCommandQueue = _pDevice->newCommandQueue();
}

// Releases the color/depth textures, geometry buffers, pipeline/depth-stencil state, queue, and
// device.
RasterRenderer::~RasterRenderer()
{
	if ( _pDepthTexture )
		_pDepthTexture->release();
	if ( _pColorTexture )
		_pColorTexture->release();
	if ( _pIndexBuffer )
		_pIndexBuffer->release();
	if ( _pVertexBuffer )
		_pVertexBuffer->release();
	_pDepthStencilState->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pDevice->release();
}

// Flattens every entity's world-space mesh (buildEntityMesh() — the same mesh builder
// SceneExporter.cpp's glTF/OBJ writers use) plus a flat per-entity material color into one combined
// vertex/index buffer pair, offsetting indices the same way SceneExporter.cpp's OBJ writer already
// does for its own file-global vertex indices.
void RasterRenderer::rebuildGeometryBuffers( const SceneDescription& scene )
{
	if ( _pVertexBuffer )
		_pVertexBuffer->release();
	if ( _pIndexBuffer )
		_pIndexBuffer->release();

	std::vector<RasterVertex> vertices;
	std::vector<uint32_t>     indices;

	const size_t entityCount = scene.transforms.size();
	for ( size_t e = 0; e < entityCount; ++e )
	{
		MeshData    mesh = buildEntityMesh( scene.transforms[ e ], scene.shapes[ e ] );
		simd_float3 color = colorForMaterial( scene.materials[ scene.shapes[ e ].materialIndex ] );

		uint32_t vertexOffset = (uint32_t)vertices.size();
		for ( size_t i = 0; i < mesh.positions.size(); ++i )
			vertices.push_back( RasterVertex{ mesh.positions[ i ], mesh.normals[ i ], color } );
		for ( uint32_t index : mesh.indices )
			indices.push_back( index + vertexOffset );
	}

	const size_t vertexBytes = vertices.size() * sizeof( RasterVertex );
	const size_t indexBytes = indices.size() * sizeof( uint32_t );

	_pVertexBuffer = _pDevice->newBuffer( vertexBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pVertexBuffer->contents(), vertices.data(), vertexBytes );

	_pIndexBuffer = _pDevice->newBuffer( indexBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pIndexBuffer->contents(), indices.data(), indexBytes );

	_indexCount = (uint32_t)indices.size();
}

// (Re)creates the color + depth textures only when width/height actually differ from the last call.
void RasterRenderer::rebuildTargetsIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pColorTexture && width == _textureWidth && height == _textureHeight )
		return;

	if ( _pColorTexture )
		_pColorTexture->release();
	if ( _pDepthTexture )
		_pDepthTexture->release();

	// Same RGBA8Unorm/Shared format GPURenderer's output texture uses, so ImageDisplayView's
	// displayTexture() needs no changes — plus TextureUsageRenderTarget, since this one is a color
	// attachment rather than a compute kernel's ShaderWrite target.
	MTL::TextureDescriptor* pColorDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pColorDesc->setStorageMode( MTL::StorageModeShared );
	pColorDesc->setUsage( MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead );
	_pColorTexture = _pDevice->newTexture( pColorDesc );

	// Depth never needs CPU readback, so Private (GPU-only) storage is fine here, unlike the color
	// texture above.
	MTL::TextureDescriptor* pDepthDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatDepth32Float, width, height, false );
	pDepthDesc->setStorageMode( MTL::StorageModePrivate );
	pDepthDesc->setUsage( MTL::TextureUsageRenderTarget );
	_pDepthTexture = _pDevice->newTexture( pDepthDesc );

	_textureWidth = width;
	_textureHeight = height;
}

// Encodes and draws the whole scene's combined geometry in one drawIndexedPrimitives call, waits
// for completion, and returns the GPU-only elapsed time in milliseconds (from the command buffer's
// own timestamps) — mirrors GPURenderer::dispatchOnce's methodology.
double RasterRenderer::drawOnce( const SceneDescription& scene )
{
	Uniforms uniforms{ buildSceneViewProjectionMatrix( scene.camera ) };

	MTL::RenderPassDescriptor* pPassDesc = MTL::RenderPassDescriptor::renderPassDescriptor();

	MTL::RenderPassColorAttachmentDescriptor* pColorAttachment = pPassDesc->colorAttachments()->object( 0 );
	pColorAttachment->setTexture( _pColorTexture );
	pColorAttachment->setLoadAction( MTL::LoadActionClear );
	pColorAttachment->setStoreAction( MTL::StoreActionStore );
	pColorAttachment->setClearColor( MTL::ClearColor::Make( 0.0, 0.0, 0.0, 1.0 ) );

	MTL::RenderPassDepthAttachmentDescriptor* pDepthAttachment = pPassDesc->depthAttachment();
	pDepthAttachment->setTexture( _pDepthTexture );
	pDepthAttachment->setLoadAction( MTL::LoadActionClear );
	pDepthAttachment->setStoreAction( MTL::StoreActionDontCare );
	pDepthAttachment->setClearDepth( 1.0 );

	MTL::CommandBuffer*        pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::RenderCommandEncoder* pEncoder = pCommandBuffer->renderCommandEncoder( pPassDesc );

	pEncoder->setRenderPipelineState( _pPipelineState );
	pEncoder->setDepthStencilState( _pDepthStencilState );
	// No back-face culling: EntityMesh.cpp's winding order was only ever verified against "the
	// normal points away from the shape's interior" (see RayTracerBenchTests/EntityMeshTests.cpp),
	// never against a specific front-face handedness convention, so culling risks silently dropping
	// correct triangles for negligible benefit at this scene's size.
	pEncoder->setCullMode( MTL::CullModeNone );
	pEncoder->setViewport( MTL::Viewport{ 0.0, 0.0, (double)_textureWidth, (double)_textureHeight, 0.0, 1.0 } );
	pEncoder->setVertexBuffer( _pVertexBuffer, 0, 0 );
	pEncoder->setVertexBytes( &uniforms, sizeof( Uniforms ), 1 );
	pEncoder->drawIndexedPrimitives( MTL::PrimitiveTypeTriangle, _indexCount, MTL::IndexTypeUInt32, _pIndexBuffer, 0 );
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}

// Rebuilds this scene's geometry/render targets, does one untimed warm-up draw, then one timed
// draw; returns the output texture plus wall-clock/GPU-only timings and the triangle count drawn.
RasterRenderResult RasterRenderer::render( const SceneDescription& scene )
{
	rebuildGeometryBuffers( scene );
	rebuildTargetsIfNeeded( scene.params.width, scene.params.height );

	drawOnce( scene ); // untimed warm-up — see header comment

	const auto   startTime = std::chrono::high_resolution_clock::now();
	const double gpuTimeMs = drawOnce( scene );
	const auto   endTime = std::chrono::high_resolution_clock::now();

	RasterRenderResult result;
	result.pTexture = _pColorTexture;
	result.wallClockTime = endTime - startTime;
	result.gpuTimeMs = gpuTimeMs;
	result.triangleCount = _indexCount / 3;
	return result;
}
