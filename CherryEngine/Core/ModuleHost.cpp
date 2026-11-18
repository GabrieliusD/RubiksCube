#include "Core/ModuleHost.h"
#include "Core/Log.h"
#include "Game/GameModule.h"
#include <windows.h>
#include <chrono>

namespace fs = std::filesystem;

namespace
{
	using FileTime = fs::file_time_type;

	bool GetWriteTime(const fs::path& path, FileTime& out)
	{
		std::error_code ec;
		out = fs::last_write_time(path, ec);
		return !ec;
	}

	// The linker holds the DLL open for writing until it's done.
	bool IsFinishedWriting(const fs::path& path)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		CloseHandle(file);
		return true;
	}
}

struct ModuleHost::Impl
{
	HMODULE module = nullptr;
	std::unique_ptr<IGameModule> game;
	fs::path source;
	fs::path copyDir;
	bool loadedTimeValid = false;
	FileTime loadedTime{};       // write time of the source when it was copied

	// Change detection: a new write time must stay the same for a moment before it counts.
	bool pendingValid = false;
	FileTime pendingTime{};
	std::chrono::steady_clock::time_point pendingSince{};
	uint32_t copyCounter = 0;
};

ModuleHost::ModuleHost() : mImpl(std::make_unique<Impl>()) {}
ModuleHost::~ModuleHost() { Unload(); }

IGameModule* ModuleHost::Game() const { return mImpl->game.get(); }
bool ModuleHost::IsLoaded() const { return mImpl->game != nullptr; }
const fs::path& ModuleHost::SourcePath() const { return mImpl->source; }

void ModuleHost::CleanShadowRoot(const fs::path& shadowRoot)
{
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::directory_iterator(shadowRoot, ec))
	{
		std::error_code removeError;
		fs::remove_all(entry.path(), removeError);   // fails (and is skipped) for copies in use
	}
}

bool ModuleHost::Load(const fs::path& sourceDll, const fs::path& shadowRoot, std::string& error)
{
	Impl& m = *mImpl;
	Unload();
	m.source = sourceDll;
	m.pendingValid = false;
	m.loadedTimeValid = GetWriteTime(sourceDll, m.loadedTime);
	if (!m.loadedTimeValid)
	{
		error = sourceDll.filename().string() + " is not built yet";
		return false;
	}

	std::error_code ec;
	m.copyDir = shadowRoot / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++m.copyCounter));
	fs::create_directories(m.copyDir, ec);
	const fs::path copy = m.copyDir / sourceDll.filename();
	if (!fs::copy_file(sourceDll, copy, fs::copy_options::overwrite_existing, ec))
	{
		error = "Can't copy " + sourceDll.filename().string() + ": " + ec.message();
		return false;
	}
	fs::path pdb = sourceDll;
	pdb.replace_extension(".pdb");
	if (fs::exists(pdb, ec))
	{
		fs::path pdbCopy = copy;
		pdbCopy.replace_extension(".pdb");
		fs::copy_file(pdb, pdbCopy, fs::copy_options::overwrite_existing, ec);   // optional: debugging only
	}

	m.module = LoadLibraryW(copy.c_str());
	if (!m.module)
	{
		error = "LoadLibrary failed for " + copy.string() + " (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	auto create = reinterpret_cast<CreateGameModuleFn>(GetProcAddress(m.module, "CreateGameModule"));
	if (!create)
	{
		error = "CreateGameModule is not exported by " + sourceDll.filename().string();
		Unload();
		return false;
	}
	m.game.reset(create());
	if (!m.game)
	{
		error = "CreateGameModule returned null";
		Unload();
		return false;
	}
	CHERRY_LOG("Loaded game module %s (copy in %s)", sourceDll.filename().string().c_str(), m.copyDir.string().c_str());
	return true;
}

void ModuleHost::Unload()
{
	Impl& m = *mImpl;
	m.game.reset();   // the destructor lives in the DLL: delete before unloading it
	if (m.module)
	{
		FreeLibrary(m.module);
		m.module = nullptr;
		std::error_code ec;
		fs::remove_all(m.copyDir, ec);
	}
}

bool ModuleHost::HasNewBuild()
{
	Impl& m = *mImpl;
	if (m.source.empty()) return false;

	FileTime now;
	if (!GetWriteTime(m.source, now))
	{
		m.pendingValid = false;
		return false;
	}
	if (m.loadedTimeValid && now == m.loadedTime)
	{
		m.pendingValid = false;
		return false;
	}

	const auto clockNow = std::chrono::steady_clock::now();
	if (!m.pendingValid || now != m.pendingTime)
	{
		m.pendingValid = true;
		m.pendingTime = now;
		m.pendingSince = clockNow;
		return false;
	}
	return clockNow - m.pendingSince > std::chrono::milliseconds(400) && IsFinishedWriting(m.source);
}
