#pragma once

#include <AppKit/AppKit.hpp>

#include <string>

// Three label lines (CPU / GPU / speedup) per CLAUDE.md's UI architecture. Deliberately dumb: it
// just displays whatever text it's given — all render-time/rays-per-sec/speedup formatting stays
// in AppDelegate, where the actual timing values are known.
class ResultsPanel
{
	public:
		explicit ResultsPanel( CGRect frame );
		~ResultsPanel();

		NS::View* view() const { return _pContainerView; }

		void setCPULine( const std::string& text );
		void setGPULine( const std::string& text );
		void setSpeedupLine( const std::string& text );

	private:
		static NS::TextField* makeLabel( CGRect frame, const char* text );

		NS::View*      _pContainerView;
		NS::TextField* _pCPULabel;
		NS::TextField* _pGPULabel;
		NS::TextField* _pSpeedupLabel;
};
