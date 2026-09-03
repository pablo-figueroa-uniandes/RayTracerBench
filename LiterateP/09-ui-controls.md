# Chapter 9: UI Controls and Results

**Abstract.** This chapter covers the three files that make up RayTracerBench's
control surface: `ControlsPanel`, which owns every input field, toggle,
checkbox, and button the user touches to configure and trigger a render or
export; `ResultsPanel`, the deliberately dumb four-line label strip that
displays whatever timing text `AppDelegate` hands it; and `AboutAlert`, the
one-shot attribution dialog. None of these files render anything or know how
long a render took — they parse input, forward clicks through
`std::function` callbacks, and display strings. That's still true after
three more render paths were added on top of the original CPU/GPU pair
(Raster in Chapter 13, the Pipeline Steps window in Chapter 14, and the
reverse-import "Load Scene" feature in Chapter 7) — each new button is one
more four-line allocate/title/target/action block and one more
`std::function` member, not a change to either class's actual job. The
interesting engineering in this chapter is not in what these classes compute
(almost nothing) but in how they talk to AppKit without a line of
Objective-C++: capture-less function-pointer trampolines standing in for
target/action selectors, a real `NSButtonTypeSwitch` checkbox that needs no
click handler at all contrasted against an older toggle button that has to
fake its own checked state, and — added later — a real `NS::OpenPanel` file
picker for "Load Scene." Files covered: `App/ControlsPanel.hpp`,
`App/ControlsPanel.cpp`, `App/ResultsPanel.hpp`, `App/ResultsPanel.cpp`,
`App/AboutAlert.hpp`, `App/AboutAlert.cpp`.

## §1. What `ControlsPanel` owns, and what it deliberately doesn't

`ControlsPanel`'s header states its own scope precisely: it "only parses
settings and forwards clicks to whichever callbacks the owner (AppDelegate)
sets."

```cpp
// image-width, samples-per-pixel, max-depth, and scene-seed fields (+ randomize), a CPU-threading
// toggle, and Render CPU / Render GPU / Compare buttons — per CLAUDE.md's UI architecture. Actual
// rendering stays outside this class: it only parses settings and forwards clicks to whichever
// callbacks the owner (AppDelegate) sets.
class ControlsPanel
```
`App/ControlsPanel.hpp:20-24`

That separation is why the class has no reference to `CPURenderer`,
`GPURenderer`, `RasterRenderer`, or `SceneExporter`/`SceneImporter` beyond the
`CPUThreading::Mode` enum it needs to fill in a `RenderSettings` struct — the
eight `std::function<void()>` members (three of them added after this
chapter was first written, as the app grew a raster renderer, Chapter 14's
pipeline-visualization window, and the "Load Scene" round-trip feature) are
the entire interface `AppDelegate` uses to react to a click:

```cpp
std::function<void()> onRenderCPU;
std::function<void()> onRenderGPU;
std::function<void()> onRenderRaster;
std::function<void()> onCompare;
std::function<void()> onShowPipeline;
std::function<void()> onSaveGLTF;
std::function<void()> onSaveOBJ;
std::function<void()> onLoadScene;
```
`App/ControlsPanel.hpp:42-49`

The struct those settings get read into mirrors exactly the fields the panel
exposes — four numeric text fields, a threading mode, and the floating
checkbox:

```cpp
struct RenderSettings
{
	uint32_t           width;
	uint32_t           samplesPerPixel;
	uint32_t           maxDepth;
	unsigned           seed;
	CPUThreading::Mode cpuMode;
	bool               floating; // "Floating?" checkbox — see Scene.hpp's buildDefaultScene()
};
```
`App/ControlsPanel.hpp:10-18`

## §2. Building the row of fields, and a 930-wide budget that outlived three later additions

`ControlsPanel`'s constructor lays out two rows of subviews inside the frame
it's given. Row 1 holds the four labeled numeric fields, a Randomize button,
and — added later, one at a time, as the app grew new render paths — three
more buttons squeezed into whatever room was left; row 2 holds the threading
toggle, the original three render buttons, the Floating? checkbox, and the
two Save buttons. The four fields are laid out first, each at a hand-picked
x-offset:

```cpp
_pContainerView->addSubview( makeLabel( ( CGRect ){ { 0.0, 32.0 }, { 45.0, 20.0 } }, "Width:" ) );
_pWidthField = makeField( ( CGRect ){ { 48.0, 32.0 }, { 55.0, 22.0 } }, "400" );
_pContainerView->addSubview( _pWidthField );

_pContainerView->addSubview( makeLabel( ( CGRect ){ { 115.0, 32.0 }, { 35.0, 20.0 } }, "SPP:" ) );
_pSppField = makeField( ( CGRect ){ { 153.0, 32.0 }, { 55.0, 22.0 } }, "20" );
_pContainerView->addSubview( _pSppField );

_pContainerView->addSubview( makeLabel( ( CGRect ){ { 220.0, 32.0 }, { 45.0, 20.0 } }, "Depth:" ) );
_pMaxDepthField = makeField( ( CGRect ){ { 268.0, 32.0 }, { 55.0, 22.0 } }, "20" );
_pContainerView->addSubview( _pMaxDepthField );

_pContainerView->addSubview( makeLabel( ( CGRect ){ { 335.0, 32.0 }, { 40.0, 20.0 } }, "Seed:" ) );
_pSeedField = makeField( ( CGRect ){ { 378.0, 32.0 }, { 80.0, 22.0 } }, "1234" );
_pContainerView->addSubview( _pSeedField );
```
`App/ControlsPanel.cpp:74-88`

Row 2 keeps growing rightward too: the threading toggle starts at x=0, the
three original render buttons follow, the Floating? checkbox sits at x=550,
and the two Save buttons land at x=680 and x=800, each 110pt wide plus a gap:

```cpp
_pSaveGLTFButton = NS::Button::alloc()->init( ( CGRect ){ { 680.0, 2.0 }, { 110.0, 26.0 } } );
_pSaveGLTFButton->setTitle( NS::String::string( "Save glTF", UTF8StringEncoding ) );
_pSaveGLTFButton->setTarget( _pSaveGLTFButton );
_pSaveGLTFButton->setAction( NS::MenuItem::registerActionCallback( "controlsSaveGLTFClicked", onSaveGLTFClicked ) );
_pContainerView->addSubview( _pSaveGLTFButton );

_pSaveOBJButton = NS::Button::alloc()->init( ( CGRect ){ { 800.0, 2.0 }, { 110.0, 26.0 } } );
_pSaveOBJButton->setTitle( NS::String::string( "Save OBJ", UTF8StringEncoding ) );
_pSaveOBJButton->setTarget( _pSaveOBJButton );
_pSaveOBJButton->setAction( NS::MenuItem::registerActionCallback( "controlsSaveOBJClicked", onSaveOBJClicked ) );
_pContainerView->addSubview( _pSaveOBJButton );
```
`App/ControlsPanel.cpp:152-162`

The Save OBJ button's right edge lands at x=910 — close enough to the
original 860pt-wide window that it would have been clipped or overlapped the
window's edge chrome. Per CLAUDE.md's project-status notes, adding the two
export buttons is exactly why "window widened from 860 to 950 to fit the
latter two" — a one-line consequence of this row's absolute-coordinate
layout that is otherwise invisible from `ControlsPanel.cpp` alone, since the
window's width is set in `AppDelegate`, not here. The two files have to
agree on geometry even though neither owns the other's numbers.

Row 2 never grew again after that — every button added since (`Load Scene`,
`Render Raster`, `Pipeline Steps`, §10) went to row 1 instead, each with a
comment explaining the same reasoning: row 1 had spare width row 2 didn't.

```cpp
// Row 1 has plenty of unused width to the right of Randomize (it only extends to x=555 out of
// this container's 930), so Load Scene lives here rather than crowding row 2's already-packed
// button row.
_pLoadSceneButton = NS::Button::alloc()->init( ( CGRect ){ { 570.0, 30.0 }, { 110.0, 26.0 } } );
```
`App/ControlsPanel.cpp:96-99`

Three additions later, row 1 itself finally ran out of room too: `Pipeline
Steps` (§10) had to be squeezed into a final 115pt gap at x=815, out of the
same 930pt budget — no window resize needed, but no further row-1 additions
would fit without one.

## §3. The CPU-threading toggle: a push button faking a checked state

