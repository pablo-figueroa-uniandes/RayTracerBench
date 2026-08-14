#include "GPURenderer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

GPURenderer::GPURenderer( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
	, _pTransformBuffer( nullptr )
	, _pShapeBuffer( nullptr )
	, _pMaterialBuffer( nullptr )
	, _pOutputTexture( nullptr )
	, _textureWidth( 0 )
	, _textureHeight( 0 )
{
	using NS::StringEncoding::UTF8StringEncoding;

	// RT_RAYTRACER_METALLIB (injected by CMakeLists.txt) is a real compiled .metallib, not a
	// source string like ImageDisplayView's Blit.metal — Raytracer.metal #includes Core/ShaderTypes.h
	// and Core/RayTraceCore.h verbatim, and a raw source string has no file location for those
	// relative #includes to resolve against. See CMakeLists.txt's comment for the build-time step.
	NS::Error*    pError = nullptr;
	MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( RT_RAYTRACER_METALLIB, UTF8StringEncoding ), &pError );
	if ( !pLibrary )
	{
		std::fprintf( stderr, "GPURenderer: failed to load Raytracer.metallib: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	MTL::Function* pKernelFn = pLibrary->newFunction( NS::String::string( "renderKernel", UTF8StringEncoding ) );

	_pPipelineState = _pDevice->newComputePipelineState( pKernelFn, &pError );
	if ( !_pPipelineState )
	{
		std::fprintf( stderr, "GPURenderer: failed to create compute pipeline state: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	pKernelFn->release();
	pLibrary->release();

	_pCommandQueue = _pDevice->newCommandQueue();
}

GPURenderer::~GPURenderer()
{
	if ( _pOutputTexture )
		_pOutputTexture->release();
	if ( _pMaterialBuffer )
		_pMaterialBuffer->release();
	if ( _pShapeBuffer )
		_pShapeBuffer->release();
	if ( _pTransformBuffer )
		_pTransformBuffer->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pDevice->release();
}

void GPURenderer::rebuildBuffers( const SceneDescription& scene )
{
	// Recreated on every render() call rather than capacity-cached: device/queue/pipeline are the
	// expensive, cache-worthy resources (shader compile latency); a few Shared-mode buffer
	// allocations per on-demand render is negligible in comparison.
	if ( _pTransformBuffer )
		_pTransformBuffer->release();
	if ( _pShapeBuffer )
		_pShapeBuffer->release();
	if ( _pMaterialBuffer )
		_pMaterialBuffer->release();

	const size_t transformBytes = scene.transforms.size() * sizeof( TransformGPU );
	const size_t shapeBytes = scene.shapes.size() * sizeof( ShapeGPU );
	const size_t materialBytes = scene.materials.size() * sizeof( MaterialGPU );

	_pTransformBuffer = _pDevice->newBuffer( transformBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pTransformBuffer->contents(), scene.transforms.data(), transformBytes );

	_pShapeBuffer = _pDevice->newBuffer( shapeBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pShapeBuffer->contents(), scene.shapes.data(), shapeBytes );

	_pMaterialBuffer = _pDevice->newBuffer( materialBytes, MTL::ResourceStorageModeShared );
	std::memcpy( _pMaterialBuffer->contents(), scene.materials.data(), materialBytes );
}

void GPURenderer::rebuildTextureIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pOutputTexture && width == _textureWidth && height == _textureHeight )
		return;

	if ( _pOutputTexture )
		_pOutputTexture->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	// Shared (not Private): ImageDisplayView blits it directly on Apple Silicon's unified memory,
	// and the deterministic-parity check reads it back on the CPU via getBytes() — both need it
	// to not be Private.
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageShaderWrite | MTL::TextureUsageShaderRead );

	_pOutputTexture = _pDevice->newTexture( pDesc );
	_textureWidth = width;
	_textureHeight = height;
}

double GPURenderer::dispatchOnce( const SceneDescription& scene )
{
	uint32_t entityCount = static_cast<uint32_t>( scene.transforms.size() );

	MTL::CommandBuffer*         pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::ComputeCommandEncoder* pEncoder = pCommandBuffer->computeCommandEncoder();

	pEncoder->setComputePipelineState( _pPipelineState );
	pEncoder->setBuffer( _pTransformBuffer, 0, 0 );
	pEncoder->setBuffer( _pShapeBuffer, 0, 1 );
	pEncoder->setBytes( &entityCount, sizeof( entityCount ), 2 );
	pEncoder->setBuffer( _pMaterialBuffer, 0, 3 );
	pEncoder->setBytes( &scene.camera, sizeof( CameraGPU ), 4 );
	pEncoder->setBytes( &scene.params, sizeof( RenderParams ), 5 );
	pEncoder->setTexture( _pOutputTexture, 0 );

	const MTL::Size threadsPerThreadgroup( 8, 8, 1 );
	const MTL::Size threadsPerGrid( scene.params.width, scene.params.height, 1 );
	pEncoder->dispatchThreads( threadsPerGrid, threadsPerThreadgroup );
	pEncoder->endEncoding();

	pCommandBuffer->commit();
	pCommandBuffer->waitUntilCompleted();

	return ( pCommandBuffer->GPUEndTime() - pCommandBuffer->GPUStartTime() ) * 1000.0;
}

GPURenderResult GPURenderer::render( const SceneDescription& scene )
{
	rebuildBuffers( scene );
	rebuildTextureIfNeeded( scene.params.width, scene.params.height );

	dispatchOnce( scene ); // untimed warm-up — see header comment

	const auto startTime = std::chrono::high_resolution_clock::now();
	const double gpuTimeMs = dispatchOnce( scene );
	const auto endTime = std::chrono::high_resolution_clock::now();

	GPURenderResult result;
	result.pTexture = _pOutputTexture;
	result.wallClockTime = endTime - startTime;
	result.gpuTimeMs = gpuTimeMs;
	return result;
}
