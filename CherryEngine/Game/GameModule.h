#pragma once
#include "Scene/Reflection.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Engine;

// Startup settings a game can request.
struct EngineConfig
{
	std::wstring windowTitle = L"Cherry Engine";
	uint32_t windowWidth = 1600;
	uint32_t windowHeight = 900;
	bool startInEditor = true;      // game sessions (--play): false = full-window game view (F1 toggles at runtime)
	// In game mode, wrap OnImGui in a small overlay window. Turn off for games that
	// draw their own HUD (e.g. with ImGui::GetForegroundDrawList()).
	bool gameUiOverlayWindow = true;
};

// Game logic that works on entities placed in scenes (Unreal: actor components / Unity:
// MonoBehaviours, but one object per type that loops over the entities it cares about).
// Systems belong to the game module and are recreated on every module (re)load.
class ISystem
{
public:
	virtual ~ISystem() = default;
	// Play: after IGameModule::OnStartup, every frame after IGameModule::OnUpdate, and
	// before IGameModule::OnShutdown.
	virtual void OnPlayBegin(Engine& /*engine*/) {}
	virtual void OnUpdate(Engine& /*engine*/, float /*deltaTime*/) {}
	virtual void OnPlayEnd(Engine& /*engine*/) {}
	// Edit mode: return true to also run OnEditUpdate on the edit scene (previews, helpers).
	// It must not create entities with an EntityInfo (they would be saved with the scene).
	virtual bool RunsInEditMode() const { return false; }
	virtual void OnEditUpdate(Engine& /*engine*/, float /*deltaTime*/) {}
	// Extra inspector UI under the component list of a selected entity (edit and play).
	virtual void OnInspectorGui(Engine& /*engine*/, Entity /*entity*/) {}
	// The module is about to be unloaded (reload, editor closing): free renderer resources
	// the system made outside play (e.g. edit-mode previews).
	virtual void OnUnload(Engine& /*engine*/) {}
};

// What a game module registers when it loads: reflected component types (they appear
// in the editor's Add Component menu, are saved in scene files and copied into the play
// world) and systems.
class GameTypes
{
public:
	explicit GameTypes(TypeRegistry& registry) : mRegistry(registry) {}

	template <typename C>
	ComponentBuilder<C> Component(const std::string& name, uint32_t flags = Component_None)
	{
		return mRegistry.Component<C>(name, flags);
	}

	template <typename S, typename... Args>
	S& System(Args&&... args)
	{
		auto system = std::make_unique<S>(std::forward<Args>(args)...);
		S& reference = *system;
		mSystems.push_back(std::move(system));
		return reference;
	}

	std::vector<std::unique_ptr<ISystem>>& Systems() { return mSystems; }

private:
	TypeRegistry& mRegistry;
	std::vector<std::unique_ptr<ISystem>> mSystems;
};

// The contract between the engine and a game, Unreal-style: the engine owns
// the window, loop, renderer and scene; a game is a separate module (DLL, or a
// static library in monolithic builds) that implements this interface.
//
// In the editor, OnStartup runs on every Play (in a fresh copy of the edit scene) and
// OnShutdown on every Stop, so a game must free everything it made in between.
class IGameModule
{
public:
	virtual ~IGameModule() = default;

	virtual EngineConfig GetConfig() const { return {}; }

	// Component types and systems. Called once each time the module is loaded, before
	// any scene is loaded.
	virtual void RegisterTypes(GameTypes& /*types*/) {}

	// Create resources and entities. The renderer and the play scene are ready.
	virtual void OnStartup(Engine& engine) = 0;
	// Game logic, once per frame before rendering.
	virtual void OnUpdate(Engine& engine, float deltaTime) = 0;
	// Game UI; ImGui calls are valid here.
	virtual void OnImGui(Engine& /*engine*/) {}
	// Release game-owned resources. Called before the play scene is destroyed.
	virtual void OnShutdown(Engine& /*engine*/) {}

	// Hot reload while playing: the engine stops the game, swaps the DLL and plays again.
	// SaveState runs on the old module before it stops; LoadState on the new one after its
	// OnStartup. Whatever the text holds (JSON, numbers) is up to the game.
	virtual std::string SaveState(Engine& /*engine*/) { return {}; }
	virtual void LoadState(Engine& /*engine*/, const std::string& /*state*/) {}
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
