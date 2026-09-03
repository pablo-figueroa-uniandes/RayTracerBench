#include "AppDelegate.hpp"

#include "../CPU/CPURenderer.hpp"
#include "../Core/Scene.hpp"
#include "../Export/ImageWriter.hpp"
#include "../Export/SceneExporter.hpp"
#include "../Export/SceneImporter.hpp"
#include "AboutAlert.hpp"

#include <cstdio>
#include <dispatch/dispatch.h>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// image-width is the only user-facing size control (per CLAUDE.md); height is always derived
	// from this fixed aspect ratio, matching Scene::buildDefaultScene's own design.
	constexpr float kAspectRatio = 16.0f / 9.0f;

	// Estimates rays/sec as width*height*samplesPerPixel divided by elapsed time.
	double raysPerSecond( const RenderParams& params, double milliseconds )
	{
		double rays = (double)params.width * (double)params.height * (double)params.samplesPerPixel;
		return rays / ( milliseconds / 1000.0 );
	}

	// Menu-item click trampoline for "About RayTracerBench".
	void onShowAboutClicked( void*, SEL, const NS::Object* )
	{
		showAboutAlert();
	}

	// Menu-item click trampoline for "Quit".
	void onQuitClicked( void*, SEL, const NS::Object* pSender )
	{
		NS::Application::sharedApplication()->terminate( pSender );
	}

	// Formats "<A> is Nx faster than <B>" for one pair of timings (in ms) — a small helper so
	// updateSpeedupIfPossible() below doesn't repeat this once per pair.
	std::string formatSpeedup( const char* nameA, double msA, const char* nameB, double msB )
	{
		char buf[ 96 ];
		if ( msA < msB )
			std::snprintf( buf, sizeof( buf ), "%s is %.1fx faster than %s", nameA, msB / msA, nameB );
		else
			std::snprintf( buf, sizeof( buf ), "%s is %.1fx faster than %s", nameB, msA / msB, nameA );
		return buf;
	}
}

// Releases the renderer, panels, views, window, and device, in reverse construction order.
AppDelegate::~AppDelegate()
{
	delete _pPipelineWindow;
	delete _pRasterRenderer;
	delete _pGPURenderer;
	delete _pResultsPanel;
	delete _pRasterImageView;
	delete _pGPUImageView;
	delete _pCPUImageView;
	delete _pControlsPanel;
	_pWindow->release();
	_pDevice->release();
}

// Builds the app's minimal menu bar: one app menu with "About RayTracerBench" and "Quit".
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

