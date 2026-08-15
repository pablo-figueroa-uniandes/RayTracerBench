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
		// Compiles Blit.metal, builds the render pipeline state, and creates the CAMetalLayer-backed
		// view sized to `frame`.
		ImageDisplayView( MTL::Device* pDevice, CGRect frame );
		// Releases the owned texture (if any), the pipeline state, command queue, layer, and view.
		~ImageDisplayView();

		// The view an owner should add as a subview.
		NS::View* view() const { return _pView; }

		// Uploads an RGBA8, row-major, row-0-is-top buffer of exactly width*height*4 bytes and
		// immediately renders + presents it.
		void updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height );

		// Renders + presents an existing texture directly (e.g. GPURenderer's output). Not owned —
		// must outlive this call, but this view never retains or releases it.
		void displayTexture( MTL::Texture* pTexture );

		// The view's current pixel size.
		CGSize size() const { return _viewSize; }

		// centerU/centerV: lens center in the same normalized [0,1] UV convention as the source
		// texture (V=0 at the top row — see Blit.metal). Re-renders immediately against whichever
		// texture was last shown, so the lens updates live as the mouse moves without needing new
		// pixel data.
		void setMagnifier( bool active, float centerU, float centerV, float zoomFactor = 4.0f, float radius = 0.18f );

	private:
		// KEEP IN SYNC with the identical struct in Shaders/Blit.metal.
		struct MagnifierUniforms
		{
			float centerU;
			float centerV;
			float viewAspect;
			float radius;
			float zoom;
			int   active;
		};

		// (Re)creates _pOwnedTexture only when width/height actually change from last time.
		void rebuildOwnedTextureIfNeeded( uint32_t width, uint32_t height );
		// Encodes and presents one blit of _pCurrentTexture through Blit.metal's pipeline.
		void render();

		MTL::Device*              _pDevice;
		CA::MetalLayer*           _pMetalLayer;
		NS::View*                 _pView;
		MTL::CommandQueue*        _pCommandQueue;
		MTL::RenderPipelineState* _pPipelineState;
		CGSize                    _viewSize;

		MTL::Texture* _pOwnedTexture; // created/uploaded by updatePixels(); released in the destructor
		uint32_t      _ownedTextureWidth;
		uint32_t      _ownedTextureHeight;

		MTL::Texture*      _pCurrentTexture; // whichever of the above (or an external texture) render() should sample
		MagnifierUniforms  _magnifier;
};
