#include "AboutAlert.hpp"

#include <AppKit/AppKit.hpp>

// Builds and runs (modally) the About panel's NS::Alert. See AboutAlert.hpp for the rationale.
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
