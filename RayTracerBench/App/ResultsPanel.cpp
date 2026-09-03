#include "ResultsPanel.hpp"

// Creates a non-editable, non-bezeled, transparent-background NS::TextField, used as a plain label.
NS::TextField* ResultsPanel::makeLabel( CGRect frame, const char* text )
{
	NS::TextField* pLabel = NS::TextField::alloc()->init( frame );
	pLabel->setEditable( false );
	pLabel->setBezeled( false );
	pLabel->setDrawsBackground( false );
	pLabel->setStringValue( NS::String::string( text, NS::StringEncoding::UTF8StringEncoding ) );
	return pLabel;
}

// Builds the four label subviews (CPU, GPU, Raster, speedup), each with its default placeholder
// text, stacked bottom-to-top inside `frame`.
ResultsPanel::ResultsPanel( CGRect frame )
{
	_pContainerView = NS::View::alloc()->init( frame );

	_pCPULabel = makeLabel( ( CGRect ){ { 0.0, 68.0 }, { frame.size.width, 20.0 } }, "CPU: not yet run" );
	_pContainerView->addSubview( _pCPULabel );

	_pGPULabel = makeLabel( ( CGRect ){ { 0.0, 46.0 }, { frame.size.width, 20.0 } }, "GPU: not yet run" );
	_pContainerView->addSubview( _pGPULabel );

	_pRasterLabel = makeLabel( ( CGRect ){ { 0.0, 24.0 }, { frame.size.width, 20.0 } }, "Raster: not yet run" );
	_pContainerView->addSubview( _pRasterLabel );

	_pSpeedupLabel = makeLabel( ( CGRect ){ { 0.0, 2.0 }, { frame.size.width, 20.0 } }, "Run at least two of CPU/GPU/Raster to compare" );
	_pContainerView->addSubview( _pSpeedupLabel );
}

// Releases the four label subviews and the container view.
ResultsPanel::~ResultsPanel()
{
	_pSpeedupLabel->release();
	_pRasterLabel->release();
	_pGPULabel->release();
	_pCPULabel->release();
	_pContainerView->release();
}

// Replaces the CPU results line's text.
void ResultsPanel::setCPULine( const std::string& text )
{
	_pCPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}

// Replaces the GPU results line's text.
void ResultsPanel::setGPULine( const std::string& text )
{
	_pGPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}

// Replaces the raster results line's text.
void ResultsPanel::setRasterLine( const std::string& text )
{
	_pRasterLabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}

// Replaces the speedup-ratio line's text.
void ResultsPanel::setSpeedupLine( const std::string& text )
{
	_pSpeedupLabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}
