#pragma once
#include <filesystem>
#include <memory>
#include <string>

class IGameModule;

// Loads a game module DLL for the editor. It never loads the built DLL itself: it
// loads a copy (with its PDB) from `<Saved>/Modules/<pid>-<n>/`, so the build can
// overwrite the original while the editor runs, and a new build can be swapped in.
//
// Game modules link with /PDBALTPATH:<name>.pdb, so the debugger looks for the PDB next
// to the copy instead of locking the one the linker writes.
class ModuleHost
{
public:
	ModuleHost();
	~ModuleHost();
	ModuleHost(const ModuleHost&) = delete;
	ModuleHost& operator=(const ModuleHost&) = delete;

	// Deletes old copies under `shadowRoot` (copies still loaded by another editor stay).
	static void CleanShadowRoot(const std::filesystem::path& shadowRoot);

	// Copies `sourceDll` into `shadowRoot`, loads the copy and creates its game module.
	bool Load(const std::filesystem::path& sourceDll, const std::filesystem::path& shadowRoot, std::string& error);
	// Destroys the game module and unloads the DLL.
	void Unload();

	IGameModule* Game() const;
	bool IsLoaded() const;
	const std::filesystem::path& SourcePath() const;

	// Polled by the engine (every few frames). True once the source DLL has changed since
	// it was loaded (or appeared, if it didn't exist) and the linker has finished writing it.
	bool HasNewBuild();

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
