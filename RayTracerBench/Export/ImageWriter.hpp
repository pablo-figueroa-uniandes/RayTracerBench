#pragma once

#include <cstdint>
#include <string>

// Writes an RGBA8, row-major, row-0-is-top pixel buffer (the same layout CPURenderer.hpp
// produces) to `path` as a PNG file, via CoreGraphics/ImageIO — real system C APIs, so this needs
// no third-party image library and no Objective-C. Returns false (rather than aborting) on
// failure, since a failed preview image shouldn't prevent the 3D export it accompanies from being
// reported as successful.
bool writePNG( const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height );
