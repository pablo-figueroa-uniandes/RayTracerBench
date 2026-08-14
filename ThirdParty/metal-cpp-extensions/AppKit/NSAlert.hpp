/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 * See NSControl.hpp for why this exists.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSAlert.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <Foundation/NSObject.hpp>
#include <Foundation/NSString.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

namespace NS
{
	class Alert : public Referencing< Alert >
	{
		public:
			static Alert* alloc();
			Alert*        init();

			void setMessageText( const String* pText );
			void setInformativeText( const String* pText );

			long runModal();
	};
}

_NS_INLINE NS::Alert* NS::Alert::alloc()
{
	return Object::sendMessage< Alert* >( objc_getClass( "NSAlert" ), sel_registerName( "alloc" ) );
}

_NS_INLINE NS::Alert* NS::Alert::init()
{
	return Object::sendMessage< Alert* >( this, sel_registerName( "init" ) );
}

_NS_INLINE void NS::Alert::setMessageText( const NS::String* pText )
{
	Object::sendMessage< void >( this, sel_registerName( "setMessageText:" ), pText );
}

_NS_INLINE void NS::Alert::setInformativeText( const NS::String* pText )
{
	Object::sendMessage< void >( this, sel_registerName( "setInformativeText:" ), pText );
}

_NS_INLINE long NS::Alert::runModal()
{
	return Object::sendMessage< long >( this, sel_registerName( "runModal" ) );
}
