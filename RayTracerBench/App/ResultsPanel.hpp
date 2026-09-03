#pragma once

#include <AppKit/AppKit.hpp>

#include <string>

// Four label lines (CPU / GPU / Raster / speedup) per CLAUDE.md's UI architecture. Deliberately
// dumb: it just displays whatever text it's given — all render-time/rays-per-sec/triangle-count/
// speedup formatting stays in AppDelegate, where the actual timing values are known.
class ResultsPanel
{
	public:
		// Builds the four label subviews inside `frame`.
		explicit ResultsPanel( CGRect frame );
		// Releases the four label subviews and the container view.
		~ResultsPanel();

		// The container view an owner should add as a subview.
		NS::View* view() const { return _pContainerView; }

		// Replaces the CPU results line's text.
		void setCPULine( const std::string& text );
		// Replaces the GPU results line's text.
		void setGPULine( const std::string& text );
		// Replaces the raster results line's text.
		void setRasterLine( const std::string& text );
		// Replaces the speedup-ratio line's text.
		void setSpeedupLine( const std::string& text );

	private:
		// Creates a non-editable, non-bezeled, transparent-background NS::TextField, used as a
		// plain label rather than an input field.
		static NS::TextField* makeLabel( CGRect frame, const char* text );

		NS::View*      _pContainerView;
		NS::TextField* _pCPULabel;
		NS::TextField* _pGPULabel;
		NS::TextField* _pRasterLabel;
		NS::TextField* _pSpeedupLabel;
};
