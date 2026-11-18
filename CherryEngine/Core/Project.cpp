#include "Core/Project.h"
#include "Core/Log.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cherry
{
	namespace
	{
		constexpr size_t kMaxRecentProjects = 10;

		fs::path RecentProjectsFile() { return EngineUserDir() / "RecentProjects.txt"; }

		bool SamePath(const fs::path& a, const fs::path& b)
		{
			std::error_code ec;
			if (fs::exists(a, ec) && fs::exists(b, ec)) return fs::equivalent(a, b, ec);
			return a.lexically_normal() == b.lexically_normal();
		}
	}

	fs::path ExecutableDir()
	{
		wchar_t buffer[MAX_PATH];
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return fs::path(buffer).parent_path();
	}

	fs::path EngineUserDir()
	{
		wchar_t buffer[MAX_PATH];
		const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
		fs::path dir = (length > 0 && length < MAX_PATH) ? fs::path(buffer) / "CherryEngine" : ExecutableDir() / "Saved";
		std::error_code ec;
		fs::create_directories(dir, ec);
		return dir;
	}

	bool LoadProjectFile(const fs::path& file, ProjectDesc& out, std::string& error)
	{
		std::ifstream stream(file);
		if (!stream)
		{
			error = "Can't open " + file.string();
			return false;
		}

		nlohmann::json json;
		try
		{
			stream >> json;
		}
		catch (const std::exception& ex)
		{
			error = file.filename().string() + ": " + ex.what();
			return false;
		}

		ProjectDesc project;
		project.file = fs::absolute(file).lexically_normal();
		project.root = project.file.parent_path();
		project.name = json.value("name", project.file.stem().string());
		project.module = json.value("module", std::string());
		project.content = project.root / fs::path(json.value("content", std::string("Content")));
		project.saved = project.root / "Saved";
		project.startupScene = json.value("startupScene", std::string());
		if (project.module.empty())
		{
			error = file.filename().string() + ": \"module\" is missing";
			return false;
		}
		out = std::move(project);
		return true;
	}

	std::vector<ProjectDesc> FindKnownProjects()
	{
		std::vector<ProjectDesc> projects;
#if defined(CHERRY_SOURCE_DIR)
		std::error_code ec;
		for (const fs::directory_entry& dir : fs::directory_iterator(fs::path(CHERRY_SOURCE_DIR), ec))
		{
			if (!dir.is_directory(ec)) continue;
			for (const fs::directory_entry& entry : fs::directory_iterator(dir.path(), ec))
			{
				if (entry.path().extension() != ".cherryproject") continue;
				ProjectDesc project;
				std::string error;
				if (LoadProjectFile(entry.path(), project, error)) projects.push_back(std::move(project));
				else CHERRY_WARN("%s", error.c_str());
			}
		}
		std::sort(projects.begin(), projects.end(), [](const ProjectDesc& a, const ProjectDesc& b) { return a.name < b.name; });
#endif
		return projects;
	}

	std::vector<fs::path> LoadRecentProjects()
	{
		std::vector<fs::path> recent;
		std::wifstream stream(RecentProjectsFile());
		std::wstring line;
		while (std::getline(stream, line))
		{
			if (!line.empty()) recent.emplace_back(line);
		}
		return recent;
	}

	void AddRecentProject(const fs::path& file)
	{
		if (file.empty()) return;
		std::vector<fs::path> recent = LoadRecentProjects();
		recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const fs::path& p) { return SamePath(p, file); }), recent.end());
		recent.insert(recent.begin(), fs::absolute(file).lexically_normal());
		if (recent.size() > kMaxRecentProjects) recent.resize(kMaxRecentProjects);

		std::wofstream stream(RecentProjectsFile(), std::ios::trunc);
		for (const fs::path& p : recent) stream << p.wstring() << L"\n";
	}
}
