#pragma once

#include <cstdint>
#include <string>
#include <vector>

// RFC 4648 standard base64 — used by the glTF exporter/importer to embed/recover the geometry
// buffer as a self-contained data: URI (no companion .bin file). Shared between SceneExporter.cpp
// and SceneImporter.cpp rather than duplicated, since encode/decode need to agree byte-for-byte.
std::string base64Encode( const uint8_t* data, size_t len );

// Decodes a base64 string back to bytes. Tolerant of '=' padding (required, per RFC 4648, and
// always present in what base64Encode produces) but not of embedded whitespace/newlines — never
// present in this project's own single-line data: URIs, so not worth handling.
std::vector<uint8_t> base64Decode( const std::string& text );
