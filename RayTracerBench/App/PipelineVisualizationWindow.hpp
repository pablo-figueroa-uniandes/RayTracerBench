#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>

#include "../Core/Scene.hpp"
#include "../GPU/PipelineStageRenderer.hpp"
#include "../GPU/RasterRenderer.hpp"
#include "ImageDisplayView.hpp"

#include <functional>

// An optional secondary window visualizing the four traditional-pipeline stages a scene passes
// through on the way to a rasterized image — see CLAUDE.md's pipeline-visualization design note for
// the full rationale. "Projective Matrix" / "Camera Matrix" / "Orthographic Matrix" are rendered by
// a dedicated PipelineStageRenderer (wireframe diagrams — see its header); "Viewport Matrix" is
// exactly the existing RasterRenderer's own output, re-rendered here through this window's own
// RasterRenderer instance rather than the main window's, so a Refresh here can never race a Render
// Raster/Compare click on the main window over shared GPU resources.
class PipelineVisualizationWindow
{
	public:
		explicit PipelineVisualizationWindow( MTL::Device* pDevice );
		~PipelineVisualizationWindow();

		// Recomputes all four stages for `scene` on a background thread, updates the four panels,
		// then brings the window to the front. Safe to call repeatedly (e.g. every time the main
		// window's "Pipeline Steps" button is clicked).
		void showWithScene( const SceneDescription& scene );

		// Set by the owner (AppDelegate): called when this window's own "Refresh" button is
		// clicked, so the owner can rebuild a scene from the main window's *current* settings and
		// call showWithScene() again — this window has no settings of its own.
		std::function<void()> onRefreshRequested;

		// Forwards to the "Refresh" button click callback, if the owner set one. Public for the
		// same reason every other button's click trampoline in this app is (metal-cpp-extensions'
		// action callbacks are capture-less function pointers).
		void handleRefreshClicked();

	private:
		void setControlsEnabled( bool enabled );

		NS::Window*    _pWindow;
		NS::Button*    _pRefreshButton;
		NS::TextField* _pLabels[ 4 ];
		ImageDisplayView* _pImageViews[ 4 ];

		MTL::Device*             _pDevice;
		PipelineStageRenderer*   _pStageRenderer;
		RasterRenderer*          _pViewportStageRenderer;
};
