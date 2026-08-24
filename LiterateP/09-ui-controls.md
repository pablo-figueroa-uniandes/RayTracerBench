# Chapter 9: UI Controls and Results

**Abstract.** This chapter covers the three files that make up RayTracerBench's
control surface: `ControlsPanel`, which owns every input field, toggle,
checkbox, and button the user touches to configure and trigger a render or
export; `ResultsPanel`, the deliberately dumb three-line label strip that
displays whatever timing text `AppDelegate` hands it; and `AboutAlert`, the
one-shot attribution dialog. None of these files render anything or know how
long a render took — they parse input, forward clicks through
`std::function` callbacks, and display strings. The interesting engineering
in this chapter is not in what these classes compute (almost nothing) but in
how they talk to AppKit without a line of Objective-C++: capture-less
function-pointer trampolines standing in for target/action selectors, and a
real `NSButtonTypeSwitch` checkbox that needs no click handler at all,
contrasted against an older toggle button that has to fake its own checked
state. Files covered: `App/ControlsPanel.hpp`, `App/ControlsPanel.cpp`,
`App/ResultsPanel.hpp`, `App/ResultsPanel.cpp`, `App/AboutAlert.hpp`,
`App/AboutAlert.cpp`.

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
`GPURenderer`, or `SceneExporter` beyond the `CPUThreading::Mode` enum it
needs to fill in a `RenderSettings` struct — the five `std::function<void()>`
members are the entire interface `AppDelegate` uses to react to a click:

```cpp
std::function<void()> onRenderCPU;
std::function<void()> onRenderGPU;
std::function<void()> onCompare;
std::function<void()> onSaveGLTF;
std::function<void()> onSaveOBJ;
```
`App/ControlsPanel.hpp:42-46`

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

## §2. Building the row of fields, and the 860→950 window widening

`ControlsPanel`'s constructor lays out two rows of subviews inside the frame
it's given: the top row holds the four labeled numeric fields plus a
Randomize button, and the bottom row holds the threading toggle, the three
render buttons, the Floating? checkbox, and the two Save buttons, each
placed at a hand-picked x-offset:

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
`App/ControlsPanel.cpp:71-85`

The bottom row keeps growing rightward as the app's feature set grew: the
threading toggle starts at x=0, the three render buttons follow, the
Floating? checkbox sits at x=550, and the two Save buttons land at x=680 and
x=800, each 110pt wide plus a gap:

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
`App/ControlsPanel.cpp:125-135`

The Save OBJ button's right edge lands at x=910 — close enough to the
original 860pt-wide window that it would have been clipped or overlapped the
window's edge chrome. Per CLAUDE.md's project-status notes, adding the two
export buttons is exactly why "window widened from 860 to 950 to fit the
latter two" — a one-line consequence of §2's absolute-coordinate layout that
is otherwise invisible from `ControlsPanel.cpp` alone, since the window's
width is set in `AppDelegate`, not here. The two files have to agree on
geometry even though neither owns the other's numbers.

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
`App/ControlsPanel.cpp:94-98`

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
`App/ControlsPanel.cpp:198-205`

That `_multiThreaded` field is the class's only piece of state that isn't
read straight out of an AppKit control on demand:

```cpp
NS::Button* _pThreadingToggleButton;
bool        _multiThreaded;
```
`App/ControlsPanel.hpp:80-81`

`currentSettings()` has no choice but to consult it, since the button itself
has no notion of "checked":

```cpp
settings.cpuMode = _multiThreaded ? CPUThreading::MultiThreaded : CPUThreading::SingleThreaded;
```
`App/ControlsPanel.cpp:165`

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
`App/ControlsPanel.cpp:118-123`

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
`App/ControlsPanel.hpp:83-86`

So where the threading toggle needs a `bool` field, a click handler, and a
title rewrite just to answer "am I on or off," the checkbox needs none of
that — `currentSettings()` just asks it:

```cpp
settings.floating = _pFloatingCheckbox->state() != 0;
```
`App/ControlsPanel.cpp:166`

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
`App/ControlsPanel.cpp:21-35`

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
`App/ControlsPanel.cpp:158-168`

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
	_pCompareButton->setEnabled( enabled );
	_pFloatingCheckbox->setEnabled( enabled );
	_pSaveGLTFButton->setEnabled( enabled );
	_pSaveOBJButton->setEnabled( enabled );
}
```
`App/ControlsPanel.cpp:172-186`

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
`App/ControlsPanel.cpp:189-196`

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
`App/ControlsPanel.cpp:87-91`

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
	void onCompareClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleCompareClicked(); }
	void onSaveGLTFClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleSaveGLTFClicked(); }
	void onSaveOBJClicked( void*, SEL, const NS::Object* ) { gControlsPanel->handleSaveOBJClicked(); }
	...
}
```
`App/ControlsPanel.cpp:7-19`

