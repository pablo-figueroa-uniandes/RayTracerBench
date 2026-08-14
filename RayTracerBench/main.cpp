#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
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
