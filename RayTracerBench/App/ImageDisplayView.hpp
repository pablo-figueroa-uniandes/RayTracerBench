#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalLayer.hpp>

#include <cstdint>

// A CAMetalLayer-backed NS::View that blits a texture onto screen via a tiny textured-quad shader
// (Shaders/Blit.metal) — deliberately not NSImageView, per CLAUDE.md's UI architecture. Used two
// ways: updatePixels() uploads a CPU-side RGBA8 buffer into a texture this view owns; displayTexture()
// blits an already-existing MTL::Texture (the GPU renderer's output) directly, borrowed rather than
// owned — it stays GPURenderer's responsibility to free.
class ImageDisplayView
{
	public:
		ImageDisplayView( MTL::Device* pDevice, CGRect frame );
		~ImageDisplayView();

		NS::View* view() const { return _pView; }

		// Uploads an RGBA8, row-major, row-0-is-top buffer of exactly width*height*4 bytes and
		// immediately renders + presents it.
		void updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height );

		// Renders + presents an existing texture directly (e.g. GPURenderer's output). Not owned —
		// must outlive this call, but this view never retains or releases it.
		void displayTexture( MTL::Texture* pTexture );

	private:
		void rebuildOwnedTextureIfNeeded( uint32_t width, uint32_t height );
		void render();

		MTL::Device*              _pDevice;
		CA::MetalLayer*           _pMetalLayer;
		NS::View*                 _pView;
		MTL::CommandQueue*        _pCommandQueue;
		MTL::RenderPipelineState* _pPipelineState;

		MTL::Texture* _pOwnedTexture; // created/uploaded by updatePixels(); released in the destructor
		uint32_t      _ownedTextureWidth;
		uint32_t      _ownedTextureHeight;

		MTL::Texture* _pCurrentTexture; // whichever of the above (or an external texture) render() should sample
};