// Builds the menu bar, window, and every subview (controls, both image previews, results), wires
// button callbacks to the start*/updateSpeedupIfPossible methods below, and installs the
// mouse-move monitor that drives the magnifier.
void AppDelegate::applicationDidFinishLaunching( NS::Notification* pNotification )
{
	using NS::StringEncoding::UTF8StringEncoding;

	NS::Application* pApp = reinterpret_cast< NS::Application* >( pNotification->object() );
	pApp->setMainMenu( createMenuBar() );

	_pDevice = MTL::CreateSystemDefaultDevice();
	_pGPURenderer = new GPURenderer( _pDevice );
	_pRasterRenderer = new RasterRenderer( _pDevice );

	// Widened from the original 860 to fit the Save glTF / Save OBJ buttons added to row 2 of
	// ControlsPanel without crowding the existing buttons.
	const CGRect windowFrame = ( CGRect ){ { 100.0, 100.0 }, { 950.0, 460.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );
	_pWindow->setAcceptsMouseMovedEvents( true );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View* pContentView = NS::View::alloc()->init( contentFrame );

	// Top to bottom: controls, three side-by-side previews, results. The same 930px width budget
	// the controls/results rows already use, split into three ~303px columns (was two 460px columns
	// before the raster preview) rather than growing the window.
	_pControlsPanel = new ControlsPanel( ( CGRect ){ { 10.0, 390.0 }, { 930.0, 60.0 } } );
	pContentView->addSubview( _pControlsPanel->view() );

	const CGRect cpuImageFrame = ( CGRect ){ { 10.0, 155.0 }, { 303.0, 225.0 } };
	const CGRect gpuImageFrame = ( CGRect ){ { 323.0, 155.0 }, { 303.0, 225.0 } };
	const CGRect rasterImageFrame = ( CGRect ){ { 636.0, 155.0 }, { 303.0, 225.0 } };
	_pCPUImageView = new ImageDisplayView( _pDevice, cpuImageFrame );
	pContentView->addSubview( _pCPUImageView->view() );
	_pGPUImageView = new ImageDisplayView( _pDevice, gpuImageFrame );
	pContentView->addSubview( _pGPUImageView->view() );
	_pRasterImageView = new ImageDisplayView( _pDevice, rasterImageFrame );
	pContentView->addSubview( _pRasterImageView->view() );

	// Grown from the original 70 to fit the new Raster results line — there's ample unused vertical
	// gap between this panel and the previews above it, so no window resize is needed.
	_pResultsPanel = new ResultsPanel( ( CGRect ){ { 10.0, 15.0 }, { 930.0, 92.0 } } );
	pContentView->addSubview( _pResultsPanel->view() );

	_pControlsPanel->onRenderCPU = [ this ]() { startCPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onRenderGPU = [ this ]() { startGPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onRenderRaster = [ this ]() { startRasterRender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onCompare = [ this ]() { startCompare( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onShowPipeline = [ this ]() { showPipelineWindow(); };
	_pControlsPanel->onSaveGLTF = [ this ]() { saveScene( true ); };
	_pControlsPanel->onSaveOBJ = [ this ]() { saveScene( false ); };
	_pControlsPanel->onLoadScene = [ this ]() { loadScene(); };

	_pWindow->setContentView( pContentView );
	_pWindow->makeKeyAndOrderFront( nullptr );

	pApp->activateIgnoringOtherApps( true );

	NS::Event::addLocalMonitorForEventsMatchingMask( NS::EventMaskMouseMoved, ^NS::Event*( NS::Event* pEvent ) {
		handleMouseMoved( pEvent );
		return pEvent;
	} );
}

// Converts the event's window-space location into each preview's local UV coordinates and updates
// (or disables) all three previews' magnifier lenses accordingly.
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

	const CGPoint rasterLocal = _pRasterImageView->view()->convertPoint( windowPoint, nullptr );
	const CGSize  rasterSize = _pRasterImageView->size();
	const bool    overRaster = rasterLocal.x >= 0.0 && rasterLocal.x <= rasterSize.width && rasterLocal.y >= 0.0 && rasterLocal.y <= rasterSize.height;

	if ( !overCPU && !overGPU && !overRaster )
	{
		_pCPUImageView->setMagnifier( false, 0.5f, 0.5f );
		_pGPUImageView->setMagnifier( false, 0.5f, 0.5f );
		_pRasterImageView->setMagnifier( false, 0.5f, 0.5f );
		return;
	}

	const CGPoint local = overCPU ? cpuLocal : ( overGPU ? gpuLocal : rasterLocal );
	const CGSize  size = overCPU ? cpuSize : ( overGPU ? gpuSize : rasterSize );

	// AppKit view coordinates are Y-up (0 at the bottom); the source texture's V convention is
	// Y-down (V=0 at the top row — see Blit.metal / CPURenderer.hpp), hence the flip.
	const float u = (float)( local.x / size.width );
	const float v = 1.0f - (float)( local.y / size.height );

	_pCPUImageView->setMagnifier( true, u, v );
	_pGPUImageView->setMagnifier( true, u, v );
	_pRasterImageView->setMagnifier( true, u, v );
}

// Builds a scene from `settings` and renders it on the CPU, off the main thread; updates the CPU
// preview/results line and re-enables controls on completion.
void AppDelegate::startCPURender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = sceneForRender( settings );
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

// Builds a scene from `settings` and renders it on the GPU, off the main thread; updates the GPU
// preview/results line and re-enables controls on completion.
void AppDelegate::startGPURender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = sceneForRender( settings );
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

// Builds a scene from `settings` and renders it through the standard (rasterized) graphics
// pipeline, off the main thread; updates the raster preview/results line and re-enables controls
// on completion.
void AppDelegate::startRasterRender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription   scene = sceneForRender( settings );
		RasterRenderResult result = _pRasterRenderer->render( scene );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pRasterImageView->displayTexture( result.pTexture );

			char buf[ 160 ];
			std::snprintf( buf, sizeof( buf ), "Raster: %.2f ms wall (%.3f ms GPU) | %u triangles",
				result.wallClockTime.count(), result.gpuTimeMs, result.triangleCount );
			_pResultsPanel->setRasterLine( buf );

			_lastRasterTimeMs = result.gpuTimeMs;
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "Raster render: %.2f ms wall-clock, %.3f ms GPU-only, %u triangles\n",
				result.wallClockTime.count(), result.gpuTimeMs, result.triangleCount );
			std::fflush( stdout );
		} );
	} ).detach();
}

