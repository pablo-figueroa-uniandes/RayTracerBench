#pragma once

#include "../Core/Scene.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace CPUThreading
{
	enum Mode
	{
		SingleThreaded,
		MultiThreaded,
	};
}

struct CPURenderResult
{
	std::vector<uint8_t>                       pixels; // RGBA8, row-major, row 0 = top
	std::chrono::duration<double, std::milli>  renderTime;
};

// Renders `scene` on the CPU by calling the exact same Core/RayTraceCore.h functions the GPU
// renderer will later call from its kernel. Default is MultiThreaded (realistic CPU perf);
// SingleThreaded is exposed explicitly so a UI-facing speedup number is never ambiguous about
// which CPU baseline it was measured against.
CPURenderResult renderCPU( const SceneDescription& scene, CPUThreading::Mode mode = CPUThreading::MultiThreaded );