Before the Floating? checkbox existed, the only two-state control in this
panel was the CPU-threading toggle, and it had to fake being one. It's a
plain `NS::Button` (the default AppKit button type, a push button) whose
*title* is overwritten on every click to describe the state that click just
switched *to*:

```cpp
_pThreadingToggleButton = NS::Button::alloc()->init( ( CGRect ){ { 0.0, 2.0 }, { 170.0, 26.0 } } );
_pThreadingToggleButton->setTitle( NS::String::string( "CPU Threads: Multi", UTF8StringEncoding ) );
_pThreadingToggleButton->setTarget( _pThreadingToggleButton );
_pThreadingToggleButton->setAction( NS::MenuItem::registerActionCallback( "controlsThreadingToggleClicked", onThreadingToggleClicked ) );
_pContainerView->addSubview( _pThreadingToggleButton );
```
`App/ControlsPanel.cpp:121-125`

The click handler does two things a real checkbox's click handler wouldn't
have to: flip a hand-maintained `bool`, and rewrite the button's title so the
label keeps matching that bool:

```cpp
// Flips the CPU-threading mode and updates the toggle button's title to match.
void ControlsPanel::handleThreadingToggleClicked()
{
	_multiThreaded = !_multiThreaded;
	_pThreadingToggleButton->setTitle( NS::String::string(
		_multiThreaded ? "CPU Threads: Multi" : "CPU Threads: Single",
		NS::StringEncoding::UTF8StringEncoding ) );
}
```
`App/ControlsPanel.cpp:231-238`

That `_multiThreaded` field is the class's only piece of state that isn't
read straight out of an AppKit control on demand:

```cpp
NS::Button* _pThreadingToggleButton;
bool        _multiThreaded;
```
`App/ControlsPanel.hpp:89-90`

`currentSettings()` has no choice but to consult it, since the button itself
has no notion of "checked":

```cpp
settings.cpuMode = _multiThreaded ? CPUThreading::MultiThreaded : CPUThreading::SingleThreaded;
```
`App/ControlsPanel.cpp:195`

This works, and it's what CLAUDE.md calls, in its own words, "the CPU-
threading toggle's older 'push button whose title changes' hack" — a hack in
the specific sense that the button's on-screen state (its title text) and
the panel's logical state (`_multiThreaded`) are two separate pieces of data
that a bug could let drift apart, kept in sync only because the one click
handler that changes either one changes both together.

## §4. The Floating? checkbox: letting AppKit own its own state

The Floating? checkbox is a genuine `NSButtonTypeSwitch` button — the AppKit
control macOS renders as a checkbox — created by setting the button type
after allocation and never touched again:

```cpp
// A real checkbox — AppKit manages its own checked state on click, so no target/action wiring
// is needed at all; currentSettings() just reads state() on demand.
_pFloatingCheckbox = NS::Button::alloc()->init( ( CGRect ){ { 550.0, 2.0 }, { 110.0, 26.0 } } );
_pFloatingCheckbox->setButtonType( NS::ButtonTypeSwitch );
_pFloatingCheckbox->setTitle( NS::String::string( "Floating?", UTF8StringEncoding ) );
_pContainerView->addSubview( _pFloatingCheckbox );
```
`App/ControlsPanel.cpp:145-150`

Notice everything that's *missing* compared to every other button in this
file: no `setTarget`, no `setAction`, no `registerActionCallback`, and no
entry in the trampoline table at the top of `ControlsPanel.cpp`. That
omission is the whole point. `setButtonType`/`setState`/`state()` are
project-local additions to `NSButton.hpp` (part of the metal-cpp-extensions
gap-filling story Chapter 10 covers in full), and their contract is that
AppKit itself flips the button's internal checked/unchecked state whenever
the user clicks it — the class comment on the header spells this out
directly:

```cpp
// A real NSButtonTypeSwitch checkbox rather than the threading toggle's "push button whose
// title changes" hack — AppKit manages its own checked/unchecked state on click, so this
// needs no click handler at all; currentSettings() just reads state() on demand.
NS::Button* _pFloatingCheckbox;
```
`App/ControlsPanel.hpp:92-95`

So where the threading toggle needs a `bool` field, a click handler, and a
title rewrite just to answer "am I on or off," the checkbox needs none of
that — `currentSettings()` just asks it:

