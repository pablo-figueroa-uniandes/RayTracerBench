#pragma once

#include "../Core/Scene.hpp"

#include <string>
#include <vector>

// Result of an export attempt: `ok` plus a human-readable summary (a success message naming the
// written path(s), or an error) and the absolute path(s) actually written.
struct SceneExportResult
{
	bool                     ok;
	std::string              message;
	std::vector<std::string> writtenFilePaths;
};

// Absolute path to the directory containing the currently-running executable, resolved via
// _NSGetExecutablePath (not argv[0] or getcwd() — this app can be launched from any working
// directory, e.g. Xcode's "Product > Run" or a Finder double-click).
std::string executableDirectory();

// A filename stem encoding the scene-identifying parameters — seed, image width (which feeds the
// camera's aspect ratio), and the Floating? state — plus a timestamp, so re-exporting identical
// settings doesn't silently overwrite a prior export. samplesPerPixel/maxDepth are deliberately
// excluded: they affect only rendering, not the exported geometry itself.
std::string sceneFilenameStem( unsigned seed, uint32_t width, bool floating );

// Writes `scene` as a single self-contained .gltf file (geometry/materials embedded as a base64
// data URI buffer, no companion .bin) into <executableDirectory()>/SavedScenes/<stem>.gltf.
SceneExportResult exportSceneAsGLTF( const SceneDescription& scene, const std::string& stem );

// Writes `scene` as a <stem>.obj + <stem>.mtl pair into <executableDirectory()>/SavedScenes/.
SceneExportResult exportSceneAsOBJ( const SceneDescription& scene, const std::string& stem );
