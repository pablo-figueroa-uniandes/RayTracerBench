#include "AppDelegate.hpp"

#include <cstdio>

namespace
{
	// registerActionCallback's IMP must decay to a plain function pointer, so the
	// click handler can't capture the window by closure — it reaches it through a
	// file-local pointer set once at launch instead.
	NS::Window* gWindow = nullptr;
	int gClickCount = 0;

	void onButtonClicked( void*, SEL, const NS::Object* )
	{
		++gClickCount;
		std::printf( "Button clicked! (%d)\n", gClickCount );
		std::fflush( stdout );

		char titleBuf[ 64 ];
		std::snprintf( titleBuf, sizeof( titleBuf ), "RayTracerBench spike — clicked %d", gClickCount );
		gWindow->setTitle( NS::String::string( titleBuf, NS::StringEncoding::UTF8StringEncoding ) );
	}
}

AppDelegate::~AppDelegate()
{
	_pButton->release();
	_pWindow->release();
}

void AppDelegate::applicationDidFinishLaunching( NS::Notification* pNotification )
{
	using NS::StringEncoding::UTF8StringEncoding;

	const CGRect frame = ( CGRect ){ { 100.0, 100.0 }, { 360.0, 200.0 } };

	_pWindow = NS::Window::alloc()->init(
		frame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench spike", UTF8StringEncoding ) );
	gWindow = _pWindow;

	const CGRect buttonFrame = ( CGRect ){ { 90.0, 80.0 }, { 180.0, 40.0 } };
	_pButton = NS::Button::alloc()->init( buttonFrame );
	_pButton->setTitle( NS::String::string( "Click Me", UTF8StringEncoding ) );

	SEL clickSel = NS::MenuItem::registerActionCallback( "spikeButtonClicked", onButtonClicked );
	_pButton->setTarget( _pButton );
	_pButton->setAction( clickSel );

	_pWindow->setContentView( _pButton );
	_pWindow->makeKeyAndOrderFront( nullptr );

	NS::Application* pApp = reinterpret_cast< NS::Application* >( pNotification->object() );
	pApp->activateIgnoringOtherApps( true );
}

bool AppDelegate::applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender )
{
	return true;
}
