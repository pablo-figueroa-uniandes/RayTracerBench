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
		// Releases the renderer, panels, views, window, and device.
		~AppDelegate();

		// Builds the menu bar, window, and all subviews, wires up button callbacks, and installs
		// the mouse-move monitor for the magnifier.
		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		// Always true: this app has exactly one window, so closing it should quit.
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;

	private:
		// Each spawns a background std::thread that renders, then marshals the UI update back to
		// the main thread via dispatch_async(dispatch_get_main_queue(), ...) — GCD's plain C API,
		// no Objective-C needed. Controls are disabled for the duration to prevent a second click
		// from racing an in-flight render against the same shared GPURenderer/ImageDisplayView state.
		// Builds a scene from `settings` and renders it on the CPU.
		void startCPURender( const RenderSettings& settings );
		// Builds a scene from `settings` and renders it on the GPU.
		void startGPURender( const RenderSettings& settings );
		// Builds one scene from `settings` and renders it with both the CPU and GPU renderers.
		void startCompare( const RenderSettings& settings );

		// Recomputes and displays the CPU/GPU speedup line once both a CPU and a GPU time are known.
		void updateSpeedupIfPossible();

		// Builds a scene from the current controls settings and writes it to <executable
		// directory>/SavedScenes/ as either glTF (asGLTF=true) or OBJ+MTL (asGLTF=false), then
		// shows an alert with the result. Synchronous on the main thread — unlike the renders
		// above, writing scene geometry to disk is fast enough that a background thread isn't
		// warranted.
		void saveScene( bool asGLTF );

		// Builds the app's minimal menu bar (About / Quit).
		NS::Menu* createMenuBar();

		// Magnifying-glass loupe: a local NS::EventMaskMouseMoved monitor (installed once, at
		// launch) reports the same normalized UV position to both ImageDisplayViews whenever the
		// mouse is over either preview, so hovering one image zooms the matching detail in both —
		// the actual point of the feature being to compare CPU vs. GPU output at high zoom.
		void handleMouseMoved( NS::Event* pEvent );

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