```cpp
settings.floating = _pFloatingCheckbox->state() != 0;
```
`App/ControlsPanel.cpp:196`

`state()` sends the real `NSButton` `state` selector via
`Object::sendMessage`, returning `1` when checked and `0` when unchecked
(the sign convention documented directly on `NSButton.hpp`'s declaration:
"`state()` is 1 when checked, 0 when unchecked. AppKit toggles this itself on
click — no manual bookkeeping needed, unlike a plain push button" —
`ThirdParty/metal-cpp-extensions/AppKit/NSButton.hpp:39-43`). The two
controls sit right next to each other in the same source file, one hand-
rolling a piece of UI state AppKit already tracks internally, the other
reading that internal state directly — the checkbox is strictly less code and
strictly less room for the label and the boolean to disagree, which is
exactly the improvement CLAUDE.md credits it with.

## §5. `currentSettings()`: read once, synchronously, before any thread starts

`currentSettings()` itself is a small, ordinary function — four calls to a
local `parseUInt` helper, one enum lookup, one `state()` read — but *when*
it's called matters more than what it does. Every field is parsed with a
clamped fallback rather than allowed to throw or read garbage on bad input:

```cpp
// Parses a text field as an unsigned integer, clamped to [minVal, maxVal]; returns `fallback`
// if the field doesn't start with a valid number.
uint32_t parseUInt( NS::TextField* pField, uint32_t minVal, uint32_t maxVal, uint32_t fallback )
{
	const char* pText = pField->stringValue()->utf8String();
	char*       pEnd = nullptr;
	long        value = std::strtol( pText, &pEnd, 10 );
	if ( pEnd == pText )
		return fallback;
	if ( value < (long)minVal )
		value = minVal;
	if ( value > (long)maxVal )
		value = maxVal;
	return (uint32_t)value;
}
```
`App/ControlsPanel.cpp:24-38`

```cpp
RenderSettings ControlsPanel::currentSettings() const
{
	RenderSettings settings;
	settings.width = parseUInt( _pWidthField, 50, 2000, 400 );
	settings.samplesPerPixel = parseUInt( _pSppField, 1, 2000, 20 );
	settings.maxDepth = parseUInt( _pMaxDepthField, 1, 100, 20 );
	settings.seed = parseUInt( _pSeedField, 0, 0xFFFFFFFFu, 1234 );
	settings.cpuMode = _multiThreaded ? CPUThreading::MultiThreaded : CPUThreading::SingleThreaded;
	settings.floating = _pFloatingCheckbox->state() != 0;
	return settings;
}
```
`App/ControlsPanel.cpp:188-198`

Chapter 8 covers in full why `AppDelegate` always calls this on the main
thread, synchronously, before spawning the `std::thread` that actually
renders — briefly, the point is that every AppKit control (`NS::TextField`,
`NS::Button`) may only be touched from the main thread, so `currentSettings()`
takes its one and only snapshot of the UI's numeric/boolean state up front,
hands a plain `RenderSettings` value (no AppKit objects, safe to touch from
anywhere) into the background thread's capture list, and never gets called
again until that render finishes. `setControlsEnabled(false)` — disabling
every field and button for the render's duration — is the other half of that
same guarantee: it closes the window during which a second click could
change what `currentSettings()` would report, rather than trying to make
`currentSettings()` itself thread-safe.

```cpp
void ControlsPanel::setControlsEnabled( bool enabled )
{
	_pWidthField->setEnabled( enabled );
	_pSppField->setEnabled( enabled );
	_pMaxDepthField->setEnabled( enabled );
	_pSeedField->setEnabled( enabled );
	_pThreadingToggleButton->setEnabled( enabled );
	_pRandomizeSeedButton->setEnabled( enabled );
	_pRenderCPUButton->setEnabled( enabled );
	_pRenderGPUButton->setEnabled( enabled );
	_pRenderRasterButton->setEnabled( enabled );
	_pShowPipelineButton->setEnabled( enabled );
	_pCompareButton->setEnabled( enabled );
	_pFloatingCheckbox->setEnabled( enabled );
	_pSaveGLTFButton->setEnabled( enabled );
	_pSaveOBJButton->setEnabled( enabled );
	_pLoadSceneButton->setEnabled( enabled );
}
```
`App/ControlsPanel.cpp:202-219`

