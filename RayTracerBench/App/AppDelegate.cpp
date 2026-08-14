#include "AppDelegate.hpp"

#include "../CPU/CPURenderer.hpp"
#include "../Core/Scene.hpp"

#include <cstdio>

namespace
{
	// registerActionCallback's IMP must decay to a plain function pointer, so the click handlers
	// can't capture `this` by closure — they reach the delegate through a file-local pointer set
	// once at launch, same pattern as the Milestone 1 spike.
	AppDelegate* gAppDelegate = nullptr;

	void onCPURenderButtonClicked( void*, SEL, const NS::Object* )
	{
		gAppDelegate->renderCPUButtonClicked();
	}

	void onGPURenderButtonClicked( void*, SEL, const NS::Object* )
	{
		gAppDelegate->renderGPUButtonClicked();
	}
}

AppDelegate::~AppDelegate()
{
	delete _pGPURenderer;
	delete _pGPUImageView;
	delete _pCPUImageView;
	_pGPUButton->release();
	_pCPUButton->release();
	_pWindow->release();
	_pDevice->release();
}

void AppDelegate::applicationDidFinishLaunching( NS::Notification* pNotification )
{
	using NS::StringEncoding::UTF8StringEncoding;

	gAppDelegate = this;

	_pDevice = MTL::CreateSystemDefaultDevice();
	_pGPURenderer = new GPURenderer( _pDevice );

	const CGRect windowFrame = ( CGRect ){ { 100.0, 100.0 }, { 860.0, 310.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View* pContentView = NS::View::alloc()->init( contentFrame );

	// Left column: CPU preview + button. Right column: GPU preview + button.
	const CGRect cpuImageFrame = ( CGRect ){ { 10.0, 70.0 }, { 400.0, 225.0 } };
	const CGRect gpuImageFrame = ( CGRect ){ { 430.0, 70.0 }, { 400.0, 225.0 } };
	const CGRect cpuButtonFrame = ( CGRect ){ { 10.0, 15.0 }, { 180.0, 40.0 } };
	const CGRect gpuButtonFrame = ( CGRect ){ { 430.0, 15.0 }, { 180.0, 40.0 } };

	_pCPUImageView = new ImageDisplayView( _pDevice, cpuImageFrame );
	pContentView->addSubview( _pCPUImageView->view() );

	_pGPUImageView = new ImageDisplayView( _pDevice, gpuImageFrame );
	pContentView->addSubview( _pGPUImageView->view() );

	_pCPUButton = NS::Button::alloc()->init( cpuButtonFrame );
	_pCPUButton->setTitle( NS::String::string( "Render CPU", UTF8StringEncoding ) );
	SEL cpuClickSel = NS::MenuItem::registerActionCallback( "renderCPUButtonClicked", onCPURenderButtonClicked );
	_pCPUButton->setTarget( _pCPUButton );
	_pCPUButton->setAction( cpuClickSel );
	pContentView->addSubview( _pCPUButton );

	_pGPUButton = NS::Button::alloc()->init( gpuButtonFrame );
	_pGPUButton->setTitle( NS::String::string( "Render GPU", UTF8StringEncoding ) );
	SEL gpuClickSel = NS::MenuItem::registerActionCallback( "renderGPUButtonClicked", onGPURenderButtonClicked );
	_pGPUButton->setTarget( _pGPUButton );
	_pGPUButton->setAction( gpuClickSel );
	pContentView->addSubview( _pGPUButton );

	_pWindow->setContentView( pContentView );
	_pWindow->makeKeyAndOrderFront( nullptr );

	NS::Application* pApp = reinterpret_cast< NS::Application* >( pNotification->object() );
	pApp->activateIgnoringOtherApps( true );
}

void AppDelegate::renderCPUButtonClicked()
{
	SceneDescription scene = buildDefaultScene( 1234u, 400, 400.0f / 225.0f, 20, 20 );
	CPURenderResult  result = renderCPU( scene, CPUThreading::MultiThreaded );

	_pCPUImageView->updatePixels( result.pixels.data(), scene.params.width, scene.params.height );

	std::printf( "CPU render: %.1f ms\n", result.renderTime.count() );
	std::fflush( stdout );
}

void AppDelegate::renderGPUButtonClicked()
{
	SceneDescription scene = buildDefaultScene( 1234u, 400, 400.0f / 225.0f, 20, 20 );
	GPURenderResult  result = _pGPURenderer->render( scene );

	_pGPUImageView->displayTexture( result.pTexture );

	std::printf( "GPU render: %.1f ms wall-clock, %.3f ms GPU-only\n", result.wallClockTime.count(), result.gpuTimeMs );
	std::fflush( stdout );
}

bool AppDelegate::applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender )
{
	return true;
}
