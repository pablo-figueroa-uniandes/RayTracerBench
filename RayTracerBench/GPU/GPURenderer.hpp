#pragma once

#include "../Core/Scene.hpp"

#include <Metal/Metal.hpp>

#include <chrono>
#include <cstdint>

struct GPURenderResult
{
	// Not owned by the caller; valid until the next render() call on this GPURenderer.
	MTL::Texture* pTexture;
	std::chrono::duration<double, std::milli> wallClockTime;
	double gpuTimeMs; // CommandBuffer::GPUEndTime() - GPUStartTime(); the headline metric
};

// Written directly against metal-cpp (no MetalKit). Device/queue/pipeline are created once in the
// constructor and cached so shader-compile latency never pollutes a render's timing. Every
// render() call does its own untimed warm-up dispatch before the timed one, so a single call's
// gpuTimeMs is never inflated by cold-start effects.
class GPURenderer
{
	public:
		explicit GPURenderer( MTL::Device* pDevice );
		~GPURenderer();

		GPURenderResult render( const SceneDescription& scene );

	private:
		void   rebuildBuffers( const SceneDescription& scene );
		void   rebuildTextureIfNeeded( uint32_t width, uint32_t height );
		double dispatchOnce( const SceneDescription& scene ); // one dispatch+wait; returns GPU-only ms

		MTL::Device*               _pDevice;
		MTL::CommandQueue*         _pCommandQueue;
		MTL::ComputePipelineState* _pPipelineState;

		MTL::Buffer*  _pSphereBuffer;
		MTL::Buffer*  _pMaterialBuffer;
		MTL::Texture* _pOutputTexture;
		uint32_t      _textureWidth;
		uint32_t      _textureHeight;
};
