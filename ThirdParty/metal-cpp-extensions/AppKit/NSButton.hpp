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
	class Button : public Referencing< Button, Control >
	{
		public:
			static Button*	alloc();
			Button*			init( CGRect frame );

			void			setTitle( const String* pTitle );
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