This is the same idiom `AppDelegate` uses for its own menu-item callback
(`onShowAboutClicked` in `App/AppDelegate.cpp:27-30`) and it's how every
AppKit control callback in this project gets wired without Objective-C++ —
`NS::Control`/`NS::Button`'s `setTarget`/`setAction`, and
`NS::MenuItem::registerActionCallback` underneath them, ultimately rest on
`Object::sendMessage`/`class_addMethod`, the same mechanism `NSButton.hpp`
itself is built from (§4 above). *Why* Apple's vendored
`metal-cpp-extensions` needed `NS::Control`/`NS::Button` added at all, and how
`registerActionCallback` bridges a plain C++ function pointer into an
Objective-C selector, is Chapter 10's subject in full; this chapter only
notes the pattern as it's used here.

## §8. `ResultsPanel`: three labels, no arithmetic

`ResultsPanel`'s header is explicit that the class does no computation of
its own:

```cpp
// Three label lines (CPU / GPU / speedup) per CLAUDE.md's UI architecture. Deliberately dumb: it
// just displays whatever text it's given — all render-time/rays-per-sec/speedup formatting stays
// in AppDelegate, where the actual timing values are known.
class ResultsPanel
```
`App/ResultsPanel.hpp:7-10`

Its constructor places three plain (non-editable, non-bezeled) label fields
stacked vertically, each starting with placeholder text:

```cpp
_pCPULabel = makeLabel( ( CGRect ){ { 0.0, 46.0 }, { frame.size.width, 20.0 } }, "CPU: not yet run" );
_pContainerView->addSubview( _pCPULabel );

_pGPULabel = makeLabel( ( CGRect ){ { 0.0, 24.0 }, { frame.size.width, 20.0 } }, "GPU: not yet run" );
_pContainerView->addSubview( _pGPULabel );

_pSpeedupLabel = makeLabel( ( CGRect ){ { 0.0, 2.0 }, { frame.size.width, 20.0 } }, "Run both CPU and GPU to compare" );
_pContainerView->addSubview( _pSpeedupLabel );
```
`App/ResultsPanel.cpp:19-26`

and its entire public surface beyond that is three setters that do nothing
but forward a pre-formatted string into the matching label:

```cpp
void ResultsPanel::setCPULine( const std::string& text )
{
	_pCPULabel->setStringValue( NS::String::string( text.c_str(), NS::StringEncoding::UTF8StringEncoding ) );
}
```
`App/ResultsPanel.cpp:45-48`

The actual numbers — render time, GPU-only time, estimated rays/sec, and the
labeled speedup ratio — are computed in `AppDelegate`, which is the only
place that has both a `RenderParams` (width/height/samplesPerPixel) and a
measured elapsed time in hand at the same moment. Rays/sec is a single
helper shared by every render path:

```cpp
// Estimates rays/sec as width*height*samplesPerPixel divided by elapsed time.
double raysPerSecond( const RenderParams& params, double milliseconds )
{
	double rays = (double)params.width * (double)params.height * (double)params.samplesPerPixel;
	return rays / ( milliseconds / 1000.0 );
}
```
`App/AppDelegate.cpp:19-24`

and the speedup line is only ever produced once both a CPU and a GPU timing
exist, formatted as whichever side is faster:

```cpp
// Once both a CPU and a GPU timing are known, formats and displays whichever ratio is >= 1x
// ("GPU is Nx faster" or "CPU is Nx faster"). No-ops until both times exist.
void AppDelegate::updateSpeedupIfPossible()
{
	if ( _lastCPUTimeMs < 0.0 || _lastGPUTimeMs < 0.0 )
		return;

	char buf[ 128 ];
	if ( _lastGPUTimeMs < _lastCPUTimeMs )
	{
		double ratio = _lastCPUTimeMs / _lastGPUTimeMs;
		std::snprintf( buf, sizeof( buf ), "GPU is %.1fx faster than CPU", ratio );
	}
	else
	{
		double ratio = _lastGPUTimeMs / _lastCPUTimeMs;
		std::snprintf( buf, sizeof( buf ), "CPU is %.1fx faster than GPU", ratio );
	}
	_pResultsPanel->setSpeedupLine( buf );
}
```
`App/AppDelegate.cpp:277-294`

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

## Where this connects

- **Chapter 8** (`main.cpp`, `App/AppDelegate.hpp/.cpp`) owns the instances
  of `ControlsPanel` and `ResultsPanel` this chapter describes, wires the
  five `std::function` callbacks (`onRenderCPU`, `onRenderGPU`, `onCompare`,
  `onSaveGLTF`, `onSaveOBJ`) to its own `start*`/`saveScene` methods, is the
  sole caller of `currentSettings()` (always synchronously, on the main
  thread, before spawning the background `std::thread` that actually
  renders — §5), and computes every number `ResultsPanel`'s setters merely
  display (§8).
- **Chapter 10** (`App/ImageDisplayView.hpp/.cpp`, plus the
  `ThirdParty/metal-cpp-extensions` additions) tells the full story behind
  the `NS::Button`/`NS::TextField`/`NS::Alert`/`NS::Control` additions this
  chapter's panels are built entirely out of — the `Object::sendMessage`/
  `class_addMethod` idiom introduced only briefly here in §4 and §7.
- **Chapter 1** (`Core/Scene.hpp/.cpp`) is where the Floating? checkbox's
  `bool` actually does something: `settings.floating` (§4) flows into
  `Scene::buildDefaultScene()`'s `floating` parameter, which places the
  small randomized-field spheres at a random height instead of resting them
  on the ground.
