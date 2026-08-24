# Chapter 8: The App Shell and Lifecycle

**Abstract.** Everything in the previous seven chapters — the shared ray-tracing
core, both renderers, the mesh/export pipeline — exists to be *driven* by
something. This chapter covers the thing that drives it: `main.cpp`'s
eleven-line bootstrap of an `NS::ApplicationDelegate`, and `AppDelegate`, the
one class that owns the window, wires the four UI panels together, and turns
every button click into a background render or export followed by a
main-thread UI update. The throughline is a single constraint stated in
`CLAUDE.md`'s "What this project is" section: pure C++ and Metal via
`metal-cpp`/`metal-cpp-extensions`, deliberately no SwiftUI, no
Objective-C++, no Storyboard. This chapter is where that constraint is most
visible, because application bootstrap and lifecycle is exactly the layer
where a normal macOS app would reach for `NSApplicationMain`, a
`.storyboard`, and Swift `@objc` closures, and this one instead reaches for
`NS::Application::sharedApplication()`, hand-built `NS::Window`/`NS::View`
trees, and `std::thread` + `dispatch_async`.

Files covered: `RayTracerBench/main.cpp`, `RayTracerBench/App/AppDelegate.hpp`,
`RayTracerBench/App/AppDelegate.cpp`.

## §1. `main.cpp`: the entire entry point

The whole program's `main()` is twenty-five lines, and reads almost like the
pseudocode you'd sketch before writing a "real" AppKit app — because in this
codebase, this sketch *is* the real app:

```cpp
// Metal/QuartzCore's own private-implementation macros (MTL_PRIVATE_IMPLEMENTATION,
// CA_PRIVATE_IMPLEMENTATION) live in ImageDisplayView.cpp instead, since that's the one
// translation unit that actually includes those headers — each must be defined exactly once,
// in whichever .cpp first includes the corresponding header.
#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>

#include "App/AppDelegate.hpp"

// Entry point: sets up the NSApplication/AppDelegate pair and runs the main event loop.
int main( int argc, char* argv[] )
{
	NS::AutoreleasePool* pAutoreleasePool = NS::AutoreleasePool::alloc()->init();

	AppDelegate del;

	NS::Application* pSharedApplication = NS::Application::sharedApplication();
	pSharedApplication->setDelegate( &del );
	pSharedApplication->setActivationPolicy( NS::ActivationPolicy::ActivationPolicyRegular );
	pSharedApplication->run();

	pAutoreleasePool->release();

	return 0;
}
```
(`RayTracerBench/main.cpp:1-25`)

There is no Swift, no `.storyboard`, no `Info.plist`-driven main nib. The
whole "app" concept — a shared `NSApplication` singleton with a delegate and
a run loop — is expressed directly through `metal-cpp`'s `NS::` wrappers:
`NS::AutoreleasePool` stands in for the implicit autorelease pool a normal
Cocoa `main` gets for free, `AppDelegate` is a plain C++ object (stack-
allocated, not heap-allocated — its address is handed to `setDelegate`
directly and it lives for the whole process), and
`ActivationPolicyRegular` is what makes this a normal foreground app with a
Dock icon and menu bar rather than a background agent. `pSharedApplication->run()`
is the call that actually starts the AppKit event loop; everything else in
this chapter happens either during the `applicationDidFinishLaunching`
callback that fires once that loop is up, or asynchronously afterward from
button clicks and background threads.

## §2. `NS_PRIVATE_IMPLEMENTATION`, and why it lives here specifically

`metal-cpp`'s headers are pure declarations; the actual Objective-C runtime
glue (the `Private::Class`/`Private::Selector` lookup tables that let
`metal-cpp` send real Objective-C messages from C++) is only emitted when a
translation unit defines the corresponding `_PRIVATE_IMPLEMENTATION` macro
before including the header. `main.cpp` defines `NS_PRIVATE_IMPLEMENTATION`
right before `#include <AppKit/AppKit.hpp>` (`RayTracerBench/main.cpp:5-6`),
and the comment directly above it states the rule this follows: `Metal.hpp`'s
and `QuartzCore.hpp`'s equivalents, `MTL_PRIVATE_IMPLEMENTATION` and
`CA_PRIVATE_IMPLEMENTATION`, are *not* defined here — they live in
`App/ImageDisplayView.cpp` instead (Chapter 10 covers that file in full).

