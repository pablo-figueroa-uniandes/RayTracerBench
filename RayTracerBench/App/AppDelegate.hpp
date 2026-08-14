#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>

#include "../GPU/GPURenderer.hpp"
#include "ControlsPanel.hpp"
#include "ImageDisplayView.hpp"
#include "ResultsPanel.hpp"

class AppDelegate : public NS::ApplicationDelegate
{
	public:
		~AppDelegate();

		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;

	private:
		// Each spawns a background std::thread that renders, then marshals the UI update back to
		// the main thread via dispatch_async(dispatch_get_main_queue(), ...) — GCD's plain C API,
		// no Objective-C needed. Controls are disabled for the duration to prevent a second click
		// from racing an in-flight render against the same shared GPURenderer/ImageDisplayView state.
		void startCPURender( const RenderSettings& settings );
		void startGPURender( const RenderSettings& settings );
		void startCompare( const RenderSettings& settings );

		void updateSpeedupIfPossible();

		NS::Menu* createMenuBar();

		NS::Window*       _pWindow = nullptr;
		MTL::Device*      _pDevice = nullptr;
		ControlsPanel*    _pControlsPanel = nullptr;
		ImageDisplayView* _pCPUImageView = nullptr;
		ImageDisplayView* _pGPUImageView = nullptr;
		ResultsPanel*     _pResultsPanel = nullptr;
		GPURenderer*      _pGPURenderer = nullptr;

		double _lastCPUTimeMs = -1.0;
		double _lastGPUTimeMs = -1.0; // GPU-only time — the headline metric per CLAUDE.md
};
