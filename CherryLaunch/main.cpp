// CherryLaunch: starts the engine on a project.
//
// Modular build (the editor):
//   CherryLaunch.exe                          reopen the last project, or show the project picker
//   CherryLaunch.exe Undertime/Undertime.cherryproject [--play] [--rhi d3d12|vulkan]
//   CherryLaunch.exe Dice/Dice.cherryproject --cook-shaders <dir>   SPIR-V for Android (Tools/Android/Package.ps1)
//   CherryLaunch.exe UndertimeGame.dll        legacy: that module's project, playing at once
// The engine loads (a copy of) the project's game DLL itself, so it can rebuild and reload it.
//
// Monolithic build: the game is statically linked; CreateGameModule() is called directly.
#include <Core/Engine.h>
#include <Game/GameModule.h>
#include <memory>
#include <string>

#if CHERRY_MONOLITHIC
extern "C" IGameModule* CreateGameModule();
#endif

namespace
{
	bool EndsWith(const std::string& text, const std::string& suffix)
	{
		if (text.size() < suffix.size()) return false;
		for (size_t i = 0; i < suffix.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(text[text.size() - suffix.size() + i])) != suffix[i]) return false;
		}
		return true;
	}
}

int main(int argc, char** argv)
{
#if CHERRY_MONOLITHIC
	(void)argc;
	(void)argv;
	std::unique_ptr<IGameModule> game(CreateGameModule());
	Engine engine;
	return engine.Run(*game);
#else
	LaunchOptions options;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--play") options.play = true;
		else if (arg == "--rhi" && i + 1 < argc) options.graphicsApi = argv[++i];
		else if (arg == "--cook-shaders" && i + 1 < argc) { options.cookShaders = argv[++i]; options.play = true; }
		else if (EndsWith(arg, ".dll")) options.moduleName = arg;
		else options.projectFile = arg;
	}
	Engine engine;
	return engine.Run(options);
#endif
}