The general rule, stated plainly in that comment, is: **each such macro must
be defined in exactly the one `.cpp` that first includes the corresponding
header** — get it wrong and the symptom is not a compile error but an
undefined-symbol failure at *link* time, for names like
`Private::Class::s_k...` or `Private::Selector::s_k...`, because the
implementation tables that back those declarations were simply never
emitted anywhere in the link. `main.cpp` is the one file in this project
that includes `<AppKit/AppKit.hpp>`, so it is the correct (and only
correct) home for `NS_PRIVATE_IMPLEMENTATION`; `ImageDisplayView.cpp` is the
one file that includes `<Metal/Metal.hpp>`/`<QuartzCore/QuartzCore.hpp>` for
`CAMetalLayer` purposes, so the other two macros belong there. Defining a
macro in more than one translation unit is just as broken as defining it in
none — it would emit duplicate implementation tables and fail at link time
the other way — so this is a strict one-macro-one-file assignment, not a
"define it wherever's convenient" convention.

## §3. `AppDelegate`'s shape

`AppDelegate.hpp` declares the class as an `NS::ApplicationDelegate`
subclass owning every top-level UI object and the GPU device/renderer by raw
pointer:

```cpp
class AppDelegate : public NS::ApplicationDelegate
{
	public:
		// Releases the renderer, panels, views, window, and device.
		~AppDelegate();

		// Builds the menu bar, window, and all subviews, wires up button callbacks, and installs
		// the mouse-move monitor for the magnifier.
		void applicationDidFinishLaunching( NS::Notification* pNotification ) override;
		// Always true: this app has exactly one window, so closing it should quit.
		bool applicationShouldTerminateAfterLastWindowClosed( NS::Application* pSender ) override;
```
(`RayTracerBench/App/AppDelegate.hpp:11-21`)

with the member list at the bottom of the header showing exactly what it
owns:

```cpp
		NS::Window*       _pWindow = nullptr;
		MTL::Device*      _pDevice = nullptr;
		ControlsPanel*    _pControlsPanel = nullptr;
		ImageDisplayView* _pCPUImageView = nullptr;
		ImageDisplayView* _pGPUImageView = nullptr;
		ResultsPanel*     _pResultsPanel = nullptr;
		GPURenderer*      _pGPURenderer = nullptr;

		double _lastCPUTimeMs = -1.0;
		double _lastGPUTimeMs = -1.0; // GPU-only time — the headline metric per CLAUDE.md
```
(`RayTracerBench/App/AppDelegate.hpp:54-63`)

`applicationDidFinishLaunching()` is where the actual window gets built. It
creates the Metal device and the `GPURenderer` up front (device/pipeline
creation is deliberately front-loaded, per Chapter 5, so first-render
shader-compile latency never pollutes a timed run), then lays out a single
content view holding four subviews top to bottom — controls, the two image
previews side by side, and the results panel:

```cpp
	_pDevice = MTL::CreateSystemDefaultDevice();
	_pGPURenderer = new GPURenderer( _pDevice );

	// Widened from the original 860 to fit the Save glTF / Save OBJ buttons added to row 2 of
	// ControlsPanel without crowding the existing buttons.
	const CGRect windowFrame = ( CGRect ){ { 100.0, 100.0 }, { 950.0, 460.0 } };
	_pWindow = NS::Window::alloc()->init(
		windowFrame,
		NS::WindowStyleMaskClosable | NS::WindowStyleMaskTitled,
		NS::BackingStoreBuffered,
		false );
	_pWindow->setTitle( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );
	_pWindow->setAcceptsMouseMovedEvents( true );

	const CGRect contentFrame = ( CGRect ){ { 0.0, 0.0 }, windowFrame.size };
	NS::View* pContentView = NS::View::alloc()->init( contentFrame );

	// Top to bottom: controls, two side-by-side previews, results.
	_pControlsPanel = new ControlsPanel( ( CGRect ){ { 10.0, 390.0 }, { 930.0, 60.0 } } );
	pContentView->addSubview( _pControlsPanel->view() );

	const CGRect cpuImageFrame = ( CGRect ){ { 10.0, 155.0 }, { 460.0, 225.0 } };
	const CGRect gpuImageFrame = ( CGRect ){ { 480.0, 155.0 }, { 460.0, 225.0 } };
	_pCPUImageView = new ImageDisplayView( _pDevice, cpuImageFrame );
	pContentView->addSubview( _pCPUImageView->view() );
	_pGPUImageView = new ImageDisplayView( _pDevice, gpuImageFrame );
	pContentView->addSubview( _pGPUImageView->view() );

	_pResultsPanel = new ResultsPanel( ( CGRect ){ { 10.0, 15.0 }, { 930.0, 70.0 } } );
	pContentView->addSubview( _pResultsPanel->view() );

	_pControlsPanel->onRenderCPU = [ this ]() { startCPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onRenderGPU = [ this ]() { startGPURender( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onCompare = [ this ]() { startCompare( _pControlsPanel->currentSettings() ); };
	_pControlsPanel->onSaveGLTF = [ this ]() { saveScene( true ); };
	_pControlsPanel->onSaveOBJ = [ this ]() { saveScene( false ); };

	_pWindow->setContentView( pContentView );
	_pWindow->makeKeyAndOrderFront( nullptr );

	pApp->activateIgnoringOtherApps( true );

	NS::Event::addLocalMonitorForEventsMatchingMask( NS::EventMaskMouseMoved, ^NS::Event*( NS::Event* pEvent ) {
		handleMouseMoved( pEvent );
		return pEvent;
	} );
```
(`RayTracerBench/App/AppDelegate.cpp:89-134`)

`AppDelegate` is the wiring layer and nothing more: it doesn't know how
`ControlsPanel` builds its text fields and checkbox (Chapter 9), how
`ResultsPanel` formats its three lines (Chapter 9), or how `ImageDisplayView`
turns a pixel buffer or `MTL::Texture` into something on screen via
`CAMetalLayer` and `Blit.metal` (Chapter 10). It just instantiates all four,
places them in the content view, and connects each `ControlsPanel` button
callback — five plain `std::function` members (`onRenderCPU`, `onRenderGPU`,
`onCompare`, `onSaveGLTF`, `onSaveOBJ`) — to one `AppDelegate` method each via
a capturing lambda. The magnifier's mouse-move monitor
(`NS::Event::addLocalMonitorForEventsMatchingMask` at line 131) is installed
here too, once, at launch, and `handleMouseMoved()` fans the same normalized
UV position out to both `ImageDisplayView`s (`RayTracerBench/App/AppDelegate.cpp:139-171`)
— full detail on that mechanism belongs to Chapter 10, since it's really an
`ImageDisplayView` feature that `AppDelegate` merely routes events into.

## §4. The threading pattern, used identically four times

All three render actions and `saveScene()` follow one pattern, spelled out
in the header comment above their declarations:

```cpp
		// Each spawns a background std::thread that renders, then marshals the UI update back to
		// the main thread via dispatch_async(dispatch_get_main_queue(), ...) — GCD's plain C API,
		// no Objective-C needed. Controls are disabled for the duration to prevent a second click
		// from racing an in-flight render against the same shared GPURenderer/ImageDisplayView state.
```
(`RayTracerBench/App/AppDelegate.hpp:24-27`)

`startCPURender()` is the clearest instance of it:

```cpp
void AppDelegate::startCPURender( const RenderSettings& settings )
{
	_pControlsPanel->setControlsEnabled( false );

	std::thread( [ this, settings ]()
	{
		SceneDescription scene = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.floating );
		CPURenderResult  result = renderCPU( scene, settings.cpuMode );
		double           rps = raysPerSecond( scene.params, result.renderTime.count() );

		dispatch_async( dispatch_get_main_queue(), ^{
			_pCPUImageView->updatePixels( result.pixels.data(), scene.params.width, scene.params.height );

			char buf[ 160 ];
			std::snprintf( buf, sizeof( buf ), "CPU (%s): %.1f ms | %.2fM rays/s",
				settings.cpuMode == CPUThreading::MultiThreaded ? "multi" : "single",
				result.renderTime.count(), rps / 1.0e6 );
			_pResultsPanel->setCPULine( buf );

			_lastCPUTimeMs = result.renderTime.count();
			updateSpeedupIfPossible();

			_pControlsPanel->setControlsEnabled( true );
			std::printf( "CPU render: %.1f ms\n", result.renderTime.count() );
			std::fflush( stdout );
		} );
	} ).detach();
}
```
(`RayTracerBench/App/AppDelegate.cpp:175-202`)