// Builds one scene from `settings` and renders it with all three renderers, off the main thread;
// updates every preview/results line and the speedup line, and re-enables controls on completion.
void AppDelegate::startCompare( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = sceneForRender( settings );

		CPURenderResult    cpuResult = renderCPU( scene, settings.cpuMode );
		GPURenderResult    gpuResult = _pGPURenderer->render( scene );
		RasterRenderResult rasterResult = _pRasterRenderer->render( scene );

		double cpuRps = raysPerSecond( scene.params, cpuResult.renderTime.count() );
		double gpuRps = raysPerSecond( scene.params, gpuResult.gpuTimeMs );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pCPUImageView->updatePixels( cpuResult.pixels.data(), scene.params.width, scene.params.height );
			_pGPUImageView->displayTexture( gpuResult.pTexture );
			_pRasterImageView->displayTexture( rasterResult.pTexture );

			char cpuBuf[ 160 ];
			char gpuBuf[ 160 ];
			char rasterBuf[ 160 ];
			std::snprintf( cpuBuf, sizeof( cpuBuf ), "CPU (%s): %.1f ms | %.2fM rays/s",
				settings.cpuMode == CPUThreading::MultiThreaded ? "multi" : "single",
				cpuResult.renderTime.count(), cpuRps / 1.0e6 );
			std::snprintf( gpuBuf, sizeof( gpuBuf ), "GPU: %.1f ms wall (%.3f ms GPU) | %.2fM rays/s",
				gpuResult.wallClockTime.count(), gpuResult.gpuTimeMs, gpuRps / 1.0e6 );
			std::snprintf( rasterBuf, sizeof( rasterBuf ), "Raster: %.2f ms wall (%.3f ms GPU) | %u triangles",
				rasterResult.wallClockTime.count(), rasterResult.gpuTimeMs, rasterResult.triangleCount );
			_pResultsPanel->setCPULine( cpuBuf );
			_pResultsPanel->setGPULine( gpuBuf );
			_pResultsPanel->setRasterLine( rasterBuf );

			_lastCPUTimeMs = cpuResult.renderTime.count();
			_lastGPUTimeMs = gpuResult.gpuTimeMs;
			_lastRasterTimeMs = rasterResult.gpuTimeMs;
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "Compare: CPU %.1f ms | GPU %.3f ms GPU-only | Raster %.3f ms GPU-only\n",
				cpuResult.renderTime.count(), gpuResult.gpuTimeMs, rasterResult.gpuTimeMs );
			std::fflush( stdout );
		} );
	} ).detach();
}

