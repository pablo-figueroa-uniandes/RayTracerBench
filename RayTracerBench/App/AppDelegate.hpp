#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>

#include "ImageDisplayView.hpp"

class AppDelegate : public NS::ApplicationDelegate
{
	public:
		~AppDelegate();

		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;

		// Public so the capture-less button callback (which can't hold a `this` closure — see
		// AppDelegate.cpp) can reach it through a file-local pointer, same pattern as the
		// Milestone 1 spike's click handler.
		void renderButtonClicked();

	private:
		NS::Window*       _pWindow = nullptr;
		NS::Button*       _pButton = nullptr;
		MTL::Device*      _pDevice = nullptr;
		ImageDisplayView* _pImageView = nullptr;
};
