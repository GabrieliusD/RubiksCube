#pragma once
#include "CherryAPI.h"
#include "Scene/Scene.h"
#include "Renderer/Renderer.h"
#include "Core/Input.h"
#include "Core/Window.h"
#include <memory>
#include <optional>

class IGameModule;

// A rectangle in window client pixels (same space as ImGui screen coordinates).
struct ViewRect
{
	float x = 0.0f;
	float y = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
};

struct RaycastHit
{
	Entity entity = INVALID_ENTITY;
	float distance = 0.0f;
	DirectX::XMFLOAT3 point{ 0, 0, 0 };
	DirectX::XMFLOAT3 normal{ 0, 0, 0 };   // world-space normal of the hit bounding-box face
};

// Owns the window, the main loop, the renderer, the scene and input, and drives
// a game module through IGameModule.
class CHERRY_API Engine
{
public:
	Engine();
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	// Runs until the window closes or RequestQuit(). Returns the process exit code.
	int Run(IGameModule& game);
	void RequestQuit();

	Scene& GetScene();
	Renderer& GetRenderer();
	Input& GetInput();
	Window& GetWindow();

	float DeltaTime() const;
	float TotalTime() const;

	// The engine creates a default camera entity; games can move it or replace it.
	Entity MainCamera() const;
	void SetMainCamera(Entity camera);

	// Ray from the main camera through a pixel of the game view (view-relative pixels,
	// as returned by Input::MousePosition()).
	Ray ScreenPointToRay(const DirectX::XMFLOAT2& viewPixel) const;
	// Closest hit against the bounding boxes of visible MeshRenderer entities.
	std::optional<RaycastHit> Raycast(const Ray& ray) const;

	bool IsEditorMode() const;
	void SetEditorMode(bool editor);

	// Where the game view is drawn this frame: the editor viewport panel, or the whole
	// window in game mode. Use it to position HUD elements drawn with ImGui.
	ViewRect GameViewRect() const;

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
