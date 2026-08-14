/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 * See NSControl.hpp for why this exists.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSEvent.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <Foundation/NSObject.hpp>
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CGGeometry.h>

namespace NS
{
	// NSEventMask is a 64-bit bitmask indexed by NSEventType (1ULL << type). Only the one mask this
	// project actually needs is defined here — NSEventTypeMouseMoved's value (5) has been ABI-stable
	// AppKit API for decades, so this is safe to hardcode rather than needing a full NSEventType enum.
	constexpr uint64_t EventMaskMouseMoved = 1ULL << 5;

	class Event : public Referencing< Event >
	{
		public:
			// A raw Objective-C block, same idiom metal-cpp's own MTL::Device::newBuffer(...)
			// deallocator parameter already uses elsewhere in this vendored codebase.
			typedef Event* ( ^MonitorHandler )( Event* pEvent );

			static Event* addLocalMonitorForEventsMatchingMask( uint64_t mask, MonitorHandler handler );

			CGPoint     locationInWindow() const;
			class Window* window() const;
	};
}

_NS_INLINE NS::Event* NS::Event::addLocalMonitorForEventsMatchingMask( uint64_t mask, NS::Event::MonitorHandler handler )
{
	return Object::sendMessage< Event* >( objc_getClass( "NSEvent" ), sel_registerName( "addLocalMonitorForEventsMatchingMask:handler:" ), mask, handler );
}

_NS_INLINE CGPoint NS::Event::locationInWindow() const
{
	return Object::sendMessage< CGPoint >( this, sel_registerName( "locationInWindow" ) );
}

_NS_INLINE NS::Window* NS::Event::window() const
{
	return Object::sendMessage< NS::Window* >( this, sel_registerName( "window" ) );
}
