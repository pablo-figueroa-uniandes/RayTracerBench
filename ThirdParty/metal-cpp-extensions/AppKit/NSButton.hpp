/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 * See NSControl.hpp for why this exists.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSButton.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include "NSControl.hpp"
#include <Foundation/NSObject.hpp>
#include <Foundation/NSString.hpp>
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CGGeometry.h>

namespace NS
{
	// NSButtonType values this project actually uses (ABI-stable AppKit enum, same rationale as
	// EventMaskMouseMoved in NSEvent.hpp for hardcoding rather than wrapping the full enum).
	constexpr long ButtonTypeSwitch = 3; // renders as a real checkbox, not a push button

	class Button : public Referencing< Button, Control >
	{
		public:
			static Button*	alloc();
			Button*			init( CGRect frame );

			void			setTitle( const String* pTitle );

			// For ButtonTypeSwitch: state() is 1 when checked, 0 when unchecked. AppKit toggles
			// this itself on click — no manual bookkeeping needed, unlike a plain push button.
			void			setButtonType( long type );
			void			setState( long state );
			long			state() const;
	};
}

_NS_INLINE NS::Button* NS::Button::alloc()
{
	return Object::sendMessage< Button* >( objc_getClass( "NSButton" ), sel_registerName( "alloc" ) );
}

_NS_INLINE NS::Button* NS::Button::init( CGRect frame )
{
	return Object::sendMessage< Button* >( this, sel_registerName( "initWithFrame:" ), frame );
}

_NS_INLINE void NS::Button::setTitle( const NS::String* pTitle )
{
	Object::sendMessage< void >( this, sel_registerName( "setTitle:" ), pTitle );
}

_NS_INLINE void NS::Button::setButtonType( long type )
{
	Object::sendMessage< void >( this, sel_registerName( "setButtonType:" ), type );
}

_NS_INLINE void NS::Button::setState( long state )
{
	Object::sendMessage< void >( this, sel_registerName( "setState:" ), state );
}

_NS_INLINE long NS::Button::state() const
{
	return Object::sendMessage< long >( this, sel_registerName( "state" ) );
}
