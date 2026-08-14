/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 *
 * Base metal-cpp only forward-declares CA::MetalLayer (as the return type of
 * CA::MetalDrawable::layer()) and never defines it — there is no vendored
 * CAMetalLayer wrapper at all, in either metal-cpp or metal-cpp-extensions.
 * This header fills that gap using the same Object::sendMessage idiom
 * Apple's own headers use elsewhere. See also AppKit/NSControl.hpp.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// QuartzCore/CAMetalLayer.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <QuartzCore/CAMetalDrawable.hpp>

#include <Metal/MTLDevice.hpp>
#include <Metal/MTLPixelFormat.hpp>

#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace CA
{
	class MetalLayer : public NS::Referencing<MetalLayer>
	{
		public:
			static MetalLayer* layer();

			void setDevice( const MTL::Device* pDevice );
			void setPixelFormat( MTL::PixelFormat pixelFormat );
			void setFramebufferOnly( bool framebufferOnly );
			void setDrawableSize( CGSize size );

			MetalDrawable* nextDrawable();
	};
}

_NS_INLINE CA::MetalLayer* CA::MetalLayer::layer()
{
	return NS::Object::sendMessage<MetalLayer*>( objc_getClass( "CAMetalLayer" ), sel_registerName( "layer" ) );
}

_NS_INLINE void CA::MetalLayer::setDevice( const MTL::Device* pDevice )
{
	NS::Object::sendMessage<void>( this, sel_registerName( "setDevice:" ), pDevice );
}

_NS_INLINE void CA::MetalLayer::setPixelFormat( MTL::PixelFormat pixelFormat )
{
	NS::Object::sendMessage<void>( this, sel_registerName( "setPixelFormat:" ), pixelFormat );
}

_NS_INLINE void CA::MetalLayer::setFramebufferOnly( bool framebufferOnly )
{
	NS::Object::sendMessage<void>( this, sel_registerName( "setFramebufferOnly:" ), framebufferOnly );
}

_NS_INLINE void CA::MetalLayer::setDrawableSize( CGSize size )
{
	NS::Object::sendMessage<void>( this, sel_registerName( "setDrawableSize:" ), size );
}

_NS_INLINE CA::MetalDrawable* CA::MetalLayer::nextDrawable()
{
	return NS::Object::sendMessage<MetalDrawable*>( this, sel_registerName( "nextDrawable" ) );
}
