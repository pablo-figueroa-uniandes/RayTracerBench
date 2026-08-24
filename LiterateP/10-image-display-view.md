# Chapter 10: The Metal-Backed Image View, and Filling AppKit's Gaps

**Abstract.** `ImageDisplayView` is the widget that puts pixels on screen: two
instances sit side by side in the app window, one showing the CPU renderer's
output and one showing the GPU renderer's, and both support a synchronized
magnifying-glass loupe for comparing fine detail between them. Getting there
required more than writing the view itself, because Apple's `metal-cpp` and
`metal-cpp-extensions` — the header-only C++ bindings this project uses in
place of Objective-C++ — simply don't define several of the AppKit and
QuartzCore classes this view needs: `CA::MetalLayer` doesn't exist at all,
and Apple's own DTS has confirmed there is no `NS::Control`/`NS::Button`
wrapper of any kind. This chapter covers both halves together: the view
itself, and the project-local additions to `ThirdParty/metal-cpp-extensions`
that had to be written, verified standalone, and only then built upon.

**Files covered.** `RayTracerBench/App/ImageDisplayView.hpp`,
`RayTracerBench/App/ImageDisplayView.cpp`, and the project-local additions
under `ThirdParty/metal-cpp-extensions/`: `QuartzCore/CAMetalLayer.hpp`,
`AppKit/NSView.hpp`, `AppKit/NSControl.hpp`, `AppKit/NSButton.hpp`,
`AppKit/NSTextField.hpp`, `AppKit/NSAlert.hpp`, and `AppKit/NSEvent.hpp`.

## §1. A `CAMetalLayer`-backed view, deliberately not `NSImageView` or `MTKView`

The header comment states the design decision before anything else:

```cpp
// A CAMetalLayer-backed NS::View that blits a texture onto screen via a tiny textured-quad shader
// (Shaders/Blit.metal) — deliberately not NSImageView, per CLAUDE.md's UI architecture. Used two
// ways: updatePixels() uploads a CPU-side RGBA8 buffer into a texture this view owns; displayTexture()
// blits an already-existing MTL::Texture (the GPU renderer's output) directly, borrowed rather than
// owned — it stays GPURenderer's responsibility to free.
```
`RayTracerBench/App/ImageDisplayView.hpp:9-13`

Two things are being ruled out here, not just one. `NSImageView` is rejected
because the two sources of pixels this view has to display are fundamentally
different: the CPU renderer produces a plain RGBA8 byte buffer that has to be
uploaded somewhere, while the GPU renderer's output is already a live
`MTL::Texture` sitting in device memory. Routing both through `NSImageView`
would mean wrapping a `MTL::Texture` back into a `CGImage` just to hand it to
an image view that would, at some point, hand it back to the GPU to actually
draw — a round trip with no purpose. `MTKView` is rejected too, even though
it exists specifically to host Metal content, because it comes with its own
CVDisplayLink-driven draw loop and delegate protocol, machinery this project
has no use for (see §2) and that would have to be bridged through in
Objective-C anyway. What `ImageDisplayView` actually is, per the field
declarations, is a plain `NS::View` whose backing layer is manually swapped
for a `CA::MetalLayer`, with the view's own render pipeline driving that
layer directly:

```cpp
MTL::Device*              _pDevice;
CA::MetalLayer*           _pMetalLayer;
NS::View*                 _pView;
MTL::CommandQueue*        _pCommandQueue;
MTL::RenderPipelineState* _pPipelineState;
CGSize                    _viewSize;

MTL::Texture* _pOwnedTexture; // created/uploaded by updatePixels(); released in the destructor
uint32_t      _ownedTextureWidth;
uint32_t      _ownedTextureHeight;

MTL::Texture*      _pCurrentTexture; // whichever of the above (or an external texture) render() should sample
MagnifierUniforms  _magnifier;
```
`RayTracerBench/App/ImageDisplayView.hpp:60-72`

The constructor wires the layer onto the view exactly the way a
`wantsLayer`/`setLayer:`-based Cocoa view is supposed to be built, then
compiles `Blit.metal` (Chapter 3) from source and builds a render pipeline
state around it, the same "read shader source from a sibling file at
runtime" approach `main.cpp`'s comments describe for this not being an app
bundle with a compiled `default.metallib`:

```cpp
_pView = NS::View::alloc()->init( frame );
_pView->setWantsLayer( true );

_pMetalLayer = CA::MetalLayer::layer();
_pMetalLayer->setDevice( _pDevice );
_pMetalLayer->setPixelFormat( MTL::PixelFormatBGRA8Unorm );
_pMetalLayer->setFramebufferOnly( true );
_pMetalLayer->setDrawableSize( frame.size );
_pView->setLayer( _pMetalLayer );

std::string source = readShaderSource( "Blit.metal" );

NS::Error* pError = nullptr;
MTL::Library* pLibrary = _pDevice->newLibrary( NS::String::string( source.c_str(), UTF8StringEncoding ), nullptr, &pError );
```
`RayTracerBench/App/ImageDisplayView.cpp:47-60`

## §2. `updatePixels()`: no continuous draw loop, because this isn't an animation

Every render this view shows is a one-shot: the CPU or GPU renderer finishes
a frame, hands it over, and the view draws it exactly once. There is no
per-frame callback, no display link, nothing ticking in the background —
which is precisely the machinery `MTKView` would have supplied and that this
view deliberately does without (§1). `updatePixels()` is the CPU-side entry
point, and its whole job is: make sure the destination texture is the right
size, upload the bytes, and immediately render+present:

```cpp
// Uploads an RGBA8 buffer into the owned texture (recreating it if the size changed), then
// immediately renders and presents.
void ImageDisplayView::updatePixels( const uint8_t* pRGBA, uint32_t width, uint32_t height )
{
	rebuildOwnedTextureIfNeeded( width, height );

	MTL::Region region = MTL::Region( 0, 0, 0, width, height, 1 );
	_pOwnedTexture->replaceRegion( region, 0, pRGBA, static_cast<size_t>( width ) * 4 );

	_pCurrentTexture = _pOwnedTexture;
	_pMetalLayer->setDrawableSize( CGSizeMake( width, height ) );
	render();
}
```
`RayTracerBench/App/ImageDisplayView.cpp:124-134`

`rebuildOwnedTextureIfNeeded()` is the guard that keeps a texture from being
thrown away and recreated on every single frame — a render preview at a
fixed image width produces the same-sized buffer call after call, so the
common case is simply overwriting the existing texture's contents via
`replaceRegion`:

```cpp
// (Re)creates _pOwnedTexture only when width/height actually differ from the last call.
void ImageDisplayView::rebuildOwnedTextureIfNeeded( uint32_t width, uint32_t height )
{
	if ( _pOwnedTexture && width == _ownedTextureWidth && height == _ownedTextureHeight )
		return;

	if ( _pOwnedTexture )
		_pOwnedTexture->release();

	MTL::TextureDescriptor* pDesc = MTL::TextureDescriptor::texture2DDescriptor( MTL::PixelFormatRGBA8Unorm, width, height, false );
	pDesc->setStorageMode( MTL::StorageModeShared );
	pDesc->setUsage( MTL::TextureUsageShaderRead );

	_pOwnedTexture = _pDevice->newTexture( pDesc );
	_ownedTextureWidth = width;
	_ownedTextureHeight = height;
}
```
`RayTracerBench/App/ImageDisplayView.cpp:105-120`