## §6. Randomize Seed: the one handler that needs no forwarding

Unlike the five render/export buttons, whose handlers are one-line
`std::function` forwards to whatever `AppDelegate` set, Randomize Seed is
handled entirely inside `ControlsPanel` — it never needs to leave this class,
since randomizing the seed field is pure UI state with no renderer involved:

```cpp
// Fills the seed field with a freshly generated random seed.
void ControlsPanel::handleRandomizeSeedClicked()
{
	std::random_device rd;
	unsigned           newSeed = rd();
	char               buf[ 16 ];
	std::snprintf( buf, sizeof( buf ), "%u", newSeed );
	_pSeedField->setStringValue( NS::String::string( buf, NS::StringEncoding::UTF8StringEncoding ) );
}
```
`App/ControlsPanel.cpp:221-229`

It draws straight from `std::random_device` rather than the seeded
`std::mt19937` `Scene::buildDefaultScene()` uses internally (Chapter 1) —
appropriately, since this call exists to *pick* a new seed for that
generator, not to be reproducible itself.

## §7. The trampoline idiom, briefly — full story in Chapter 10

Every clickable button in this file follows the same four-line pattern:
allocate, set a title, set the button as its own target, and register an
action callback through a file-local free function:

```cpp
_pRandomizeSeedButton = NS::Button::alloc()->init( ( CGRect ){ { 465.0, 30.0 }, { 90.0, 26.0 } } );
_pRandomizeSeedButton->setTitle( NS::String::string( "Randomize", UTF8StringEncoding ) );
_pRandomizeSeedButton->setTarget( _pRandomizeSeedButton );
_pRandomizeSeedButton->setAction( NS::MenuItem::registerActionCallback( "controlsRandomizeSeedClicked", onRandomizeSeedClicked ) );
_pContainerView->addSubview( _pRandomizeSeedButton );
```
`App/ControlsPanel.cpp:90-94`

`registerActionCallback` wants a capture-less function pointer, not a
lambda with captures, so `ControlsPanel.cpp` keeps a single file-local
pointer to the live panel instance and a small table of trampolines that
route each callback through it:

```cpp
namespace
{
	ControlsPanel* gControlsPanel = nullptr;

	// Button click trampolines: metal-cpp-extensions' action callbacks are capture-less function
	// pointers, so each reaches the current panel through the file-local gControlsPanel pointer.
	void onRandomizeSeedClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRandomizeSeedClicked(); }
	void onThreadingToggleClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleThreadingToggleClicked(); }
	void onRenderCPUClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRenderCPUClicked(); }
	void onRenderGPUClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRenderGPUClicked(); }
	void onRenderRasterClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleRenderRasterClicked(); }
	void onCompareClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleCompareClicked(); }
	void onShowPipelineClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleShowPipelineClicked(); }
	void onSaveGLTFClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleSaveGLTFClicked(); }
	void onSaveOBJClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleSaveOBJClicked(); }
	void onLoadSceneClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleLoadSceneClicked(); }
	...
}
```
`App/ControlsPanel.cpp:9-23`

This is the same idiom `AppDelegate` uses for its own menu-item callback
(`onShowAboutClicked` in `App/AppDelegate.cpp:29-33`) and it's how every
AppKit control callback in this project gets wired without Objective-C++ —
`NS::Control`/`NS::Button`'s `setTarget`/`setAction`, and
`NS::MenuItem::registerActionCallback` underneath them, ultimately rest on
`Object::sendMessage`/`class_addMethod`, the same mechanism `NSButton.hpp`
itself is built from (§4 above). *Why* Apple's vendored
`metal-cpp-extensions` needed `NS::Control`/`NS::Button` added at all, and how
`registerActionCallback` bridges a plain C++ function pointer into an
Objective-C selector, is Chapter 10's subject in full; this chapter only
notes the pattern as it's used here.

## §8. `ResultsPanel`: four labels, no arithmetic

`ResultsPanel`'s header is explicit that the class does no computation of
its own — the "three" it originally said became "four" the moment the
raster renderer (Chapter 13) needed a line of its own, but the "deliberately
dumb" design didn't change at all:

```cpp
// Four label lines (CPU / GPU / Raster / speedup) per CLAUDE.md's UI architecture. Deliberately
// dumb: it just displays whatever text it's given — all render-time/rays-per-sec/triangle-count/
// speedup formatting stays in AppDelegate, where the actual timing values are known.
class ResultsPanel
```
`App/ResultsPanel.hpp:7-10`

Its constructor places four plain (non-editable, non-bezeled) label fields
stacked vertically, each starting with placeholder text:

```cpp
_pCPULabel = makeLabel( ( CGRect ){ { 0.0, 68.0 }, { frame.size.width, 20.0 } }, "CPU: not yet run" );
_pContainerView->addSubview( _pCPULabel );

_pGPULabel = makeLabel( ( CGRect ){ { 0.0, 46.0 }, { frame.size.width, 20.0 } }, "GPU: not yet run" );
_pContainerView->addSubview( _pGPULabel );

_pRasterLabel = makeLabel( ( CGRect ){ { 0.0, 24.0 }, { frame.size.width, 20.0 } }, "Raster: not yet run" );
_pContainerView->addSubview( _pRasterLabel );

_pSpeedupLabel = makeLabel( ( CGRect ){ { 0.0, 2.0 }, { frame.size.width, 20.0 } }, "Run at least two of CPU/GPU/Raster to compare" );
_pContainerView->addSubview( _pSpeedupLabel );
```
`App/ResultsPanel.cpp:20-30`

and its entire public surface beyond that is four setters that do nothing
but forward a pre-formatted string into the matching label:

```cpp
void ResultsPanel::setCPULine( const std::string& text )
{
	_pCPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}
```
`App/ResultsPanel.cpp:44-47`

The actual numbers — render time, GPU-only time, estimated rays/sec (or, for
the raster line, a triangle count instead — Chapter 13, §2, since
rasterization has no "rays" to estimate a rate for), and the speedup line —
are computed in `AppDelegate`, which is the only place that has both a
`RenderParams` (width/height/samplesPerPixel) and a measured elapsed time in
hand at the same moment. Rays/sec is a single helper shared by both ray
tracers:

```cpp
// Estimates rays/sec as width*height*samplesPerPixel divided by elapsed time.
double raysPerSecond( const RenderParams& params, double milliseconds )
{
	double rays = (double)params.width * (double)params.height * (double)params.samplesPerPixel;
	return rays / ( milliseconds / 1000.0 );
}
```
`App/AppDelegate.cpp:22-27`

The speedup line started out simple — CPU and GPU were the only two
renderers, so there was only ever one ratio to report, whichever side was
faster. Adding a third renderer meant that simple two-way branch could no
longer say everything worth saying (once all three have run, there are three
pairwise ratios, not one), so `updateSpeedupIfPossible()` was generalized to
build a list of whichever timings are currently known and format every
pairwise ratio among them — while still producing *exactly* the old
single-pair message on a scene where only CPU and GPU have run, so this was
an additive change, not a breaking one:

```cpp
// Once at least two of CPU/GPU/Raster timings are known, formats and displays every pairwise ratio
// among whichever are known (1 pair today, up to 3 once all three have run) — e.g. "GPU is 152.3x
// faster than CPU | Raster is 9.8x faster than GPU | Raster is 1492.1x faster than CPU". No-ops
// until at least two times exist.
void AppDelegate::updateSpeedupIfPossible()
{
	struct Timing { const char* name; double ms; };
	std::vector<Timing> known;
	if ( _lastCPUTimeMs >= 0.0 )
		known.push_back( { "CPU", _lastCPUTimeMs } );
	if ( _lastGPUTimeMs >= 0.0 )
		known.push_back( { "GPU", _lastGPUTimeMs } );
	if ( _lastRasterTimeMs >= 0.0 )
		known.push_back( { "Raster", _lastRasterTimeMs } );

	if ( known.size() < 2 )
		return;

	std::string line;
	for ( size_t i = 0; i < known.size(); ++i )
		for ( size_t j = i + 1; j < known.size(); ++j )
		{
			if ( !line.empty() )
				line += " | ";
			line += formatSpeedup( known[ i ].name, known[ i ].ms, known[ j ].name, known[ j ].ms );
		}

	_pResultsPanel->setSpeedupLine( line );
}
```
`App/AppDelegate.cpp:349-377`

