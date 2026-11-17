#pragma once
#include "Scene/ECS.h"
#include <DirectXMath.h>

class Engine;
class IGameModule;

// The engine's ImGui UI. In editor mode: a dockspace with the game Viewport,
// a Stats/Settings panel and a Game panel (the module's OnImGui). In game mode:
// the game's UI as a small overlay. Also owns engine debug tools (voxel test).
class EditorLayer
{
public:
	// `gameOverlayWindow`: wrap the game's UI in a small window in game mode (EngineConfig).
	void Draw(Engine& engine, IGameModule& game, bool editorMode, bool gameOverlayWindow, float frameMs);
	void Shutdown(Engine& engine);

	// Results of the last Draw(), used for input routing and view sizing.
	bool viewportHovered = false;
	bool viewportFocused = false;
	DirectX::XMFLOAT2 viewportPosition{ 0, 0 };   // window client coordinates
	DirectX::XMFLOAT2 viewportSize{ 0, 0 };

private:
	void DrawDockspace();
	void DrawViewport(Engine& engine);
	void DrawStats(Engine& engine, float frameMs);
	void SetVoxelTest(Engine& engine, bool enabled);

	bool mLayoutBuilt = false;
	bool mVoxelTest = false;
	Entity mVoxelEntity = INVALID_ENTITY;
	float mFrameMsSmoothed = 16.0f;
};