The shape is always the same: disable controls synchronously on the calling
(main) thread, spawn a detached `std::thread` that captures `this` and a
by-value copy of `settings`, do all the heavy work — building the scene and
invoking the renderer — off the main thread, then hand the *results* back to
the main thread through `dispatch_async(dispatch_get_main_queue(), ...)`,
where the UI is actually touched (`updatePixels`/`displayTexture`,
`setCPULine`/`setGPULine`, `setControlsEnabled(true)`). `startGPURender()`
(`RayTracerBench/App/AppDelegate.cpp:206-232`) and `startCompare()`
(`RayTracerBench/App/AppDelegate.cpp:236-273`, which does both renders on the
same background thread before a single `dispatch_async` block updates both
preview lines and the speedup line) are structurally identical, just with
`GPURenderer::render()` or both renderers in place of `renderCPU()`.

The use of `dispatch_async`/`dispatch_get_main_queue()` here is itself a
small instance of the project's larger "no Objective-C++" discipline: Grand
Central Dispatch is a plain C API (declared in `<dispatch/dispatch.h>`, which
`AppDelegate.cpp` includes at line 10), and its block-based callback argument
compiles fine as a C++ Objective-C block literal without needing an `.mm`
file — the same observation `CLAUDE.md` makes about
`NS::Event::addLocalMonitorForEventsMatchingMask`'s block argument and
`MTL::Buffer`'s deallocator parameter. Marshaling work *onto* a background
`std::thread` uses the C++ standard library; marshaling the result *back* to
the main thread uses GCD's C API. Neither needs Objective-C++.

## §5. Why controls are disabled, and why `currentSettings()` is read where it is

The comment at `AppDelegate.hpp:24-27` (quoted above) gives the reason
controls are disabled for a render's duration: `GPURenderer` and both
`ImageDisplayView`s are shared, mutable state, and only one render should be
touching them at a time. If a second click were allowed to slip in while the
first render's background thread was still running, its `dispatch_async`
block could stomp on the same texture, or `GPURenderer` could be asked to
start a second dispatch while its first hasn't finished — a data race under
one benign-looking button.

Look again at where `currentSettings()` gets called relative to
`setControlsEnabled( false )` inside `applicationDidFinishLaunching()`'s
wiring:

```cpp
	_pControlsPanel->onRenderCPU = [ this ]() { startCPURender( _pControlsPanel->currentSettings() ); };
```
(`RayTracerBench/App/AppDelegate.cpp:120`)

`currentSettings()` runs synchronously, on the main thread, as an argument
evaluated *before* `startCPURender` is even entered — and `startCPURender`'s
very first statement is `_pControlsPanel->setControlsEnabled( false )`
(`RayTracerBench/App/AppDelegate.cpp:177`), still on the main thread, still
before the `std::thread` is constructed. So the full ordering for any one
click is: read the text fields into a `RenderSettings` value → disable the
controls → *then* spin up the background thread. There is no point between
"the user could still click a control" and "the controls are now disabled"
at which a second click could be dispatched, because both steps happen
synchronously on the main thread's own event-handling call stack, with
nothing yielding back to the run loop in between. `saveScene()` follows the
exact same ordering — `currentSettings()` is read at
`RayTracerBench/App/AppDelegate.cpp:307`, immediately after
`setControlsEnabled( false )` at line 305, both still on the main thread,
before its own `std::thread` is constructed at line 309.

## §6. Verifying "doesn't freeze the UI" without a display

The whole point of putting every render on a background thread is that the
AppKit run loop should keep servicing events — window dragging, menu
clicks, the eventual "controls re-enabled" update — the entire time a
`Compare` run is grinding through both renderers. That's a claim about
runtime behavior, not something readable from the source, and the obvious
way to check it — script a UI interaction with `osascript`/System Events
while a render is in flight and confirm the window stays responsive —
turned out not to be available in this development environment:
`osascript` returned error `-25211` (no assistive-access permission for a
background session), confirmed by actually trying it, not assumed from
general knowledge of sandboxed environments.