// Once at least two of CPU/GPU/Raster timings are known, formats and displays every pairwise ratio
// among whichever are known (1 pair today, up to 3 once all three have run) — e.g. "GPU is 152.3x
// faster than CPU | Raster is 9.8x faster than GPU | Raster is 1492.1x faster than CPU". No-ops
// until at least two times exist.
void AppDelegate::updateSpeedupIfPossible()
{
	struct Timing { const char* name; double ms; };
	std::vector<Timing> known;
	if ( _lastCPUTimeMs >= 0.0 )
		known.push_back( { "CPU", _lastCPUTimeMs } );
	if ( _lastGPUTimeMs >= 0.0 )
		known.push_back( { "GPU", _lastGPUTimeMs } );
	if ( _lastRasterTimeMs >= 0.0 )
		known.push_back( { "Raster", _lastRasterTimeMs } );

	if ( known.size() < 2 )
		return;

	std::string line;
	for ( size_t i = 0; i < known.size(); ++i )
		for ( size_t j = i + 1; j < known.size(); ++j )
		{
			if ( !line.empty() )
				line += " | ";
			line += formatSpeedup( known[ i ].name, known[ i ].ms, known[ j ].name, known[ j ].ms );
		}

	_pResultsPanel->setSpeedupLine( line );
}

// Builds a scene from the current controls settings (geometry depends only on seed/width/
// floating — samplesPerPixel/maxDepth affect rendering, not the exported geometry), writes it via
// the requested exporter, and — on success — also renders it (at the current settings, exactly
// like "Render CPU" would) and writes a same-named PNG preview alongside it, so the 3D export has
// a quick-look image without needing a model viewer. Runs on a background thread like the render
// actions above: once a full CPU render is part of this, it's no longer the fast, main-thread-only
// operation it was when it only wrote geometry.
void AppDelegate::saveScene( bool asGLTF )
{
	_pControlsPanel->setControlsEnabled( false );

	RenderSettings settings = _pControlsPanel->currentSettings();

	std::thread( [ this, settings, asGLTF ]()
	{
		SceneDescription scene = sceneForRender( settings );
		std::string      stem = sceneFilenameStem( settings.seed, settings.width, settings.floating );

		SceneExportResult exportResult = asGLTF ? exportSceneAsGLTF( scene, stem ) : exportSceneAsOBJ( scene, stem );
		std::string       message = exportResult.message;

		if ( exportResult.ok )
		{
			CPURenderResult preview = renderCPU( scene, settings.cpuMode );
			std::string     directory = ensureSavedScenesDirectoryPath();
			std::string     pngPath = directory + "/" + stem + ".png";
			bool            pngOk = !directory.empty() && writePNG( pngPath, preview.pixels.data(), scene.params.width, scene.params.height );
			message += pngOk ? ( "\nand " + pngPath ) : "\n(preview image failed to write)";
		}

		dispatch_async( dispatch_get_main_queue(), ^{
			using NS::StringEncoding::UTF8StringEncoding;

			// NS::Alert, not release()'d here — matches AboutAlert.cpp's existing pattern for this
			// project's other one-off modal dialogs.
			NS::Alert* pAlert = NS::Alert::alloc()->init();
			pAlert->setMessageText( NS::String::string( exportResult.ok ? "Scene Saved" : "Save Failed", UTF8StringEncoding ) );
			pAlert->setInformativeText( NS::String::string( message.c_str(), UTF8StringEncoding ) );
			pAlert->runModal();

			std::printf( "%s\n", message.c_str() );
			std::fflush( stdout );

			_pControlsPanel->setControlsEnabled( true );
		} );
	} ).detach();
}

