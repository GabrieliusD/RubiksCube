#pragma once
#include <string>
#include <cstdint>

class Engine;

// Startup settings a game can request.
struct EngineConfig
{
	std::wstring windowTitle = L"Cherry Engine";
	uint32_t windowWidth = 1600;
	uint32_t windowHeight = 900;
	bool startInEditor = true;      // false = full-window game view (F1 toggles at runtime)
};

// The contract between the engine and a game, Unreal-style: the engine owns
// the window, loop, renderer and scene; a game is a separate module (DLL, or a
// static library in monolithic builds) that implements this interface.
class IGameModule
{
public:
	virtual ~IGameModule() = default;

	virtual EngineConfig GetConfig() const { return {}; }

	// Create resources and entities. The renderer and scene are ready.
	virtual void OnStartup(Engine& engine) = 0;
	// Game logic, once per frame before rendering.
	virtual void OnUpdate(Engine& engine, float deltaTime) = 0;
	// Game UI; ImGui calls are valid here.
	virtual void OnImGui(Engine& /*engine*/) {}
	// Release game-owned resources. Called before the scene is cleared.
	virtual void OnShutdown(Engine& /*engine*/) {}
};

// Every game module exports this factory.
using CreateGameModuleFn = IGameModule* (*)();

#if defined(_WIN32)
	#define CHERRY_GAME_EXPORT extern "C" __declspec(dllexport)
#else
	#define CHERRY_GAME_EXPORT extern "C"
#endif

#define CHERRY_IMPLEMENT_GAME_MODULE(ModuleClass) \
	CHERRY_GAME_EXPORT IGameModule* CreateGameModule() { return new ModuleClass(); }
