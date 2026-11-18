#pragma once
#include "Scene/ECS.h"
#include "Core/Log.h"
#include <DirectXMath.h>
#include <deque>

class Engine;
class IGameModule;

// The engine's ImGui UI. In editor mode: a menu bar with the play controls, and a
// dockspace with the game Viewport, Stats, Output and Game panels (the module's
// OnImGui). In game mode: the game's UI as a small overlay. Also owns engine debug
// tools (voxel test) and the project picker shown when no project is open.
class EditorLayer
{
public:
	// `game` is the running game, or null while editing (its UI only runs while playing).
	// `gameOverlayWindow`: wrap the game's UI in a small window in game mode (EngineConfig).
	void Draw(Engine& engine, IGameModule* game, bool editorMode, bool gameOverlayWindow, float frameMs);
	// The active world is about to be destroyed or swapped (play / stop).
	void OnWorldChanging(Engine& engine);
	void Shutdown(Engine& engine);

	// Results of the last Draw(), used for input routing and view sizing.
	bool viewportHovered = false;
	bool viewportFocused = false;
	DirectX::XMFLOAT2 viewportPosition{ 0, 0 };   // window client coordinates
	DirectX::XMFLOAT2 viewportSize{ 0, 0 };

private:
	void HandleHotkeys(Engine& engine);
	void DrawMenuBar(Engine& engine);
	void DrawDockspace();
	void DrawViewport(Engine& engine);
	void DrawStats(Engine& engine, float frameMs);
	void DrawOutput();
	void DrawProjectPicker(Engine& engine);
	void BrowseForProject(Engine& engine);
	void SetVoxelTest(Engine& engine, bool enabled);

	bool mLayoutBuilt = false;
	bool mVoxelTest = false;
	bool mPhysicsDebug = false;
	Entity mVoxelEntity = INVALID_ENTITY;
	float mFrameMsSmoothed = 16.0f;

	// Output panel
	std::deque<cherry::LogEntry> mLog;
	uint64_t mLogSequence = 0;
	bool mLogScrollToBottom = true;
	bool mLogShowInfo = true;
	bool mLogShowBuild = true;
};
