/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 *
 * Apple ships no NS::Control/NS::Button wrapper in metal-cpp-extensions; an Apple
 * DTS engineer confirmed this is a known, permanent gap in the AppKit headers
 * (developer.apple.com/forums/thread/722886). This header fills it using the
 * exact same idiom Apple's own headers use elsewhere (Object::sendMessage over
 * objc_msgSend, selectors resolved via sel_registerName) — no Objective-C++
 * required.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSControl.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include "NSView.hpp"
#include <Foundation/NSObject.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

namespace NS
{
	class Control : public Referencing< Control, View >
	{
		public:
			void	setTarget( const Object* pTarget );
			void	setAction( SEL pAction );
	};
}

_NS_INLINE void NS::Control::setTarget( const NS::Object* pTarget )
{
	Object::sendMessage< void >( this, sel_registerName( "setTarget:" ), pTarget );
}

_NS_INLINE void NS::Control::setAction( SEL pAction )
{
	Object::sendMessage< void >( this, sel_registerName( "setAction:" ), pAction );
}