The fix, per `CLAUDE.md`'s "Project status" notes, was the same class of
workaround already used elsewhere in this project for environment-limited
verification (the offscreen GPU-texture readback test in Chapter 10 is the
sibling case): add a temporary, env-var-gated self-test hook directly in the
app — a main-thread heartbeat plus an auto-triggered `Compare` — observe the
result, then revert the hook rather than leaving it committed. The
heartbeat was a 200ms `dispatch_source` timer on the main thread; the
self-test triggered a `Compare` run and let the heartbeat keep ticking
underneath it. The observed result: the heartbeat ticked continuously and
evenly straight through the entire ~6.5 second `Compare` run, with no
missed or delayed ticks — direct evidence that the main run loop was never
blocked by the CPU or GPU render work, because both were genuinely running
on the background `std::thread` this chapter has been describing, not
merely dispatched-and-immediately-waited-on from the main thread. That hook
is not part of the committed source (by design — it was a diagnostic, not a
feature), which is why it doesn't appear in `AppDelegate.cpp` today; this
chapter records the result rather than the hook itself.

## §7. `saveScene()`: a design that had to change under a later feature

`saveScene()` is worth reading as a small case study in how a design
constraint can be overturned by a feature added afterward, because the code
and its comments still show the seam. Building a `SceneDescription` and
writing it out as glTF or OBJ+MTL (Chapter 7) is cheap — no ray tracing
involved, just walking the entity arrays and emitting geometry — so when the
export feature was first built, `saveScene()` was a fast, synchronous,
main-thread-only operation, exactly the kind of thing that's safe to run
directly inside a button-click handler without ceremony.

