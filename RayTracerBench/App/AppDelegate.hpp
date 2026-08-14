#pragma once

#include <AppKit/AppKit.hpp>
#include <Metal/Metal.hpp>

#include "../GPU/GPURenderer.hpp"
#include "ImageDisplayView.hpp"

class AppDelegate : public NS::ApplicationDelegate
{
	public:
		~AppDelegate();

		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;

		// Public so the capture-less button callbacks (which can't hold a `this` closure — see
		// AppDelegate.cpp) can reach them through file-local pointers, same pattern as the
		// Milestone 1 spike's click handler.
		void renderCPUButtonClicked();
		void renderGPUButtonClicked();

	private:
		NS::Window*       _pWindow = nullptr;
		NS::Button*       _pCPUButton = nullptr;
		NS::Button*       _pGPUButton = nullptr;
		MTL::Device*      _pDevice = nullptr;
		ImageDisplayView* _pCPUImageView = nullptr;
		ImageDisplayView* _pGPUImageView = nullptr;
		GPURenderer*      _pGPURenderer = nullptr;
};
