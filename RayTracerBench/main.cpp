// Metal/QuartzCore's own private-implementation macros (MTL_PRIVATE_IMPLEMENTATION,
// CA_PRIVATE_IMPLEMENTATION) live in ImageDisplayView.cpp instead, since that's the one
// translation unit that actually includes those headers — each must be defined exactly once,
// in whichever .cpp first includes the corresponding header.
#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>

#include "App/AppDelegate.hpp"

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
