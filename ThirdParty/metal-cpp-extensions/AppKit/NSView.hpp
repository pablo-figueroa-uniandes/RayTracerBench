/*
 *
 * Copyright 2020-2021 Apple Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSView.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include "AppKitPrivate.hpp"
#include <Foundation/NSObject.hpp>
#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace NS
{
	class View : public NS::Referencing< View >
	{
		public:
			// alloc() + this fixed init() are a project-local correction: Apple's
			// original vendored init() sent initWithFrame: to the NSView *class*
			// object instead of to an allocated instance, which crashes at runtime
			// ("unrecognized selector sent to class") — verified by hand before
			// fixing. addSubview()/setWantsLayer()/setLayer() are project-local
			// additions filling in AppKit surface Apple's extensions never wrapped
			// (see AppKit/NSControl.hpp for the same situation with NS::Button).
			static View* alloc();
			View*		 init( CGRect frame );

			void		 addSubview( const View* pSubview );
			void		 setWantsLayer( bool wantsLayer );
			void		 setLayer( const void* pLayer );

			// pFromView == nullptr converts from the window's own coordinate system, per Cocoa's
			// documented convertPoint:fromView: behavior — used for hit-testing mouse events
			// against a specific view without needing that view to be a real event responder.
			CGPoint		 convertPoint( CGPoint point, const View* pFromView ) const;
	};
}

_NS_INLINE NS::View* NS::View::alloc()
{
	return Object::sendMessage< View* >( _APPKIT_PRIVATE_CLS( NSView ), _NS_PRIVATE_SEL( alloc ) );
}

_NS_INLINE NS::View* NS::View::init( CGRect frame )
{
	return Object::sendMessage< View* >( this, _APPKIT_PRIVATE_SEL( initWithFrame_ ), frame );
}

_NS_INLINE void NS::View::addSubview( const NS::View* pSubview )
{
	Object::sendMessage< void >( this, sel_registerName( "addSubview:" ), pSubview );
}

_NS_INLINE void NS::View::setWantsLayer( bool wantsLayer )
{
	Object::sendMessage< void >( this, sel_registerName( "setWantsLayer:" ), wantsLayer );
}

_NS_INLINE void NS::View::setLayer( const void* pLayer )
{
	Object::sendMessage< void >( this, sel_registerName( "setLayer:" ), pLayer );
}

_NS_INLINE CGPoint NS::View::convertPoint( CGPoint point, const NS::View* pFromView ) const
{
	return Object::sendMessage< CGPoint >( this, sel_registerName( "convertPoint:fromView:" ), point, pFromView );
}
