#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>

#include "../Core/Scene.hpp"
#include "../GPU/GPURenderer.hpp"
#include "../GPU/RasterRenderer.hpp"
#include "ControlsPanel.hpp"
#include "ImageDisplayView.hpp"
#include "PipelineVisualizationWindow.hpp"
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
		// Builds a scene from `settings` and renders it through the standard (rasterized) graphics
		// pipeline.
		void startRasterRender( const RenderSettings& settings );
		// Builds one scene from `settings` and renders it with all three renderers.
		void startCompare( const RenderSettings& settings );

		// Recomputes and displays the speedup line covering every pair of CPU/GPU/Raster times
		// currently known (needs at least two to produce anything).
		void updateSpeedupIfPossible();

		// Shows (creating it the first time) the "Pipeline Steps" secondary window, populated from
		// a scene built at the main window's current settings.
		void showPipelineWindow();

		// Builds a scene from the current controls settings and writes it to <executable
		// directory>/SavedScenes/ as either glTF (asGLTF=true) or OBJ+MTL (asGLTF=false), plus a
		// same-named PNG preview rendered at the current settings, then shows an alert with the
		// result. Runs on a background thread, like the render actions above — a full CPU
		// render for the preview image can take a while, unlike the geometry export alone.
		void saveScene( bool asGLTF );

		// Shows an NS::OpenPanel (blocks the main thread only for the duration of that modal
		// picker -- the same kind of brief, intentional block the save-result NS::Alert already
		// does elsewhere in this app), then -- if the user picked a file -- hands its path to a
		// background thread that reconstructs the scene via SceneImporter.hpp and shows a result
		// alert. On success, the reconstructed entities/materials become `_loadedScene`, and every
		// subsequent Render CPU/GPU/Compare click renders it (with camera/params refreshed from
		// whatever the controls currently say) instead of building a fresh buildDefaultScene() --
		// until another Load Scene (or an app relaunch) replaces it.
		void loadScene();

		// The scene each render action should actually render: `_loadedScene`'s entities/materials
		// with camera/params refreshed from `settings` if a scene has been loaded via loadScene(),
		// or a freshly built buildDefaultScene() otherwise.
		SceneDescription sceneForRender( const RenderSettings& settings ) const;

		// Builds the app's minimal menu bar (About / Quit).
		NS::Menu* createMenuBar();

		// Magnifying-glass loupe: a local NS::EventMaskMouseMoved monitor (installed once, at
		// launch) reports the same normalized UV position to all three ImageDisplayViews whenever
		// the mouse is over any preview, so hovering one image zooms the matching detail in all —
		// the actual point of the feature being to compare renderer output at high zoom.
		void handleMouseMoved( NS::Event* pEvent );

		NS::Window*       _pWindow = nullptr;
		MTL::Device*      _pDevice = nullptr;
		ControlsPanel*    _pControlsPanel = nullptr;
		ImageDisplayView* _pCPUImageView = nullptr;
		ImageDisplayView* _pGPUImageView = nullptr;
		ImageDisplayView* _pRasterImageView = nullptr;
		ResultsPanel*     _pResultsPanel = nullptr;
		GPURenderer*      _pGPURenderer = nullptr;
		RasterRenderer*   _pRasterRenderer = nullptr;

		// Lazily created on the first "Pipeline Steps" click; kept alive (not destroyed on close —
		// see NSWindow.hpp's setReleasedWhenClosed() comment) for the rest of the app's lifetime.
		PipelineVisualizationWindow* _pPipelineWindow = nullptr;

		double _lastCPUTimeMs = -1.0;
		double _lastGPUTimeMs = -1.0;    // GPU-only time — the headline metric per CLAUDE.md
		double _lastRasterTimeMs = -1.0; // GPU-only time, same convention as _lastGPUTimeMs

		// Set by a successful loadScene(); consulted by sceneForRender().
		bool             _hasLoadedScene = false;
		SceneDescription _loadedScene;
};
