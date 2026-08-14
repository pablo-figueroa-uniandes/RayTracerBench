#pragma once

#include <AppKit/AppKit.hpp>

class AppDelegate : public NS::ApplicationDelegate
{
	public:
		~AppDelegate();

		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;

	private:
		NS::Window* _pWindow = nullptr;
		NS::Button* _pButton = nullptr;
};
