#pragma once
#include "CherryAPI.h"
#include <filesystem>
#include <string>
#include <vector>

// A game project: one game module plus its content, described by a `.cherryproject`
// JSON file in the project's root folder:
//
//   { "name": "UNDERTIME", "module": "UndertimeGame", "content": "Content" }
//
// The editor opens one project at a time. Per-project editor state (window layout,
// loaded module copies) lives in <root>/Saved/.
struct CHERRY_API ProjectDesc
{
	std::string name;                  // shown in the window title and project lists
	std::string module;                // game module target name, without ".dll"
	std::filesystem::path file;        // the .cherryproject file (empty for a project made up at runtime)
	std::filesystem::path root;        // folder holding the project file
	std::filesystem::path content;     // absolute Content folder
	std::filesystem::path saved;       // absolute Saved folder (editor state, never checked in)
	std::string startupScene;          // relative to root; unused until scenes exist

	bool IsValid() const { return !module.empty(); }
};

namespace cherry
{
	// Reads a .cherryproject file. Relative paths in it are resolved against its folder.
	CHERRY_API bool LoadProjectFile(const std::filesystem::path& file, ProjectDesc& out, std::string& error);

	// Every .cherryproject one folder below the source tree (e.g. Undertime/Undertime.cherryproject).
	// Empty when the engine wasn't built from a source tree.
	CHERRY_API std::vector<ProjectDesc> FindKnownProjects();

	// Most recently opened first. Stored in %LOCALAPPDATA%/CherryEngine/RecentProjects.txt.
	CHERRY_API std::vector<std::filesystem::path> LoadRecentProjects();
	CHERRY_API void AddRecentProject(const std::filesystem::path& file);

	// %LOCALAPPDATA%/CherryEngine (created on demand): editor state when no project is open.
	CHERRY_API std::filesystem::path EngineUserDir();

	// The folder a packaged game's files sit in (next to the executable on Windows).
	CHERRY_API std::filesystem::path ExecutableDir();
#if !defined(_WIN32)
	// Android: the app's data folder, where its packaged files were unpacked. Set before Run.
	CHERRY_API void SetExecutableDir(const std::filesystem::path& dir);
#endif
}
