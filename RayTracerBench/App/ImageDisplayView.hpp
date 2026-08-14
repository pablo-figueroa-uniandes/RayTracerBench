#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalLayer.hpp>

#include <cstdint>

// A CAMetalLayer-backed NS::View that blits an RGBA8 buffer (the CPU renderer's output today; a
// GPU-rendered MTL::Texture directly, once the GPU renderer exists) onto screen via a tiny
// textured-quad shader (Shaders/Blit.metal) — deliberately not NSImageView, per CLAUDE.md's UI
// architecture.
class ImageDisplayView
{
	public:
		ImageDisplayView( MTL::Device* pDevice, CGRect frame );
		~ImageDisplayView();

		NS::View* view() const { return _pView; }

		// Uploads an RGBA8, row-major, row-0-is-top buffer of exactly width*height*4 bytes and
		// immediately renders + presents it.
		void updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height );

	private:
		void rebuildTextureIfNeeded( uint32_t width, uint32_t height );
		void render();

		MTL::Device*          _pDevice;
		CA::MetalLayer*       _pMetalLayer;
		NS::View*             _pView;
		MTL::CommandQueue*    _pCommandQueue;
		MTL::RenderPipelineState* _pPipelineState;
		MTL::Texture*         _pSourceTexture;
		uint32_t              _textureWidth;
		uint32_t              _textureHeight;
};
