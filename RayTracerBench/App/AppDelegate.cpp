#include "AppDelegate.hpp"

#include "../CPU/CPURenderer.hpp"
#include "../Core/Scene.hpp"
#include "AboutAlert.hpp"

#include <cstdio>
#include <dispatch/dispatch.h>
#include <thread>

namespace
{
	// image-width is the only user-facing size control (per CLAUDE.md); height is always derived
	// from this fixed aspect ratio, matching Scene::buildDefaultScene's own design.
	constexpr float kAspectRatio = 16.0f / 9.0f;

	double raysPerSecond( const RenderParams& params, double milliseconds )
	{
		double rays = (double)params.width * (double)params.height * (double)params.samplesPerPixel;
		return rays / ( milliseconds / 1000.0 );
	}

	void onShowAboutClicked( void*, SEL, const NS::Object* )
	{
		showAboutAlert();
	}

	void onQuitClicked( void*, SEL, const NS::Object* pSender )
	{
		NS::Application::sharedApplication()->terminate( pSender );
	}
}

AppDelegate::~AppDelegate()
{
	delete _pGPURenderer;
	delete _pResultsPanel;
	delete _pGPUImageView;
	delete _pCPUImageView;
	delete _pControlsPanel;
	_pWindow->release();
	_pDevice->release();
}

NS::Menu* AppDelegate::createMenuBar()
{
	using NS::StringEncoding::UTF8StringEncoding;

	NS::Menu*     pMainMenu = NS::Menu::alloc()->init();
	NS::MenuItem* pAppMenuItem = NS::MenuItem::alloc()->init();
	NS::Menu*     pAppMenu = NS::Menu::alloc()->init( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );

	SEL           aboutSel = NS::MenuItem::registerActionCallback( "appShowAbout", onShowAboutClicked );
	NS::MenuItem* pAboutItem = pAppMenu->addItem( NS::String::string( "About RayTracerBench", UTF8StringEncoding ), aboutSel, NS::String::string( "", UTF8StringEncoding ) );
	(void)pAboutItem;

	NS::String*   appName = NS::RunningApplication::currentApplication()->localizedName();
	NS::String*   quitTitle = NS::String::string( "Quit ", UTF8StringEncoding )->stringByAppendingString( appName );
	SEL           quitSel = NS::MenuItem::registerActionCallback( "appQuit", onQuitClicked );
	NS::MenuItem* pQuitItem = pAppMenu->addItem( quitTitle, quitSel, NS::String::string( "q", UTF8StringEncoding ) );
	pQuitItem->setKeyEquivalentModifierMask( NS::EventModifierFlagCommand );

	pAppMenuItem->setSubmenu( pAppMenu );
	pMainMenu->addItem( pAppMenuItem );

	pAppMenuItem->release();
	pAppMenu->release();

	return pMainMenu->autorelease();
}

void AppDelegate::applicationDidFinishLaunching( NS::Notification* pNotification )
{
	using NS::StringEncoding::UTF8StringEncoding;

	NS::Application* pApp = reinterpret_cast< NS::Application* >( pNotification->object() );
	pApp->setMainMenu( createMenuBar() );

	_pDevice = MTL::CreateSystemDefaultDevice();
	_pGPURenderer = new GPURenderer( _pDevice );

	const CGRect windowFrame = ( CGRect ){ { 100.0, 100.0 }, { 860.0, 460.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );
	_pWindow->setAcceptsMouseMovedEvents( true );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View* pContentView = NS::View::alloc()->init( contentFrame );

	// Top to bottom: controls, two side-by-side previews, results.
	_pControlsPanel = new ControlsPanel( ( CGRect ){ { 10.0, 390.0 }, { 840.0, 60.0 } } );
	pContentView->addSubview( _pControlsPanel->view() );

	const CGRect cpuImageFrame = ( CGRect ){ { 10.0, 155.0 }, { 400.0, 225.0 } };
	const CGRect gpuImageFrame = ( CGRect ){ { 430.0, 155.0 }, { 400.0, 225.0 } };
	_pCPUImageView = new ImageDisplayView( _pDevice, cpuImageFrame );
	pContentView->addSubview( _pCPUImageView->view() );
	_pGPUImageView = new ImageDisplayView( _pDevice, gpuImageFrame );
	pContentView->addSubview( _pGPUImageView->view() );

	_pResultsPanel = new ResultsPanel( ( CGRect ){ { 10.0, 15.0 }, { 840.0, 70.0 } } );
	pContentView->addSubview( _pResultsPanel->view() );

	_pControlsPanel->onRenderCPU = [ this ]() { startCPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onRenderGPU = [ this ]() { startGPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onCompare = [ this ]() { startCompare( _pControlsPanel->currentSettings() ); };

	_pWindow->setContentView( pContentView );
	_pWindow->makeKeyAndOrderFront( nullptr );

	pApp->activateIgnoringOtherApps( true );

	NS::Event::addLocalMonitorForEventsMatchingMask( NS::EventMaskMouseMoved, ^NS::Event*( NS::Event* pEvent ) {
		handleMouseMoved( pEvent );
		return pEvent;
	} );
}

void AppDelegate::handleMouseMoved( NS::Event* pEvent )
{
	if ( pEvent->window() != _pWindow )
		return;

	const CGPoint windowPoint = pEvent->locationInWindow();

	const CGPoint cpuLocal = _pCPUImageView->view()->convertPoint( windowPoint, nullptr );
	const CGSize  cpuSize = _pCPUImageView->size();
	const bool    overCPU = cpuLocal.x >= 0.0 && cpuLocal.x <= cpuSize.width && cpuLocal.y >= 0.0 && cpuLocal.y <= cpuSize.height;

	const CGPoint gpuLocal = _pGPUImageView->view()->convertPoint( windowPoint, nullptr );
	const CGSize  gpuSize = _pGPUImageView->size();
	const bool    overGPU = gpuLocal.x >= 0.0 && gpuLocal.x <= gpuSize.width && gpuLocal.y >= 0.0 && gpuLocal.y <= gpuSize.height;

	if ( !overCPU && !overGPU )
	{
		_pCPUImageView->setMagnifier( false, 0.5f, 0.5f );
		_pGPUImageView->setMagnifier( false, 0.5f, 0.5f );
		return;
	}

	const CGPoint local = overCPU ? cpuLocal : gpuLocal;
	const CGSize  size = overCPU ? cpuSize : gpuSize;

	// AppKit view coordinates are Y-up (0 at the bottom); the source texture's V convention is
	// Y-down (V=0 at the top row — see Blit.metal / CPURenderer.hpp), hence the flip.
	const float u = (float)( local.x / size.width );
	const float v = 1.0f - (float)( local.y / size.height );

	_pCPUImageView->setMagnifier( true, u, v );
	_pGPUImageView->setMagnifier( true, u, v );
}

void AppDelegate::startCPURender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth );
		CPURenderResult  result = renderCPU( scene, settings.cpuMode );
		double           rps = raysPerSecond( scene.params, result.renderTime.count() );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pCPUImageView->updatePixels( result.pixels.data(), scene.params.width, scene.params.height );

			char buf[ 160 ];
			std::snprintf( buf, sizeof( buf ), "CPU (%s): %.1f ms | %.2fM rays/s",
				settings.cpuMode == CPUThreading::MultiThreaded ? "multi" : "single",
				result.renderTime.count(), rps / 1.0e6 );
			_pResultsPanel->setCPULine( buf );

			_lastCPUTimeMs = result.renderTime.count();
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "CPU render: %.1f ms\n", result.renderTime.count() );
			std::fflush( stdout );
		} );
	} ).detach();
}