The actual "which one's faster, and by how much" arithmetic for one pair was
pulled out into its own tiny helper, so the nested loop above doesn't repeat
it once per pair:

```cpp
// Formats "<A> is Nx faster than <B>" for one pair of timings (in ms) — a small helper so
// updateSpeedupIfPossible() below doesn't repeat this once per pair.
std::string formatSpeedup( const char* nameA, double msA, const char* nameB, double msB )
{
	char buf[ 96 ];
	if ( msA < msB )
		std::snprintf( buf, sizeof( buf ), "%s is %.1fx faster than %s", nameA, msB / msA, nameB );
	else
		std::snprintf( buf, sizeof( buf ), "%s is %.1fx faster than %s", nameB, msA / msB, nameA );
	return buf;
}
```
`App/AppDelegate.cpp:41-51`

This division of labor mirrors `ControlsPanel`'s: the view-owning class
knows only how to display text in a labeled field, and the class that owns
the actual renderers and timers (`AppDelegate`, Chapter 8) is the only one
that touches `RenderParams` or a stopwatch value.

## §9. `AboutAlert`: attribution, via the project-local `NSAlert.hpp`

`AboutAlert` is the smallest file in this chapter — one free function,
`showAboutAlert()`, that builds and runs an `NS::Alert` modally:

```cpp
void showAboutAlert()
{
	using NS::StringEncoding::UTF8StringEncoding;

	NS::Alert* pAlert = NS::Alert::alloc()->init();
	pAlert->setMessageText( NS::String::string( "RayTracerBench", UTF8StringEncoding ) );
	pAlert->setInformativeText( NS::String::string(
		"A CPU-vs-GPU (Metal compute) ray tracing benchmark, built in pure C++ and Metal via "
		"Apple's metal-cpp/metal-cpp-extensions.\n\n"
		"Based on the algorithm and structure of \"Ray Tracing in One Weekend\" by Peter Shirley "
		"et al. (raytracing.github.io, CC0) and its CUDA port by Roger Allen "
		"(github.com/rogerallen/raytracinginoneweekendincuda, public domain). Neither license "
		"requires attribution; this note is a courtesy credit, not a legal one.",
		UTF8StringEncoding ) );
	pAlert->runModal();
}
```
`App/AboutAlert.cpp:6-21`

Its header explains why the note exists at all even though nothing legally
requires it:

```cpp
// An NS::Alert carrying the courtesy attribution note this project's reference sources don't
// legally require (both are CC0/public-domain) but credits anyway. See CLAUDE.md's Attribution
// requirements section.
void showAboutAlert();
```
`App/AboutAlert.hpp:3-6`

That matches CLAUDE.md's attribution requirements directly: both reference
repositories (`RayTracing/raytracing.github.io` and
`rogerallen/raytracinginoneweekendincuda`) are CC0/public-domain and impose
no legal obligation, but the project still credits both here (and in
`README.md`), alongside an MIT license listing Pablo Figueroa and Claude as
authors. `NS::Alert` itself — like `NS::Button` and `NS::TextField` used
throughout this chapter — is a project-local addition to
`metal-cpp-extensions` built with the same `Object::sendMessage`/
`class_addMethod` idiom as §7's button trampolines, since Apple's vendored
headers have no alert wrapper either; that gap-filling story belongs to
Chapter 10.

## §10. Three later additions: Load Scene, Render Raster, Pipeline Steps

Three buttons were added to row 1 after this chapter was first written, each
following the exact same four-line allocate/title/target/action pattern §7
already covers, but each worth a sentence for *why* it exists and what it
sits next to.

`Load Scene` shows a real `NS::OpenPanel` — a project-local addition to
`metal-cpp-extensions`, since Apple's vendored headers have no open-panel
wrapper any more than they have an alert or button one (§9, Chapter 10) —
then hands the picked path to `Export/SceneImporter.hpp` (Chapter 7, §11) on
a background thread:

