#include "ControlsPanel.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>

namespace
{
	ControlsPanel* gControlsPanel = nullptr;

	void onRandomizeSeedClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRandomizeSeedClicked(); }
	void onThreadingToggleClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleThreadingToggleClicked(); }
	void onRenderCPUClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRenderCPUClicked(); }
	void onRenderGPUClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRenderGPUClicked(); }
	void onCompareClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleCompareClicked(); }

	uint32_t parseUInt( NS::TextField* pField, uint32_t minVal, uint32_t maxVal, uint32_t fallback )
	{
		const char* pText = pField->stringValue()->utf8String();
		char*       pEnd = nullptr;
		long        value = std::strtol( pText, &pEnd, 10 );
		if ( pEnd == pText )
			return fallback;
		if ( value < (long)minVal )
			value = minVal;
		if ( value > (long)maxVal )
			value = maxVal;
		return (uint32_t)value;
	}
}

NS::TextField* ControlsPanel::makeLabel( CGRect frame, const char* text )
{
	NS::TextField* pLabel = NS::TextField::alloc()->init( frame );
	pLabel->setEditable( false );
	pLabel->setBezeled( false );
	pLabel->setDrawsBackground( false );
	pLabel->setStringValue( NS::String::string( text, NS::StringEncoding::UTF8StringEncoding ) );
	return pLabel;
}

NS::TextField* ControlsPanel::makeField( CGRect frame, const char* initialText )
{
	NS::TextField* pField = NS::TextField::alloc()->init( frame );
	pField->setEditable( true );
	pField->setBezeled( true );
	pField->setStringValue( NS::String::string( initialText, NS::StringEncoding::UTF8StringEncoding ) );
	return pField;
}

ControlsPanel::ControlsPanel( CGRect frame )
	: _multiThreaded( true )
{
	using NS::StringEncoding::UTF8StringEncoding;

	gControlsPanel = this;

	_pContainerView = NS::View::alloc()->init( frame );

	// Row 1 (top): width / samples-per-pixel / max-depth / seed fields + randomize.
	_pContainerView->addSubview( makeLabel( ( CGRect ){ { 0.0, 32.0 }, { 45.0, 20.0 } }, "Width:" ) );
	_pWidthField = makeField( ( CGRect ){ { 48.0, 32.0 }, { 55.0, 22.0 } }, "400" );
	_pContainerView->addSubview( _pWidthField );

	_pContainerView->addSubview( makeLabel( ( CGRect ){ { 115.0, 32.0 }, { 35.0, 20.0 } }, "SPP:" ) );
	_pSppField = makeField( ( CGRect ){ { 153.0, 32.0 }, { 55.0, 22.0 } }, "20" );
	_pContainerView->addSubview( _pSppField );

	_pContainerView->addSubview( makeLabel( ( CGRect ){ { 220.0, 32.0 }, { 45.0, 20.0 } }, "Depth:" ) );
	_pMaxDepthField = makeField( ( CGRect ){ { 268.0, 32.0 }, { 55.0, 22.0 } }, "20" );
	_pContainerView->addSubview( _pMaxDepthField );

	_pContainerView->addSubview( makeLabel( ( CGRect ){ { 335.0, 32.0 }, { 40.0, 20.0 } }, "Seed:" ) );
	_pSeedField = makeField( ( CGRect ){ { 378.0, 32.0 }, { 80.0, 22.0 } }, "1234" );
	_pContainerView->addSubview( _pSeedField );

	_pRandomizeSeedButton = NS::Button::alloc()->init( ( CGRect ){ { 465.0, 30.0 }, { 90.0, 26.0 } } );
	_pRandomizeSeedButton->setTitle( NS::String::string( "Randomize", UTF8StringEncoding ) );
	_pRandomizeSeedButton->setTarget( _pRandomizeSeedButton );
	_pRandomizeSeedButton->setAction( NS::MenuItem::registerActionCallback( "controlsRandomizeSeedClicked", onRandomizeSeedClicked ) );
	_pContainerView->addSubview( _pRandomizeSeedButton );

	// Row 2 (bottom): CPU-threading toggle + Render CPU / Render GPU / Compare.
	_pThreadingToggleButton = NS::Button::alloc()->init( ( CGRect ){ { 0.0, 2.0 }, { 170.0, 26.0 } } );
	_pThreadingToggleButton->setTitle( NS::String::string( "CPU Threads: Multi", UTF8StringEncoding ) );
	_pThreadingToggleButton->setTarget( _pThreadingToggleButton );
	_pThreadingToggleButton->setAction( NS::MenuItem::registerActionCallback( "controlsThreadingToggleClicked", onThreadingToggleClicked ) );
	_pContainerView->addSubview( _pThreadingToggleButton );

	_pRenderCPUButton = NS::Button::alloc()->init( ( CGRect ){ { 180.0, 2.0 }, { 110.0, 26.0 } } );
	_pRenderCPUButton->setTitle( NS::String::string( "Render CPU", UTF8StringEncoding ) );
	_pRenderCPUButton->setTarget( _pRenderCPUButton );
	_pRenderCPUButton->setAction( NS::MenuItem::registerActionCallback( "controlsRenderCPUClicked", onRenderCPUClicked ) );
	_pContainerView->addSubview( _pRenderCPUButton );

	_pRenderGPUButton = NS::Button::alloc()->init( ( CGRect ){ { 300.0, 2.0 }, { 110.0, 26.0 } } );
	_pRenderGPUButton->setTitle( NS::String::string( "Render GPU", UTF8StringEncoding ) );
	_pRenderGPUButton->setTarget( _pRenderGPUButton );
	_pRenderGPUButton->setAction( NS::MenuItem::registerActionCallback( "controlsRenderGPUClicked", onRenderGPUClicked ) );
	_pContainerView->addSubview( _pRenderGPUButton );

	_pCompareButton = NS::Button::alloc()->init( ( CGRect ){ { 420.0, 2.0 }, { 110.0, 26.0 } } );
	_pCompareButton->setTitle( NS::String::string( "Compare", UTF8StringEncoding ) );
	_pCompareButton->setTarget( _pCompareButton );
	_pCompareButton->setAction( NS::MenuItem::registerActionCallback( "controlsCompareClicked", onCompareClicked ) );
	_pContainerView->addSubview( _pCompareButton );

	// A real checkbox — AppKit manages its own checked state on click, so no target/action wiring
	// is needed at all; currentSettings() just reads state() on demand.
	_pFloatingCheckbox = NS::Button::alloc()->init( ( CGRect ){ { 550.0, 2.0 }, { 110.0, 26.0 } } );
	_pFloatingCheckbox->setButtonType( NS::ButtonTypeSwitch );
	_pFloatingCheckbox->setTitle( NS::String::string( "Floating?", UTF8StringEncoding ) );
	_pContainerView->addSubview( _pFloatingCheckbox );
}