That stopped being true once the companion preview PNG was added (Chapter
7's `ImageWriter`): writing a same-named `.png` next to the exported
geometry means actually calling `renderCPU()` at the current samples-per-
pixel and max-depth settings — a real, potentially multi-second CPU render,
not a cheap geometry dump. Running *that* synchronously on the main thread
would reintroduce exactly the UI freeze this whole chapter has been
describing renders being kept off of. So `saveScene()` was moved onto the
same background-thread-plus-`dispatch_async` pattern as the three render
actions:

```cpp
void AppDelegate::saveScene( bool asGLTF )
{
	_pControlsPanel->setControlsEnabled( false );

	RenderSettings settings = _pControlsPanel->currentSettings();

	std::thread( [ this, settings, asGLTF ]()
	{
		SceneDescription scene = buildDefaultScene( settings.seed, settings.width, kAspectRatio, settings.samplesPerPixel, settings.maxDepth, settings.floating );
		std::string      stem = sceneFilenameStem( settings.seed, settings.width, settings.floating );

		SceneExportResult exportResult = asGLTF ? exportSceneAsGLTF( scene, stem ) : exportSceneAsOBJ( scene, stem );
		std::string       message = exportResult.message;

		if ( exportResult.ok )
		{
			CPURenderResult preview = renderCPU( scene, settings.cpuMode );
			std::string     directory = ensureSavedScenesDirectoryPath();
			std::string     pngPath = directory + "/" + stem + ".png";
			bool            pngOk = !directory.empty() && writePNG( pngPath, preview.pixels.data(), scene.params.width, scene.params.height );
			message += pngOk ? ( "\nand " + pngPath ) : "\n(preview image failed to write)";
		}

		dispatch_async( dispatch_get_main_queue(), ^{
			using NS::StringEncoding::UTF8StringEncoding;

			// NS::Alert, not release()'d here — matches AboutAlert.cpp's existing pattern for this
			// project's other one-off modal dialogs.
			NS::Alert* pAlert = NS::Alert::alloc()->init();
			pAlert->setMessageText( NS::String::string( exportResult.ok ? "Scene Saved" : "Save Failed", UTF8StringEncoding ) );
			pAlert->setInformativeText( NS::String::string( message.c_str(), UTF8StringEncoding ) );
			pAlert->runModal();

			std::printf( "%s\n", message.c_str() );
			std::fflush( stdout );

			_pControlsPanel->setControlsEnabled( true );
		} );
	} ).detach();
}
```
(`RayTracerBench/App/AppDelegate.cpp:303-342`)

The header comment above `saveScene()`'s declaration states the causality
directly: it "[r]uns on a background thread, like the render actions above
— a full CPU render for the preview image can take a while, unlike the
geometry export alone" (`RayTracerBench/App/AppDelegate.hpp:38-42`), and the
comment immediately above the definition spells out the "before/after":
"once a full CPU render is part of this, it's no longer the fast,
main-thread-only operation it was when it only wrote geometry"
(`RayTracerBench/App/AppDelegate.cpp:300-302`). Nothing about the export
logic itself demanded a background thread; it was purely a consequence of
composing it with a feature (the preview render) that arrived later and
carries a real cost. Note too that this function's final `dispatch_async`
block runs `pAlert->runModal()` — a *blocking*, modal call — but that's fine
precisely because it's back on the main thread by then: the expensive work
(scene build, export, and the full CPU render) already happened off-thread,
so the only thing left to block on is the user dismissing a dialog, which is
supposed to be modal.

One more small echo of the theme this chapter opened with: `AboutAlert` and
`saveScene()`'s result alert both use `NS::Alert` without ever releasing it
— a deliberate, consistent convention for this project's one-off modal
dialogs, called out explicitly in the comment at
`RayTracerBench/App/AppDelegate.cpp:329-330`, rather than an oversight in
one call site that the other happens to repeat.

## §8. A gap `AppDelegate` runs straight into (and hands off)

`applicationDidFinishLaunching()` calls `NS::View::alloc()->init( contentFrame )`
to build the plain content view at `RayTracerBench/App/AppDelegate.cpp:104`,
the same `alloc()`-then-`init()` idiom used for `NS::Window` a few lines
above it. That idiom is not incidental: `metal-cpp`'s vendored
`NS::View::init()` originally sent `initWithFrame:` to the *class* object
rather than to an allocated instance — a bug this project found via a real
crash, not by inspection — and the project-local fix is exactly the
`alloc()`+`init()` two-step visible here. The bug, its diagnosis, and the
fix live in `ThirdParty/metal-cpp-extensions`, not in `AppDelegate.cpp`
itself, so full treatment of it belongs to Chapter 10; this chapter just
notes that `AppDelegate` is a direct, working beneficiary of that fix every
time it constructs a view.

## Where this connects

- **Chapter 4 (The CPU Renderer)** — `startCPURender()` and the CPU half of
  `startCompare()` both call `renderCPU()`; `saveScene()` calls it a third
  time to produce the preview image.
- **Chapter 5 (The GPU Renderer)** — `startGPURender()` and the GPU half of
  `startCompare()` call `_pGPURenderer->render()`, on the `GPURenderer`
  instance `applicationDidFinishLaunching()` constructs once, up front, at
  `RayTracerBench/App/AppDelegate.cpp:90`.
- **Chapter 7 (Scene Export: glTF, OBJ, and Preview Images)** —
  `saveScene()` is entirely a thin driver around
  `exportSceneAsGLTF()`/`exportSceneAsOBJ()`, `sceneFilenameStem()`,
  `ensureSavedScenesDirectoryPath()`, and `writePNG()`; this chapter covers
  only why and how it's threaded, not the export/PNG logic itself.
- **Chapter 9 (UI Controls and Results)** — `ControlsPanel`, `ResultsPanel`,
  and `AboutAlert` are constructed and wired here but implemented there;
  `RenderSettings`/`currentSettings()` and the five `on*` callback members
  are `ControlsPanel`'s surface, not `AppDelegate`'s.
- **Chapter 10 (The Metal-Backed Image View, and Filling AppKit's Gaps)** —
  both `ImageDisplayView`s are constructed and placed here, and the
  magnifier's mouse routing lives in `AppDelegate::handleMouseMoved()`, but
  the `CAMetalLayer` blit machinery, the `MTL_PRIVATE_IMPLEMENTATION`/
  `CA_PRIVATE_IMPLEMENTATION` placement this chapter contrasted itself
  against, and the `NS::View::init()` bug fix §8 depends on all belong to
  that chapter.
