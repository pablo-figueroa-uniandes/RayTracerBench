#pragma once

#include <AppKit/AppKit.hpp>

#include "../CPU/CPURenderer.hpp"

#include <cstdint>
#include <functional>

struct RenderSettings
{
	uint32_t           width;
	uint32_t           samplesPerPixel;
	uint32_t           maxDepth;
	unsigned           seed;
	CPUThreading::Mode cpuMode;
	bool               floating; // "Floating?" checkbox — see Scene.hpp's buildDefaultScene()
};

// image-width, samples-per-pixel, max-depth, and scene-seed fields (+ randomize), a CPU-threading
// toggle, and Render CPU / Render GPU / Compare buttons — per CLAUDE.md's UI architecture. Actual
// rendering stays outside this class: it only parses settings and forwards clicks to whichever
// callbacks the owner (AppDelegate) sets.
class ControlsPanel
{
	public:
		explicit ControlsPanel( CGRect frame );
		~ControlsPanel();

		NS::View* view() const { return _pContainerView; }

		RenderSettings currentSettings() const;

		// Disables every field/button (prevents a second click from racing an in-flight
		// background render against the same shared state) and re-enables them afterward.
		void setControlsEnabled( bool enabled );

		std::function<void()> onRenderCPU;
		std::function<void()> onRenderGPU;
		std::function<void()> onCompare;

		// Public so the capture-less click-callback trampolines in ControlsPanel.cpp (which can't
		// hold a `this` closure) can reach them through a file-local pointer — same pattern as
		// AppDelegate's click handlers.
		void handleRandomizeSeedClicked();
		void handleThreadingToggleClicked();
		void handleRenderCPUClicked();
		void handleRenderGPUClicked();
		void handleCompareClicked();

	private:
		static NS::TextField* makeLabel( CGRect frame, const char* text );
		static NS::TextField* makeField( CGRect frame, const char* initialText );

		NS::View* _pContainerView;

		NS::TextField* _pWidthField;
		NS::TextField* _pSppField;
		NS::TextField* _pMaxDepthField;
		NS::TextField* _pSeedField;

		NS::Button* _pThreadingToggleButton;
		bool        _multiThreaded;

		// A real NSButtonTypeSwitch checkbox rather than the threading toggle's "push button whose
		// title changes" hack — AppKit manages its own checked/unchecked state on click, so this
		// needs no click handler at all; currentSettings() just reads state() on demand.
		NS::Button* _pFloatingCheckbox;

		NS::Button* _pRandomizeSeedButton;
		NS::Button* _pRenderCPUButton;
		NS::Button* _pRenderGPUButton;
		NS::Button* _pCompareButton;
};
