// This is the one translation unit that includes Metal/QuartzCore headers, so it's the one that
// must actually define their private-implementation macros (each emits real global symbol
// definitions for its header's private class/selector caches — must happen in exactly one TU).
// NS_PRIVATE_IMPLEMENTATION already lives in main.cpp, which owns Foundation/AppKit instead.
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "ImageDisplayView.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	// RT_SHADERS_DIR is injected by CMakeLists.txt as the absolute path to RayTracerBench/Shaders.
	// Loading .metal source from disk at runtime (rather than a compiled-in default.metallib) is a
	// deliberate consequence of this project deliberately not being an app bundle (see CLAUDE.md's
	// "Command Line Tool template, not App template" decision) — Apple's own LearnMetalCPP samples
	// use the equivalent technique of compiling MSL from a source string via newLibrary() rather
	// than newDefaultLibrary(), just with the string embedded instead of read from a sibling file.
	std::string readShaderSource( const char* fileName )
	{
		std::string   path = std::string( RT_SHADERS_DIR ) + "/" + fileName;
		std::ifstream file( path );
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}
}

ImageDisplayView::ImageDisplayView( MTL::Device* pDevice, CGRect frame )
	: _pDevice( pDevice->retain() )
	, _pSourceTexture( nullptr )
	, _textureWidth( 0 )
	, _textureHeight( 0 )
{
	using NS::StringEncoding::UTF8StringEncoding;

	_pView = NS::View::alloc()->init( frame );
	_pView->setWantsLayer( true );

	_pMetalLayer = CA::MetalLayer::layer();
	_pMetalLayer->setDevice( _pDevice );
	_pMetalLayer->setPixelFormat( MTL::PixelFormatBGRA8Unorm );
	_pMetalLayer->setFramebufferOnly( true );
	_pMetalLayer->setDrawableSize( frame.size );
	_pView->setLayer( _pMetalLayer );

	std::string source = readShaderSource( "Blit.metal" );

	NS::Error* pError = nullptr;
	MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
	if ( !pLibrary )
	{
		std::fprintf( stderr, "ImageDisplayView: failed to compile Blit.metal: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	MTL::Function* pVertexFn = pLibrary->newFunction( NS::String::string( "blitVertex", UTF8StringEncoding ) );
	MTL::Function* pFragmentFn = pLibrary->newFunction( NS::String::string( "blitFragment", UTF8StringEncoding ) );

	MTL::RenderPipelineDescriptor* pPipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pPipelineDesc->setVertexFunction( pVertexFn );
	pPipelineDesc->setFragmentFunction( pFragmentFn );
	pPipelineDesc->colorAttachments()->object( 0 )->setPixelFormat( MTL::PixelFormatBGRA8Unorm );

	_pPipelineState = _pDevice->newRenderPipelineState( pPipelineDesc, &pError );
	if ( !_pPipelineState )
	{
		std::fprintf( stderr, "ImageDisplayView: failed to create render pipeline state: %s\n",
			pError ? pError->localizedDescription()->utf8String() : "unknown error" );
		std::abort();
	}

	pVertexFn->release();
	pFragmentFn->release();
	pPipelineDesc->release();
	pLibrary->release();

	_pCommandQueue = _pDevice->newCommandQueue();
}

ImageDisplayView::~ImageDisplayView()
{
	if ( _pSourceTexture )
		_pSourceTexture->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pMetalLayer->release();
	_pView->release();
	_pDevice->release();
}

void ImageDisplayView::rebuildTextureIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pSourceTexture && width == _textureWidth && height == _textureHeight )
		return;

	if ( _pSourceTexture )
		_pSourceTexture->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageShaderRead );

	_pSourceTexture = _pDevice->newTexture( pDesc );
	_textureWidth = width;
	_textureHeight = height;

	_pMetalLayer->setDrawableSize( CGSizeMake( width, height ) );
}

void ImageDisplayView::updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height )
{
	rebuildTextureIfNeeded( width, height );

	MTL::Region region = MTL::Region( 0, 0, 0, width, height, 1 );
	_pSourceTexture->replaceRegion( region, 0, pRGBA, static_cast<size_t>( width ) * 4 );

	render();
}

void ImageDisplayView::render()
{
	CA::MetalDrawable* pDrawable = _pMetalLayer->nextDrawable();
	if ( !pDrawable )
		return;

	MTL::RenderPassDescriptor* pPassDesc = MTL::RenderPassDescriptor::renderPassDescriptor();
	MTL::RenderPassColorAttachmentDescriptor* pColorAttachment = pPassDesc->colorAttachments()->object( 0 );
	pColorAttachment->setTexture( pDrawable->texture() );
	pColorAttachment->setLoadAction( MTL::LoadActionClear );
	pColorAttachment->setStoreAction( MTL::StoreActionStore );
	pColorAttachment->setClearColor( MTL::ClearColor::Make( 0.0, 0.0, 0.0, 1.0 ) );

	MTL::CommandBuffer* pCommandBuffer = _pCommandQueue->commandBuffer();
	MTL::RenderCommandEncoder* pEncoder = pCommandBuffer->renderCommandEncoder( pPassDesc );
	pEncoder->setRenderPipelineState( _pPipelineState );
	pEncoder->setFragmentTexture( _pSourceTexture, 0 );
	pEncoder->drawPrimitives( MTL::PrimitiveTypeTriangle, ( NS::UInteger )0, ( NS::UInteger )6 );
	pEncoder->endEncoding();

	pCommandBuffer->presentDrawable( pDrawable );
	pCommandBuffer->commit();
}