// `_loadedScene`'s entities/materials (geometry SceneImporter.hpp reconstructed from a file) carry
// no camera/render-params of their own — those aren't part of what either export format stores
// (see SceneExporter.hpp) — so camera/params always come from a freshly built buildDefaultScene()
// at the CURRENT settings, exactly like the no-loaded-scene case, even when the geometry itself
// comes from `_loadedScene`. That keeps width/samples-per-pixel/max-depth edits taking effect on a
// loaded scene's render the same way they already do on a generated one.
SceneDescription AppDelegate::sceneForRender( const RenderSettings& settings ) const
{
	if ( !_hasLoadedScene )
		return buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.floating );

	SceneDescription scene = _loadedScene;
	SceneDescription cameraSource = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.floating );
	scene.camera = cameraSource.camera;
	scene.params = cameraSource.params;
	return scene;
}

// Shows (creating it the first time) the "Pipeline Steps" window, populated from a scene built at
// the main window's current settings. The window's own "Refresh" button re-runs this same
// rebuild-from-current-settings logic, wired once here rather than every time the window is shown.
void AppDelegate::showPipelineWindow()
{
	if ( !_pPipelineWindow )
	{
		_pPipelineWindow = new PipelineVisualizationWindow( _pDevice );
		_pPipelineWindow->onRefreshRequested = [ this ]() {
			_pPipelineWindow->showWithScene( sceneForRender( _pControlsPanel->currentSettings() ) );
		};
	}

	_pPipelineWindow->showWithScene( sceneForRender( _pControlsPanel->currentSettings() ) );
}

// Shows a file picker, and on a chosen .gltf/.obj hands it to SceneImporter.hpp on a background
// thread — see the header comment for the full contract.
void AppDelegate::loadScene()
{
	using NS::StringEncoding::UTF8StringEncoding;

	NS::OpenPanel* pPanel = NS::OpenPanel::openPanel();
	pPanel->setCanChooseFiles( true );
	pPanel->setCanChooseDirectories( false );
	pPanel->setAllowsMultipleSelection( false );

	if ( pPanel->runModal() != 1 /* NSModalResponseOK */ )
		return; // user cancelled — nothing to do, controls were never disabled

	std::string path = pPanel->url()->fileSystemRepresentation();

	_pControlsPanel->setControlsEnabled( false );

	RenderSettings settings = _pControlsPanel->currentSettings();

	std::thread( [ this, settings, path ]()
	{
		// Format is decided by the chosen file's own extension, not a panel-side type filter (see
		// NSOpenPanel.hpp's header comment) — case-sensitive is fine here since both this app's own
		// exporter and the check below always use lowercase ".gltf"/".obj".
		bool isGLTF = path.size() >= 5 && path.compare( path.size() - 5, 5, ".gltf" ) == 0;
		bool isOBJ = path.size() >= 4 && path.compare( path.size() - 4, 4, ".obj" ) == 0;

		SceneImportResult importResult;
		if ( isGLTF )
			importResult = importSceneFromGLTF( path, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.seed );
		else if ( isOBJ )
			importResult = importSceneFromOBJ( path, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.seed );
		else
		{
			importResult.ok = false;
			importResult.message = "Unrecognized file extension (expected .gltf or .obj): " + path;
		}

		dispatch_async( dispatch_get_main_queue(), ^{
			if ( importResult.ok )
			{
				_loadedScene = importResult.scene;
				_hasLoadedScene = true;
			}

			// NS::Alert, not release()'d here — matches AboutAlert.cpp's existing pattern for this
			// project's other one-off modal dialogs.
			NS::Alert* pAlert = NS::Alert::alloc()->init();
			pAlert->setMessageText( NS::String::string( importResult.ok ? "Scene Loaded" : "Load Failed", UTF8StringEncoding ) );
			pAlert->setInformativeText( NS::String::string( importResult.message.c_str(), UTF8StringEncoding ) );
			pAlert->runModal();

			std::printf( "%s\n", importResult.message.c_str() );
			std::fflush( stdout );

			_pControlsPanel->setControlsEnabled( true );
		} );
	} ).detach();
}

// Always true: this app has exactly one window, so closing it should quit.
bool AppDelegate::applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender )
{
	return true;
}
