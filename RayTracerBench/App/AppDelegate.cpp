#include "AppDelegate.hpp"

#include "../CPU/CPURenderer.hpp"
#include "../Core/Scene.hpp"

#include <cstdio>

namespace
{
	// registerActionCallback's IMP must decay to a plain function pointer, so the click handler
	// can't capture `this` by closure — it reaches the delegate through a file-local pointer set
	// once at launch, same pattern as the Milestone 1 spike.
	AppDelegate* gAppDelegate = nullptr;

	void onRenderButtonClicked( void*, SEL, const NS::Object* )
	{
		gAppDelegate->renderButtonClicked();
	}
}

AppDelegate::~AppDelegate()
{
	delete _pImageView;
	_pButton->release();
	_pWindow->release();
	_pDevice->release();
}

void AppDelegate::applicationDidFinishLaunching( NS::Notification* pNotification )
{
	using NS::StringEncoding::UTF8StringEncoding;

	gAppDelegate = this;

	_pDevice = MTL::CreateSystemDefaultDevice();

	const CGRect windowFrame = ( CGRect ){ { 100.0, 100.0 }, { 420.0, 300.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View* pContentView = NS::View::alloc()->init( contentFrame );

	const CGRect imageFrame = ( CGRect ){ { 10.0, 70.0 }, { 400.0, 225.0 } };
	_pImageView = new ImageDisplayView( _pDevice, imageFrame );
	pContentView->addSubview( _pImageView->view() );

	const CGRect buttonFrame = ( CGRect ){ { 10.0, 15.0 }, { 180.0, 40.0 } };
	_pButton = NS::Button::alloc()->init( buttonFrame );
	_pButton->setTitle( NS::String::string( "Render CPU", UTF8StringEncoding ) );

	SEL clickSel = NS::MenuItem::registerActionCallback( "renderButtonClicked", onRenderButtonClicked );
	_pButton->setTarget( _pButton );
	_pButton->setAction( clickSel );

	pContentView->addSubview( _pButton );

	_pWindow->setContentView( pContentView );
	_pWindow->makeKeyAndOrderFront( nullptr );

	NS::Application* pApp = reinterpret_cast< NS::Application* >( pNotification->object() );
	pApp->activateIgnoringOtherApps( true );
}

void AppDelegate::renderButtonClicked()
{
	SceneDescription scene = buildDefaultScene( 1234u, 400, 400.0f / 225.0f, 20, 20 );
	CPURenderResult  result = renderCPU( scene, CPUThreading::MultiThreaded );

	_pImageView->updatePixels( result.pixels.data(), scene.params.width, scene.params.height );

	char titleBuf[ 128 ];
	std::snprintf( titleBuf, sizeof( titleBuf ), "RayTracerBench — CPU render: %.1f ms", result.renderTime.count() );
	_pWindow->setTitle( NS::String::string( titleBuf, NS::StringEncoding::UTF8StringEncoding ) );

	std::printf( "CPU render: %.1f ms\n", result.renderTime.count() );
	std::fflush( stdout );
}

bool AppDelegate::applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender )
{
	return true;
}
