#include <iostream>
#include <memory>

#include <D3DApp.h>

#if CHERRY_MONOLITHIC
extern "C" D3DApp* CreateApplication();
#else
#include <Windows.h>
#include <GameModuleAPI.h>
#ifndef CHERRY_ACTIVE_GAME_MODULE
#define CHERRY_ACTIVE_GAME_MODULE "RubikGame.dll"
#endif
#endif

int main(int argc, char** argv)
{
#if CHERRY_MONOLITHIC
	std::unique_ptr<D3DApp> app(CreateApplication());
	return app ? app->Run() : -1;
#else
	const char* moduleName = (argc > 1) ? argv[1] : CHERRY_ACTIVE_GAME_MODULE;

	HMODULE gameModule = LoadLibraryA(moduleName);
	if (!gameModule)
	{
		std::cerr << "Failed to load module: " << moduleName << '\n';
		return -1;
	}

	auto createApplication = reinterpret_cast<CreateApplicationFn>(GetProcAddress(gameModule, "CreateApplication"));
	if (!createApplication)
	{
		std::cerr << "CreateApplication not found in module." << '\n';
		FreeLibrary(gameModule);
		return -2;
	}

	std::unique_ptr<D3DApp> app(createApplication());
	if (!app)
	{
		FreeLibrary(gameModule);
		return -3;
	}

	const int result = app->Run();
	app.reset();

	FreeLibrary(gameModule);
	return result;
#endif
}
