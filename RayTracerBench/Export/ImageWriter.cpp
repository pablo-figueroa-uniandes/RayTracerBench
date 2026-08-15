#include "ImageWriter.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

// Wraps rgba in a CGImage (no pixel copy — CGDataProviderCreateWithData just borrows the pointer
// for the duration of this call) and asks ImageIO to encode and write it as PNG. "public.png" is
// passed as a literal UTI string rather than linking CoreServices/UniformTypeIdentifiers just for
// the kUTTypePNG constant.
bool writePNG( const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height )
{
	CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider = CGDataProviderCreateWithData( nullptr, rgba, (size_t)width * height * 4, nullptr );

	CGImageRef image = CGImageCreate( width, height, 8, 32, (size_t)width * 4, colorSpace,
		kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, provider, nullptr, false, kCGRenderingIntentDefault );

	CGDataProviderRelease( provider );
	CGColorSpaceRelease( colorSpace );

	if ( !image )
		return false;

	CFURLRef url = CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false );
	CGImageDestinationRef destination = CGImageDestinationCreateWithURL( url, CFSTR( "public.png" ), 1, nullptr );
	CFRelease( url );

	if ( !destination )
	{
		CGImageRelease( image );
		return false;
	}

	CGImageDestinationAddImage( destination, image, nullptr );
	bool ok = CGImageDestinationFinalize( destination );

	CFRelease( destination );
	CGImageRelease( image );

	return ok;
}