void AppDelegate::startGPURender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth );
		GPURenderResult  result = _pGPURenderer->render( scene );
		double           rps = raysPerSecond( scene.params, result.gpuTimeMs );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pGPUImageView->displayTexture( result.pTexture );

			char buf[ 160 ];
			std::snprintf( buf, sizeof( buf ), "GPU: %.1f ms wall (%.3f ms GPU) | %.2fM rays/s",
				result.wallClockTime.count(), result.gpuTimeMs, rps / 1.0e6 );
			_pResultsPanel->setGPULine( buf );

			_lastGPUTimeMs = result.gpuTimeMs;
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "GPU render: %.1f ms wall-clock, %.3f ms GPU-only\n", result.wallClockTime.count(), result.gpuTimeMs );
			std::fflush( stdout );
		} );
	} ).detach();
}

void AppDelegate::startCompare( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth );

		CPURenderResult cpuResult = renderCPU( scene, settings.cpuMode );
		GPURenderResult gpuResult = _pGPURenderer->render( scene );

		double cpuRps = raysPerSecond( scene.params, cpuResult.renderTime.count() );
		double gpuRps = raysPerSecond( scene.params, gpuResult.gpuTimeMs );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pCPUImageView->updatePixels( cpuResult.pixels.data(), scene.params.width, scene.params.height );
			_pGPUImageView->displayTexture( gpuResult.pTexture );

			char cpuBuf[ 160 ];
			char gpuBuf[ 160 ];
			std::snprintf( cpuBuf, sizeof( cpuBuf ), "CPU (%s): %.1f ms | %.2fM rays/s",
				settings.cpuMode == CPUThreading::MultiThreaded ? "multi" : "single",
				cpuResult.renderTime.count(), cpuRps / 1.0e6 );
			std::snprintf( gpuBuf, sizeof( gpuBuf ), "GPU: %.1f ms wall (%.3f ms GPU) | %.2fM rays/s",
				gpuResult.wallClockTime.count(), gpuResult.gpuTimeMs, gpuRps / 1.0e6 );
			_pResultsPanel->setCPULine( cpuBuf );
			_pResultsPanel->setGPULine( gpuBuf );

			_lastCPUTimeMs = cpuResult.renderTime.count();
			_lastGPUTimeMs = gpuResult.gpuTimeMs;
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "Compare: CPU %.1f ms | GPU %.3f ms GPU-only\n", cpuResult.renderTime.count(), gpuResult.gpuTimeMs );
			std::fflush( stdout );
		} );
	} ).detach();
}

void AppDelegate::updateSpeedupIfPossible()
{
	if ( _lastCPUTimeMs < 0.0 || _lastGPUTimeMs < 0.0 )
		return;

	char buf[ 128 ];
	if ( _lastGPUTimeMs < _lastCPUTimeMs )
	{
		double ratio = _lastCPUTimeMs / _lastGPUTimeMs;
		std::snprintf( buf, sizeof( buf ), "GPU is %.1fx faster than CPU", ratio );
	}
	else
	{
		double ratio = _lastGPUTimeMs / _lastCPUTimeMs;
		std::snprintf( buf, sizeof( buf ), "CPU is %.1fx faster than GPU", ratio );
	}
	_pResultsPanel->setSpeedupLine( buf );
}

bool AppDelegate::applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender )
{
	return true;
}