The GPU path, `displayTexture()`, skips the upload step entirely — the
`GPURenderer`'s output is already a texture in shared storage, so this view
just points at it and presents, without ever taking ownership (the header
comment above is explicit that the borrowed texture "stays GPURenderer's
responsibility to free"):

```cpp
// Points rendering at an externally-owned texture (e.g. the GPU renderer's output) and presents.
void ImageDisplayView::displayTexture( MTL::Texture* pTexture )
{
	_pCurrentTexture = pTexture;
	_pMetalLayer->setDrawableSize( CGSizeMake( pTexture->width(), pTexture->height() ) );
	render();
}
```
`RayTracerBench/App/ImageDisplayView.cpp:137-142`

Both paths converge on the same private `render()`, which grabs one
drawable from the `CAMetalLayer`, encodes a single full-screen-quad draw
through `Blit.metal`'s pipeline, and presents:

```cpp
CA::MetalDrawable* pDrawable = _pMetalLayer->nextDrawable();
if ( !pDrawable )
	return;
/* ... render-pass/encoder setup ... */
pEncoder->setFragmentTexture( _pCurrentTexture, 0 );
pEncoder->setFragmentBytes( &_magnifier, sizeof( _magnifier ), 0 );
pEncoder->drawPrimitives( MTL::PrimitiveTypeTriangle, ( NS::UInteger )0, ( NS::UInteger )6 );
pEncoder->endEncoding();

pCommandBuffer->presentDrawable( pDrawable );
pCommandBuffer->commit();
```
`RayTracerBench/App/ImageDisplayView.cpp:161-181` (elided: render-pass descriptor and color-attachment setup, `RayTracerBench/App/ImageDisplayView.cpp:165-170`)

`setMagnifier()` also calls `render()` directly whenever a texture is
already showing, which is what lets the loupe (§5) track the mouse live
without any new pixel data arriving — the same one-shot `render()` is reused
for "new frame" and "same frame, new lens position" alike, because from this
view's perspective they're the same operation: re-encode one blit with
whatever the current `_magnifier` uniforms happen to be.

## §3. `CA::MetalLayer`: forward-declared everywhere, defined nowhere

`ImageDisplayView` needs a real `CA::MetalLayer` type — something it can
call `layer()`, `setDevice()`, `setPixelFormat()`, `setDrawableSize()`, and
`nextDrawable()` on — but vendored `metal-cpp` doesn't provide one. The only
place `CA::MetalLayer` appears in Apple's own headers is as a forward
declaration, used solely as the return type of
`CA::MetalDrawable::layer()`:

```cpp
/*
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 *
 * Base metal-cpp only forward-declares CA::MetalLayer (as the return type of
 * CA::MetalDrawable::layer()) and never defines it — there is no vendored
 * CAMetalLayer wrapper at all, in either metal-cpp or metal-cpp-extensions.
 * This header fills that gap using the same Object::sendMessage idiom
 * Apple's own headers use elsewhere. See also AppKit/NSControl.hpp.
 */
```
`ThirdParty/metal-cpp-extensions/QuartzCore/CAMetalLayer.hpp:1-11`

The fix is a project-local header at
`ThirdParty/metal-cpp-extensions/QuartzCore/CAMetalLayer.hpp` that defines
the type properly, using exactly the idiom the rest of `metal-cpp` already
uses everywhere else — `NS::Referencing<>` for retain/release-managed
lifetime, and each method a thin `Object::sendMessage<>()` call resolving a
selector by name:

```cpp
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

_NS_INLINE CA::MetalLayer* CA::MetalLayer::layer()
{
	return NS::Object::sendMessage<MetalLayer*>( objc_getClass( "CAMetalLayer" ), sel_registerName( "layer" ) );
}
```
`ThirdParty/metal-cpp-extensions/QuartzCore/CAMetalLayer.hpp:32-51`

Nothing here is a new idea — `objc_getClass`/`sel_registerName`/
`Object::sendMessage<>()` is precisely how every other class in vendored
`metal-cpp` is implemented — but the class itself simply had a hole where
`CAMetalLayer` should have been, and `ImageDisplayView` is the one place in
this codebase that needed it filled.

## §4. `NS::View::init()`: a real crash, not a missing feature

Unlike `CA::MetalLayer`, `NS::View` does exist in vendored
`metal-cpp-extensions` — but its `init()` had an outright bug, discovered
the way most convincing bugs are discovered: it crashed. Apple's shipped
version sent `initWithFrame:` to the `NSView` *class* object rather than to
an instance that had actually been `alloc()`'d, which is the Objective-C
runtime equivalent of calling a constructor on a type instead of an object —
it raises `"unrecognized selector sent to class"` at runtime, not a compile
error, since Objective-C message sends are resolved dynamically. The fix
mirrors the pattern already used correctly elsewhere in the same file for
`NS::Window`: a separate `alloc()` that returns an allocated-but-uninitialized
instance, and an `init()` that operates on `this` rather than on the class:

```cpp
// alloc() + this fixed init() are a project-local correction: Apple's
// original vendored init() sent initWithFrame: to the NSView *class*
// object instead of to an allocated instance, which crashes at runtime
// ("unrecognized selector sent to class") — verified by hand before
// fixing. addSubview()/setWantsLayer()/setLayer() are project-local
// additions filling in AppKit surface Apple's extensions never wrapped
// (see AppKit/NSControl.hpp for the same situation with NS::Button).
static View* alloc();
View*		 init( CGRect frame );
```
`ThirdParty/metal-cpp-extensions/AppKit/NSView.hpp:39-47`

```cpp
_NS_INLINE NS::View* NS::View::alloc()
{
	return Object::sendMessage< View* >( _APPKIT_PRIVATE_CLS( NSView ), _NS_PRIVATE_SEL( alloc ) );
}

_NS_INLINE NS::View* NS::View::init( CGRect frame )
{
	return Object::sendMessage< View* >( this, _APPKIT_PRIVATE_SEL( initWithFrame_ ), frame );
}
```
`ThirdParty/metal-cpp-extensions/AppKit/NSView.hpp:60-68`

Every `NS::View::alloc()->init( frame )` call in this codebase — including
the two `ImageDisplayView` construction sites in `AppDelegate.cpp` and
`ImageDisplayView.cpp:47` itself — depends on this fix; without it, simply
constructing the view that hosts the render preview would crash the app
before a single pixel could be drawn. The same header also picked up three
methods that were missing outright rather than broken: `addSubview()` (used
to attach both `ImageDisplayView`s, `ControlsPanel`, and `ResultsPanel` to
the window's content view in `AppDelegate.cpp:108-118`), and
`setWantsLayer()`/`setLayer()` — the pair `ImageDisplayView`'s constructor
uses to swap in the `CA::MetalLayer` from §3
(`RayTracerBench/App/ImageDisplayView.cpp:48,55`). `convertPoint()`, the
fourth method in this file, belongs to the magnifier feature and is covered
in §5.

## §5. The private-implementation macro rule

Metal and QuartzCore's `metal-cpp` bindings are header-only, but each of
their headers still needs *some* translation unit to actually emit the real
class/selector-cache symbols the header declares — that's what
`MTL_PRIVATE_IMPLEMENTATION` and `CA_PRIVATE_IMPLEMENTATION` do when
`#define`d immediately before the corresponding header is first included.
Define one of these macros in more than one `.cpp`, or in none, and the
result is either duplicate-symbol or undefined-symbol linker errors —
`Private::Class`/`Private::Selector` references with nothing backing them.
The rule, as `main.cpp`'s own comment states it, is one macro per header,
defined in whichever single translation unit includes that header first:

```cpp
// Metal/QuartzCore's own private-implementation macros (MTL_PRIVATE_IMPLEMENTATION,
// CA_PRIVATE_IMPLEMENTATION) live in ImageDisplayView.cpp instead, since that's the one
// translation unit that actually includes those headers — each must be defined exactly once,
// in whichever .cpp first includes the corresponding header.
#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>
```
`RayTracerBench/main.cpp:1-6`

`main.cpp` owns `NS_PRIVATE_IMPLEMENTATION` because it's the translation
unit that includes `<AppKit/AppKit.hpp>` (which pulls in Foundation).
`ImageDisplayView.cpp` is, symmetrically, the one file in the project that
actually includes `Metal/Metal.hpp` and `QuartzCore/CAMetalLayer.hpp` for
real Metal-and-layer work (as opposed to just declaring pointers to
`MTL::Device`/`MTL::Texture` the way other files do), so it carries the
other two macros:

```cpp
// This is the one translation unit that includes Metal/QuartzCore headers, so it's the one that
// must actually define their private-implementation macros (each emits real global symbol
// definitions for its header's private class/selector caches — must happen in exactly one TU).
// NS_PRIVATE_IMPLEMENTATION already lives in main.cpp, which owns Foundation/AppKit instead.
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "ImageDisplayView.hpp"
```
`RayTracerBench/App/ImageDisplayView.cpp:1-7`

This is a small piece of code but an easy one to get wrong by copy-paste,
since nothing about the macro name suggests *which* file should hold it —
only which header it pairs with and the one-definition-per-header-inclusion
rule above.

## §6. The magnifier loupe, from the host side

The loupe's purpose, stated directly in `AppDelegate.hpp`'s comment, is
comparison rather than mere magnification: hovering over *either* preview
zooms the *same* normalized position in *both*, so a user can see whether
the CPU and GPU renderers actually agree on fine detail at a given pixel —
not just get a bigger picture of one image in isolation.

```cpp
// Magnifying-glass loupe: a local NS::EventMaskMouseMoved monitor (installed once, at
// launch) reports the same normalized UV position to both ImageDisplayViews whenever the
// mouse is over either preview, so hovering one image zooms the matching detail in both —
// the actual point of the feature being to compare CPU vs. GPU output at high zoom.
void handleMouseMoved( NS::Event* pEvent );
```
`RayTracerBench/App/AppDelegate.hpp:48-52`

The shader-side lens math (sampling a de-magnified region within a circular,
aspect-corrected radius, with a thin border ring) belongs to `Blit.metal`
and is Chapter 3's territory; what this chapter covers is how that shader
gets fed live mouse coordinates from the host side, since `render()` (§2)
only ever consumes whatever `_magnifier` currently holds. `ImageDisplayView`
exposes exactly one entry point for this, `setMagnifier()`, taking a lens
center in the same normalized `[0,1]` UV convention Blit.metal expects
(`RayTracerBench/App/ImageDisplayView.hpp:37-41`) and immediately
re-rendering if a texture is already on screen.

Feeding that entry point requires knowing, continuously, where the mouse is
— and AppKit's mechanism for that is a local event monitor, which `metal-cpp`
also had no wrapper for. `AppKit/NSEvent.hpp` is a third project-local
addition, filling in just the one event mask this project needs
(`NSEventTypeMouseMoved`'s value has been ABI-stable for decades, so it's
hardcoded rather than reconstructed from a full enum) and the block-based
registration API:

```cpp
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
```
`ThirdParty/metal-cpp-extensions/AppKit/NSEvent.hpp:25-41`

The comment's cross-reference is worth taking literally: `MonitorHandler` is
declared as a genuine Objective-C block type (`Event* (^)(Event*)`), and that
compiles and links fine from plain C++ because this project already relies
on the same fact elsewhere — `MTL::Device::newBuffer`'s deallocator
parameter, vendored by Apple itself, is a block passed through
`sendMessage` the same way. `AppDelegate::setup()` installs the monitor
once, at launch, using that same block syntax directly in C++:

```cpp
NS::Event::addLocalMonitorForEventsMatchingMask( NS::EventMaskMouseMoved, ^NS::Event*( NS::Event* pEvent ) {
	handleMouseMoved( pEvent );
	return pEvent;
} );
```
`RayTracerBench/App/AppDelegate.cpp:131-134`

`handleMouseMoved()` then has to turn a window-space point into a
normalized UV coordinate local to whichever preview the mouse is over — and
that's `NS::View::convertPoint()`, the fourth method added to `NSView.hpp`
alongside the `init()` fix in §4, implementing Cocoa's documented
`convertPoint:fromView:` (`pFromView == nullptr` meaning "convert from the
window's own coordinate system"):

```cpp
_NS_INLINE CGPoint NS::View::convertPoint( CGPoint point, const NS::View* pFromView ) const
{
	return Object::sendMessage< CGPoint >( this, sel_registerName( "convertPoint:fromView:" ), point, pFromView );
}
```
`ThirdParty/metal-cpp-extensions/AppKit/NSView.hpp:85-88`

```cpp
const CGPoint windowPoint = pEvent->locationInWindow();

const CGPoint cpuLocal = _pCPUImageView->view()->convertPoint( windowPoint, nullptr );
const CGSize  cpuSize = _pCPUImageView->size();
const bool    overCPU = cpuLocal.x >= 0.0 && cpuLocal.x <= cpuSize.width && cpuLocal.y >= 0.0 && cpuLocal.y <= cpuSize.height;
```
`RayTracerBench/App/AppDelegate.cpp:144-148`

The last step is the one coordinate-space mismatch that has to be corrected
by hand: AppKit view coordinates are Y-up with the origin at the
bottom-left, while the source texture's V convention — shared with
`CPURenderer` and `Blit.metal` — is Y-down, row 0 at the top. `AppDelegate`
flips it explicitly before ever calling `setMagnifier()`:

```cpp
// AppKit view coordinates are Y-up (0 at the bottom); the source texture's V convention is
// Y-down (V=0 at the top row — see Blit.metal / CPURenderer.hpp), hence the flip.
const float u = (float)( local.x / size.width );
const float v = 1.0f - (float)( local.y / size.height );

_pCPUImageView->setMagnifier( true, u, v );
_pGPUImageView->setMagnifier( true, u, v );
```
`RayTracerBench/App/AppDelegate.cpp:164-170`

Calling `setMagnifier()` on *both* views with the same `(u, v)` — regardless
of which one the mouse is actually over — is the whole feature in one line:
it's what makes the loupe a comparison tool rather than a per-image zoom.

## §7. The permanent gap, and what fills it

`NSControl.hpp`'s own header comment states the scope of the underlying
problem plainly, and cites its source:

```cpp
/*
 * Project-local addition — NOT part of Apple's metal-cpp-extensions.
 *
 * Apple ships no NS::Control/NS::Button wrapper in metal-cpp-extensions; an Apple
 * DTS engineer confirmed this is a known, permanent gap in the AppKit headers
 * (developer.apple.com/forums/thread/722886). This header fills it using the
 * exact same idiom Apple's own headers use elsewhere (Object::sendMessage over
 * objc_msgSend, selectors resolved via sel_registerName) — no Objective-C++
 * required.
 */
```
`ThirdParty/metal-cpp-extensions/AppKit/NSControl.hpp:1-12`

This isn't a bug like §4's `NS::View::init()` — it's a hole Apple has
confirmed it does not intend to fill, which is why the project treats
extending `metal-cpp-extensions` by hand as the standing pattern rather than
a one-off workaround. `NS::Control` itself is minimal — target/action wiring
plus `setEnabled()`/`isEnabled()`, the latter pair used throughout
`ControlsPanel` to disable every field and button for the duration of a
render (Chapter 9) — and every other AppKit widget this project touches
builds on it the same way `NS::Button` does, as a `Referencing< Button,
Control >`:

```cpp
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
```
`ThirdParty/metal-cpp-extensions/AppKit/NSButton.hpp:26-45`

Three sibling headers fill in the rest of the AppKit surface this project's
UI layer needed and found similarly missing, each following the exact same
`Object::sendMessage`/`sel_registerName` idiom:

- **`AppKit/NSTextField.hpp`** wraps `NSTextField` for two roles at once —
  an editable numeric input and, via
  `setEditable(false)`/`setBezeled(false)`/`setDrawsBackground(false)`, a
  plain non-interactive label — used throughout `ControlsPanel`'s fields and
  `ResultsPanel`'s output lines (Chapter 9).
- **`AppKit/NSAlert.hpp`** wraps `NSAlert`'s message/informative text and
  `runModal()`, used by `AboutAlert` (Chapter 9) and by the scene-export
  success/failure dialog `AppDelegate::saveScene()` shows.
- **`AppKit/NSEvent.hpp`** and `NS::View::convertPoint()` are the pair this
  chapter's §6 covers directly — mouse-position tracking for the magnifier
  loupe.

## §8. Verified before being built on top of

Every one of these additions was checked with a real, standalone
compile-and-run before any of the application code above was written
against it — not written once and trusted. The clearest example is the
loupe feature itself, verified three separate ways rather than assumed
correct end to end in one shot: an offscreen shader test rendered a known
2-pixel marker through `Blit.metal` and confirmed exact 4x magnification
with no positional drift (the shader-side half of this check is Chapter
3's); a coordinate-math test built windows and views with the exact frame
dimensions the production app uses and confirmed `convertPoint()` together
with the Y-up-to-V-down flip in §6 map to the correct normalized position;
and a full end-to-end test ran the real scene/`CPURenderer`/`Blit.metal`
pipeline together and confirmed the lens actually zoomed into recognizable,
correctly-positioned scene detail rather than noise or a shifted region.
The `NS::View::init()` fix in §4 has an even more direct provenance: it was
found by actually hitting the crash Apple's original vendored code caused,
not by code review — the fix exists because something broke first.

## Where this connects

- **Chapter 3** (`Shaders/Blit.metal`) owns the shader this view drives:
  the vertex/fragment textured-quad blit `render()` (§2) dispatches every
  frame, and the `MagnifierUniforms` struct and lens sampling math that
  `setMagnifier()` (§6) feeds via `setFragmentBytes()`.
- **Chapter 8** (`main.cpp`, `App/AppDelegate.hpp/.cpp`) is where both
  `ImageDisplayView` instances are actually constructed and attached to the
  window's content view, where the `NS_PRIVATE_IMPLEMENTATION` half of §5's
  macro rule lives, and where the mouse-tracking monitor and coordinate
  conversion in §6 are wired up around this view's `setMagnifier()`
  interface.
- **Chapter 9** (`App/ControlsPanel.hpp/.cpp`, `App/ResultsPanel.hpp/.cpp`,
  `App/AboutAlert.hpp/.cpp`) is where the rest of §7's additions —
  `NS::Control`, `NS::Button`, `NS::TextField`, `NS::Alert` — do their work,
  making this chapter and Chapter 9 the two consumers of the entire
  project-local `metal-cpp-extensions` layer described here.
