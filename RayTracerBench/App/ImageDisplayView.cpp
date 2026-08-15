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

// Compiles Blit.metal from source, builds the render pipeline state, and wires up the
// CAMetalLayer-backed view. See the header comment for why this is a plain view, not MTKView.
ImageDisplayView::ImageDisplayView( MTL::Device* pDevice, CGRect frame )
	: _pDevice( pDevice->retain() )
	, _viewSize( frame.size )
	, _pOwnedTexture( nullptr )
	, _ownedTextureWidth( 0 )
	, _ownedTextureHeight( 0 )
	, _pCurrentTexture( nullptr )
	, _magnifier{ 0.5f, 0.5f, 1.0f, 0.18f, 4.0f, 0 }
{
	using NS::StringEncoding::UTF8StringEncoding;

	_magnifier.viewAspect = (float)( frame.size.width / frame.size.height );

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

// Releases the owned texture (if any), pipeline state, command queue, layer, and view.
ImageDisplayView::~ImageDisplayView()
{
	if ( _pOwnedTexture )
		_pOwnedTexture->release();
	_pPipelineState->release();
	_pCommandQueue->release();
	_pMetalLayer->release();
	_pView->release();
	_pDevice->release();
}

// (Re)creates _pOwnedTexture only when width/height actually differ from the last call.
void ImageDisplayView::rebuildOwnedTextureIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pOwnedTexture && width == _ownedTextureWidth && height == _ownedTextureHeight )
		return;

	if ( _pOwnedTexture )
		_pOwnedTexture->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageShaderRead );

	_pOwnedTexture = _pDevice->newTexture( pDesc );
	_ownedTextureWidth = width;
	_ownedTextureHeight = height;
}

// Uploads an RGBA8 buffer into the owned texture (recreating it if the size changed), then
// immediately renders and presents.
void ImageDisplayView::updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height )
{
	rebuildOwnedTextureIfNeeded( width, height );

	MTL::Region region = MTL::Region( 0, 0, 0, width, height, 1 );
	_pOwnedTexture->replaceRegion( region, 0, pRGBA, static_cast<size_t>( width ) * 4 );

	_pCurrentTexture = _pOwnedTexture;
	_pMetalLayer->setDrawableSize( CGSizeMake( width, height ) );
	render();
}

// Points rendering at an externally-owned texture (e.g. the GPU renderer's output) and presents.
void ImageDisplayView::displayTexture( MTL::Texture* pTexture )
{
	_pCurrentTexture = pTexture;
	_pMetalLayer->setDrawableSize( CGSizeMake( pTexture->width(), pTexture->height() ) );
	render();
}

// Updates the lens uniforms and, if a texture is already showing, immediately re-renders so the
// lens tracks the mouse live.
void ImageDisplayView::setMagnifier( bool active, float centerU, float centerV, float zoomFactor, float radius )
{
	_magnifier.active = active ? 1 : 0;
	_magnifier.centerU = centerU;
	_magnifier.centerV = centerV;
	_magnifier.zoom = zoomFactor;
	_magnifier.radius = radius;

	if ( _pCurrentTexture )
		render();
}

// Encodes and presents one full-screen-quad blit of _pCurrentTexture through Blit.metal's pipeline.
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
	pEncoder->setFragmentTexture( _pCurrentTexture, 0 );
	pEncoder->setFragmentBytes( &_magnifier, sizeof( _magnifier ), 0 );
	pEncoder->drawPrimitives( MTL::PrimitiveTypeTriangle, ( NS::UInteger )0, ( NS::UInteger )6 );
	pEncoder->endEncoding();

	pCommandBuffer->presentDrawable( pDrawable );
	pCommandBuffer->commit();
}
