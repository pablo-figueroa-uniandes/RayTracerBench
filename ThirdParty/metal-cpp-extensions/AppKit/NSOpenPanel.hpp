/*
 *
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 * See NSControl.hpp for why this exists: Apple's vendored metal-cpp-extensions has no
 * NSOpenPanel wrapper at all (same confirmed, permanent gap as NSButton/NSControl), so this
 * fills it with the exact same Object::sendMessage idiom Apple's own headers use.
 *
 * Deliberately minimal: just enough to let the user pick a single file to load (canChooseFiles,
 * canChooseDirectories, allowsMultipleSelection, runModal, url()) — no allowedContentTypes/
 * allowedFileTypes filtering wrapper, since this app tells .gltf from .obj by the chosen file's
 * own extension after the fact rather than restricting the panel's file-type filter.
 *
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSOpenPanel.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <Foundation/NSObject.hpp>
#include <Foundation/NSURL.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

namespace NS
{
	class OpenPanel : public Referencing< OpenPanel >
	{
		public:
			// NSOpenPanel is normally obtained via +openPanel, not +alloc/-init, and is a shared
			// autoreleased instance per Apple's docs — matches how this is actually used here (one
			// synchronous runModal() call on the main thread, no retained ownership needed after).
			static OpenPanel* openPanel();

			void setCanChooseFiles( bool value );
			void setCanChooseDirectories( bool value );
			void setAllowsMultipleSelection( bool value );

			// Blocks (this is a real modal panel) until the user picks a file or cancels; returns
			// NSModalResponseOK (1) or NSModalResponseCancel (0).
			long runModal();
			// Valid only after runModal() returns NSModalResponseOK.
			URL* url() const;
	};
}

_NS_INLINE NS::OpenPanel* NS::OpenPanel::openPanel()
{
	return Object::sendMessage< OpenPanel* >( objc_getClass( "NSOpenPanel" ), sel_registerName( "openPanel" ) );
}

_NS_INLINE void NS::OpenPanel::setCanChooseFiles( bool value )
{
	Object::sendMessage< void >( this, sel_registerName( "setCanChooseFiles:" ), value );
}

_NS_INLINE void NS::OpenPanel::setCanChooseDirectories( bool value )
{
	Object::sendMessage< void >( this, sel_registerName( "setCanChooseDirectories:" ), value );
}

_NS_INLINE void NS::OpenPanel::setAllowsMultipleSelection( bool value )
{
	Object::sendMessage< void >( this, sel_registerName( "setAllowsMultipleSelection:" ), value );
}

_NS_INLINE long NS::OpenPanel::runModal()
{
	return Object::sendMessage< long >( this, sel_registerName( "runModal" ) );
}

_NS_INLINE NS::URL* NS::OpenPanel::url() const
{
	return Object::sendMessage< URL* >( this, sel_registerName( "URL" ) );
}
