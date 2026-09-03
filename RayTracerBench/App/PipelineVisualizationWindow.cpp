#include "PipelineVisualizationWindow.hpp"

#include <dispatch/dispatch.h>
#include <thread>

namespace
{
	// Diagram resolution for stages 1-3 (fixed — they're schematic diagrams, not "the render", so
	// there's no reason to tie them to the user's width field). Stage 4 ("Viewport Matrix") instead
	// renders at the scene's own params.width/height, per the user's explicit "appropriate
	// dimensions" wording for that stage only.
	constexpr uint32_t kDiagramWidth = 460;
	constexpr uint32_t kDiagramHeight = 300;

	PipelineVisualizationWindow* gPipelineWindow = nullptr;

	// Button click trampoline — same capture-less-function-pointer pattern ControlsPanel.cpp's
	// gControlsPanel trampolines use, since metal-cpp-extensions' action callbacks can't hold a
	// `this` closure. Only one PipelineVisualizationWindow ever exists (AppDelegate creates it
	// lazily and keeps it alive for the app's lifetime), so a single file-local pointer is enough.
	void onRefreshClicked( void*, SEL, const NS::Object* ) { gPipelineWindow->handleRefreshClicked(); }

	NS::TextField* makeLabel( CGRect frame, const char* text )
	{
		NS::TextField* pLabel = NS::TextField::alloc()->init( frame );
		pLabel->setEditable( false );
		pLabel->setBezeled( false );
		pLabel->setDrawsBackground( false );
		pLabel->setStringValue( NS::String::string( text, NS::StringEncoding::UTF8StringEncoding ) );
		return pLabel;
	}
}

// Builds the window, its four labeled preview panels, the Refresh button, and the two renderers
// (one for the three wireframe stages, one dedicated RasterRenderer for the fourth — see the header
// comment for why it's not the main window's own renderer).
PipelineVisualizationWindow::PipelineVisualizationWindow( MTL::Device* pDevice )
	: _pDevice( pDevice->retain() )
{
	using NS::StringEncoding::UTF8StringEncoding;

	gPipelineWindow = this;

	_pStageRenderer = new PipelineStageRenderer( _pDevice );
	_pViewportStageRenderer = new RasterRenderer( _pDevice );

	const CGRect windowFrame = ( CGRect ){ { 150.0, 120.0 }, { 970.0, 800.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "Pipeline Steps", UTF8StringEncoding ) );
	// See NSWindow.hpp's comment on this method: without it, closing this window via its title-bar
	// button would deallocate it, leaving this class's _pWindow (and gPipelineWindow) dangling.
	_pWindow->setReleasedWhenClosed( false );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View*    pContentView = NS::View::alloc()->init( contentFrame );

	const char* titles[ 4 ] = { "1. Projective Matrix", "2. Camera Matrix", "3. Orthographic Matrix", "4. Viewport Matrix" };
	const CGRect panelFrames[ 4 ] = {
		( CGRect ){ { 10.0, 470.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 480.0, 470.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 10.0, 140.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
		( CGRect ){ { 480.0, 140.0 }, { (double)kDiagramWidth, (double)kDiagramHeight } },
	};

	for ( int i = 0; i < 4; ++i )
	{
		CGRect labelFrame = panelFrames[ i ];
		labelFrame.origin.y += labelFrame.size.height + 2.0;
		labelFrame.size.height = 20.0;
		_pLabels[ i ] = makeLabel( labelFrame, titles[ i ] );
		pContentView->addSubview( _pLabels[ i ] );

		_pImageViews[ i ] = new ImageDisplayView( _pDevice, panelFrames[ i ] );
		pContentView->addSubview( _pImageViews[ i ]->view() );
	}

	_pRefreshButton = NS::Button::alloc()->init( ( CGRect ){ { 10.0, 15.0 }, { 110.0, 26.0 } } );
	_pRefreshButton->setTitle( NS::String::string( "Refresh", UTF8StringEncoding ) );
	_pRefreshButton->setTarget( _pRefreshButton );
	_pRefreshButton->setAction( NS::MenuItem::registerActionCallback( "pipelineRefreshClicked", onRefreshClicked ) );
	pContentView->addSubview( _pRefreshButton );

	_pWindow->setContentView( pContentView );
}

PipelineVisualizationWindow::~PipelineVisualizationWindow()
{
	delete _pViewportStageRenderer;
	delete _pStageRenderer;
	for ( int i = 0; i < 4; ++i )
		delete _pImageViews[ i ];
	_pRefreshButton->release();
	_pWindow->release();
	_pDevice->release();
}

void PipelineVisualizationWindow::setControlsEnabled( bool enabled )
{
	_pRefreshButton->setEnabled( enabled );
}

// Computes all four stages for `scene` on a background thread (mirrors every render action's
// std::thread + dispatch_async pattern elsewhere in this app), updates the four panels, then shows
// the window.
void PipelineVisualizationWindow::showWithScene( const SceneDescription& scene )
{
	setControlsEnabled( false );

	std::thread( [ this, scene ]()
	{
		PipelineStageResult projective = _pStageRenderer->renderProjectiveStage( scene, kDiagramWidth, kDiagramHeight );
		PipelineStageResult camera = _pStageRenderer->renderCameraStage( scene, kDiagramWidth, kDiagramHeight );
		PipelineStageResult orthographic = _pStageRenderer->renderOrthographicStage( scene, kDiagramWidth, kDiagramHeight );
		RasterRenderResult  viewport = _pViewportStageRenderer->render( scene );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pImageViews[ 0 ]->displayTexture( projective.pTexture );
			_pImageViews[ 1 ]->displayTexture( camera.pTexture );
			_pImageViews[ 2 ]->displayTexture( orthographic.pTexture );
			_pImageViews[ 3 ]->displayTexture( viewport.pTexture );

			setControlsEnabled( true );
			_pWindow->makeKeyAndOrderFront( nullptr );
		} );
	} ).detach();
}

// Forwards to the onRefreshRequested callback, if the owner set one.
void PipelineVisualizationWindow::handleRefreshClicked()
{
	if ( onRefreshRequested )
		onRefreshRequested();
}
