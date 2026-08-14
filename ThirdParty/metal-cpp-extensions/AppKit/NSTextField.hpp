/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 * See NSControl.hpp for why this exists.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSTextField.hpp
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
	class TextField : public Referencing< TextField, Control >
	{
		public:
			static TextField* alloc();
			TextField*        init( CGRect frame );

			void    setStringValue( const String* pValue );
			String* stringValue() const;

			// Also used to make a TextField act as a plain, non-interactive label — clear
			// ControlsPanel/ResultsPanel field vs. label intent, rather than a bag of booleans:
			// setEditable(false) + setBezeled(false) + setDrawsBackground(false).
			void setEditable( bool editable );
			void setBezeled( bool bezeled );
			void setDrawsBackground( bool drawsBackground );
	};
}

_NS_INLINE NS::TextField* NS::TextField::alloc()
{
	return Object::sendMessage< TextField* >( objc_getClass( "NSTextField" ), sel_registerName( "alloc" ) );
}

_NS_INLINE NS::TextField* NS::TextField::init( CGRect frame )
{
	return Object::sendMessage< TextField* >( this, sel_registerName( "initWithFrame:" ), frame );
}

_NS_INLINE void NS::TextField::setStringValue( const NS::String* pValue )
{
	Object::sendMessage< void >( this, sel_registerName( "setStringValue:" ), pValue );
}

_NS_INLINE NS::String* NS::TextField::stringValue() const
{
	return Object::sendMessage< String* >( this, sel_registerName( "stringValue" ) );
}

_NS_INLINE void NS::TextField::setEditable( bool editable )
{
	Object::sendMessage< void >( this, sel_registerName( "setEditable:" ), editable );
}

_NS_INLINE void NS::TextField::setBezeled( bool bezeled )
{
	Object::sendMessage< void >( this, sel_registerName( "setBezeled:" ), bezeled );
}

_NS_INLINE void NS::TextField::setDrawsBackground( bool drawsBackground )
{
	Object::sendMessage< void >( this, sel_registerName( "setDrawsBackground:" ), drawsBackground );
}
