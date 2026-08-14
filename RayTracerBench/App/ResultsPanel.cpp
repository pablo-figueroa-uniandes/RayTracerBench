#include "ResultsPanel.hpp"

NS::TextField* ResultsPanel::makeLabel( CGRect frame, const char* text )
{
	NS::TextField* pLabel = NS::TextField::alloc()->init( frame );
	pLabel->setEditable( false );
	pLabel->setBezeled( false );
	pLabel->setDrawsBackground( false );
	pLabel->setStringValue( NS::String::string( text, NS::StringEncoding::UTF8StringEncoding ) );
	return pLabel;
}

ResultsPanel::ResultsPanel( CGRect frame )
{
	_pContainerView = NS::View::alloc()->init( frame );

	_pCPULabel = makeLabel( ( CGRect ){ { 0.0, 46.0 }, { frame.size.width, 20.0 } }, "CPU: not yet run" );
	_pContainerView->addSubview( _pCPULabel );

	_pGPULabel = makeLabel( ( CGRect ){ { 0.0, 24.0 }, { frame.size.width, 20.0 } }, "GPU: not yet run" );
	_pContainerView->addSubview( _pGPULabel );

	_pSpeedupLabel = makeLabel( ( CGRect ){ { 0.0, 2.0 }, { frame.size.width, 20.0 } }, "Run both CPU and GPU to compare" );
	_pContainerView->addSubview( _pSpeedupLabel );
}

ResultsPanel::~ResultsPanel()
{
	_pSpeedupLabel->release();
	_pGPULabel->release();
	_pCPULabel->release();
	_pContainerView->release();
}

void ResultsPanel::setCPULine( const std::string& text )
{
	_pCPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}

void ResultsPanel::setGPULine( const std::string& text )
{
	_pGPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}

void ResultsPanel::setSpeedupLine( const std::string& text )
{
	_pSpeedupLabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}
