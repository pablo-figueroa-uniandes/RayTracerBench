#pragma once

#include "../Core/Scene.hpp"

#include <Metal/Metal.hpp>

#include <chrono>
#include <cstdint>

struct RasterRenderResult
{
	// Not owned by the caller; valid until the next render() call on this RasterRenderer.
	MTL::Texture* pTexture;
	std::chrono::duration<double, std::milli> wallClockTime;
	double   gpuTimeMs; // CommandBuffer::GPUEndTime() - GPUStartTime(); the headline metric
	uint32_t triangleCount; // reported instead of rays/sec — rasterization has no "rays"
};

// A standard graphics-pipeline (vertex/fragment + rasterizer + depth buffer) renderer for the same
// SceneDescription the CPU and GPU ray tracers consume — see CLAUDE.md's "Project status" for the
// full rationale. Unlike GPURenderer (a compute kernel), this drives a real MTL::RenderPipelineState
// over Shaders/Raster.metal, compiled from source at runtime exactly like ImageDisplayView's
// Blit.metal (it has no ShaderTypes.h/RayTraceCore.h #includes, so it doesn't need Raytracer.metal's
// xcrun/.metallib build step). Device/queue/pipeline/depth-stencil-state are created once in the
// constructor and cached, matching GPURenderer's warm-up-then-timed-dispatch methodology.
class RasterRenderer
{
	public:
		// Compiles Raster.metal, builds the render pipeline + depth-stencil state, and creates the
		// command queue. Aborts on failure (see .cpp), matching GPURenderer's contract.
		explicit RasterRenderer( MTL::Device* pDevice );
		// Releases the color/depth textures, geometry buffers, pipeline/depth-stencil state, queue,
		// and device.
		~RasterRenderer();

		// Rebuilds this scene's geometry buffers/render targets, does one untimed warm-up draw, then
		// one timed draw; returns the resulting texture plus wall-clock/GPU-only timings and the
		// triangle count actually drawn.
		RasterRenderResult render( const SceneDescription& scene );

	private:
		// Flattens every entity's buildEntityMesh() output (already world-space — see
		// Export/EntityMesh.hpp) plus a flat per-entity material color into one combined
		// vertex/index buffer pair, and uploads them. Rebuilt on every render() call, like
		// GPURenderer::rebuildBuffers — a few Shared-mode buffer allocations per on-demand render
		// click is negligible next to the actual render cost.
		void   rebuildGeometryBuffers( const SceneDescription& scene );
		// (Re)creates the color + depth textures only when width/height actually change.
		void   rebuildTargetsIfNeeded( uint32_t width, uint32_t height );
		double drawOnce( const SceneDescription& scene ); // one draw+wait; returns GPU-only ms

		MTL::Device*              _pDevice;
		MTL::CommandQueue*        _pCommandQueue;
		MTL::RenderPipelineState* _pPipelineState;
		MTL::DepthStencilState*   _pDepthStencilState;

		MTL::Buffer*  _pVertexBuffer;
		MTL::Buffer*  _pIndexBuffer;
		uint32_t      _indexCount;

		MTL::Texture* _pColorTexture;
		MTL::Texture* _pDepthTexture;
		uint32_t      _textureWidth;
		uint32_t      _textureHeight;
};