ControlsPanel::~ControlsPanel()
{
	_pFloatingCheckbox->release();
	_pCompareButton->release();
	_pRenderGPUButton->release();
	_pRenderCPUButton->release();
	_pRandomizeSeedButton->release();
	_pThreadingToggleButton->release();
	_pSeedField->release();
	_pMaxDepthField->release();
	_pSppField->release();
	_pWidthField->release();
	_pContainerView->release();
}

RenderSettings ControlsPanel::currentSettings() const
{
	RenderSettings settings;
	settings.width = parseUInt( _pWidthField, 50, 2000, 400 );
	settings.samplesPerPixel = parseUInt( _pSppField, 1, 2000, 20 );
	settings.maxDepth = parseUInt( _pMaxDepthField, 1, 100, 20 );
	settings.seed = parseUInt( _pSeedField, 0, 0xFFFFFFFFu, 1234 );
	settings.cpuMode = _multiThreaded ? CPUThreading::MultiThreaded : CPUThreading::SingleThreaded;
	settings.floating = _pFloatingCheckbox->state() != 0;
	return settings;
}

void ControlsPanel::setControlsEnabled( bool enabled )
{
	_pWidthField->setEnabled( enabled );
	_pSppField->setEnabled( enabled );
	_pMaxDepthField->setEnabled( enabled );
	_pSeedField->setEnabled( enabled );
	_pThreadingToggleButton->setEnabled( enabled );
	_pRandomizeSeedButton->setEnabled( enabled );
	_pRenderCPUButton->setEnabled( enabled );
	_pRenderGPUButton->setEnabled( enabled );
	_pCompareButton->setEnabled( enabled );
	_pFloatingCheckbox->setEnabled( enabled );
}

void ControlsPanel::handleRandomizeSeedClicked()
{
	std::random_device rd;
	unsigned           newSeed = rd();
	char               buf[ 16 ];
	std::snprintf( buf, sizeof( buf ), "%u", newSeed );
	_pSeedField->setStringValue( NS::String::string( buf, NS::StringEncoding::UTF8StringEncoding ) );
}

void ControlsPanel::handleThreadingToggleClicked()
{
	_multiThreaded = !_multiThreaded;
	_pThreadingToggleButton->setTitle( NS::String::string(
		_multiThreaded ? "CPU Threads: Multi" : "CPU Threads: Single",
		NS::StringEncoding::UTF8StringEncoding ) );
}

void ControlsPanel::handleRenderCPUClicked()
{
	if ( onRenderCPU )
		onRenderCPU();
}

void ControlsPanel::handleRenderGPUClicked()
{
	if ( onRenderGPU )
		onRenderGPU();
}

void ControlsPanel::handleCompareClicked()
{
	if ( onCompare )
		onCompare();
}
