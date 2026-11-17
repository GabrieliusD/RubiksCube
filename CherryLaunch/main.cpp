// CherryLaunch: starts the engine with a game module.
//
// Modular build:    loads the game DLL (default CHERRY_ACTIVE_GAME_MODULE, or the
//                   first command-line argument) and calls its CreateGameModule().
// Monolithic build: the game is statically linked; CreateGameModule() is called directly.
#include <Core/Engine.h>
#include <Game/GameModule.h>
#include <Core/Log.h>
#include <memory>

#if CHERRY_MONOLITHIC
extern "C" IGameModule* CreateGameModule();
#else
#include <windows.h>
#ifndef CHERRY_ACTIVE_GAME_MODULE
#define CHERRY_ACTIVE_GAME_MODULE "RubikGame.dll"
#endif
#endif

int main(int argc, char** argv)
{
#if CHERRY_MONOLITHIC
	(void)argc;
	(void)argv;
	std::unique_ptr<IGameModule> game(CreateGameModule());
	Engine engine;
	return engine.Run(*game);
#else
	const char* moduleName = (argc > 1) ? argv[1] : CHERRY_ACTIVE_GAME_MODULE;

	HMODULE gameModule = LoadLibraryA(moduleName);
	if (!gameModule)
	{
		CHERRY_ERROR("Failed to load game module: %s (error %lu)", moduleName, GetLastError());
		return -1;
	}

	auto createGameModule = reinterpret_cast<CreateGameModuleFn>(GetProcAddress(gameModule, "CreateGameModule"));
	if (!createGameModule)
	{
		CHERRY_ERROR("CreateGameModule not exported by %s", moduleName);
		FreeLibrary(gameModule);
		return -2;
	}

	int result = -3;
	{
		std::unique_ptr<IGameModule> game(createGameModule());
		if (game)
		{
			Engine engine;
			result = engine.Run(*game);
		}
		// Engine and game are destroyed here, before the module is unloaded.
	}

	FreeLibrary(gameModule);
	return result;
#endif
}