```cpp
// Row 1 has plenty of unused width to the right of Randomize (it only extends to x=555 out of
// this container's 930), so Load Scene lives here rather than crowding row 2's already-packed
// button row.
_pLoadSceneButton = NS::Button::alloc()->init( ( CGRect ){ { 570.0, 30.0 }, { 110.0, 26.0 } } );
_pLoadSceneButton->setTitle( NS::String::string( "Load Scene", UTF8StringEncoding ) );
_pLoadSceneButton->setTarget( _pLoadSceneButton );
_pLoadSceneButton->setAction( NS::MenuItem::registerActionCallback( "controlsLoadSceneClicked", onLoadSceneClicked ) );
_pContainerView->addSubview( _pLoadSceneButton );
```
`App/ControlsPanel.cpp:96-103`

`NS::OpenPanel` itself is deliberately minimal — just enough to pick one
file and read back its URL, with no content-type filter wrapper, since this
app tells `.gltf` from `.obj` apart by the chosen file's own extension
afterward rather than restricting the panel itself:

```cpp
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
```
`ThirdParty/metal-cpp-extensions/AppKit/NSOpenPanel.hpp:32-49`

`Render Raster` and `Pipeline Steps` need no new AppKit wrapper at all — both
just forward through the same `std::function` pattern every other button
uses (§1) — but they mark two successive points where row 1 ran out of the
"plenty of unused width" the `Load Scene` comment above once had:

```cpp
// Same reasoning as Load Scene just above: row 1 still has room (this ends at x=810, out of
// this container's 930) while row 2 is already packed with the render/save/floating controls.
_pRenderRasterButton = NS::Button::alloc()->init( ( CGRect ){ { 700.0, 30.0 }, { 110.0, 26.0 } } );
```
`App/ControlsPanel.cpp:105-107`

```cpp
// Same row-1 reasoning as Load Scene/Render Raster above — the last remaining gap on row 1.
_pShowPipelineButton = NS::Button::alloc()->init( ( CGRect ){ { 815.0, 30.0 }, { 115.0, 26.0 } } );
```
`App/ControlsPanel.cpp:113-114`

Each comment explicitly measures how much of the 930pt row-1 budget is left
before deciding the new button still fits without a window resize — the same
discipline §2's `Load Scene` story already established, applied twice more
until the row was genuinely full.

## Where this connects

- **Chapter 8** (`main.cpp`, `App/AppDelegate.hpp/.cpp`) owns the instances
  of `ControlsPanel` and `ResultsPanel` this chapter describes, wires all
  eight `std::function` callbacks (`onRenderCPU`, `onRenderGPU`,
  `onRenderRaster`, `onCompare`, `onShowPipeline`, `onSaveGLTF`, `onSaveOBJ`,
  `onLoadScene`) to its own `start*`/`saveScene`/`loadScene`/
  `showPipelineWindow` methods, is the sole caller of `currentSettings()`
  (always synchronously, on the main thread, before spawning the background
  `std::thread` that actually renders — §5), and computes every number
  `ResultsPanel`'s setters merely display (§8).
- **Chapter 10** (`App/ImageDisplayView.hpp/.cpp`, plus the
  `ThirdParty/metal-cpp-extensions` additions) tells the full story behind
  the `NS::Button`/`NS::TextField`/`NS::Alert`/`NS::Control`/`NS::OpenPanel`
  additions this chapter's panels are built entirely out of — the
  `Object::sendMessage`/`class_addMethod` idiom introduced only briefly here
  in §4, §7, and §10.
- **Chapter 1** (`Core/Scene.hpp/.cpp`) is where the Floating? checkbox's
  `bool` actually does something: `settings.floating` (§4) flows into
  `Scene::buildDefaultScene()`'s `floating` parameter, which places the
  small randomized-field spheres at a random height instead of resting them
  on the ground.
- **Chapter 7** (`Export/SceneImporter.hpp/.cpp`) is what `Load Scene` (§10)
  actually hands the picked file to, and the round-trip contract (this app's
  own exports only) that makes reconstruction exact.
- **Chapter 13** (`GPU/RasterRenderer.hpp/.cpp`) is what `Render Raster`
  (§10) and the generalized every-pairwise-ratio speedup line (§8) exist
  to expose — the third renderer whose `triangleCount`, not a rays/sec
  figure, `ResultsPanel`'s raster line displays.
- **Chapter 14** (`App/PipelineVisualizationWindow.hpp/.cpp`,
  `GPU/PipelineStageRenderer.hpp/.cpp`) is the secondary window `Pipeline
  Steps` (§10) opens.
