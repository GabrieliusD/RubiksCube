#pragma once
#include "CherryAPI.h"
#include "Scene/Scene.h"
#include "Renderer/Renderer.h"
#include "Core/Input.h"
#include "Core/Window.h"
#include "Physics/PhysicsWorld.h"
#include "Core/Project.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class IGameModule;

// How the engine starts (see CherryLaunch).
struct LaunchOptions
{
	// Open this project in the editor. Empty: reopen the most recent project, or show
	// the project picker.
	std::filesystem::path projectFile;
	// Legacy launch by DLL name ("UndertimeGame.dll"): opens the project whose module
	// this is and plays at once.
	std::string moduleName;
	// Start playing right away. This makes it a game session: the game quitting
	// (RequestQuit) closes the window instead of returning to the editor.
	bool play = false;
	// Monolithic builds: the game is linked into the executable. Implies `play`;
	// there's no module to compile or reload.
	IGameModule* staticGame = nullptr;
};

// Edit: the edit world is shown and no game code runs (not even OnImGui).
// Playing: the game runs in a fresh play world. Paused: the play world is frozen
// (no OnUpdate, no physics) but still rendered, and OnImGui still runs.
enum class PlayState { Edit, Playing, Paused };

// The game module as the editor sees it.
struct ModuleStatus
{
	bool hasProject = false;
	bool isStatic = false;        // linked into the exe: no compile / reload
	bool loaded = false;
	bool compiling = false;
	bool lastBuildFailed = false;
	std::string moduleFile;       // e.g. "UndertimeGame.dll"
	std::string error;            // why the module isn't loaded
	uint32_t reloads = 0;
};

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
	int Run(const LaunchOptions& options);
	// Shorthand for a statically linked game (monolithic builds).
	int Run(IGameModule& game);
	// In a game session: closes the window. While playing in the editor: stops playing.
	void RequestQuit();

	// ---- Project --------------------------------------------------------------
	// The open project (IsValid() is false while the project picker is shown).
	const ProjectDesc& Project() const;
	// Absolute path of a file in the project's Content folder.
	std::wstring ContentPath(const std::wstring& relative) const;
	// Per-project folder for editor and game state that isn't checked in.
	std::filesystem::path SavedDir() const;

	// ---- Play mode (requests take effect at the start of the next frame) -------
	PlayState GetPlayState() const;
	void Play();
	void Stop();
	void SetPaused(bool paused);

	// ---- Editor-facing ----------------------------------------------------------
	ModuleStatus GetModuleStatus() const;
	// Builds the project's game module with CMake in the background; a successful build
	// is reloaded automatically (also when the build comes from Visual Studio).
	void CompileModule();
	void ReloadModule();
	// Restarts the editor on another project.
	void OpenProject(const std::filesystem::path& projectFile);

	Scene& GetScene();
	Renderer& GetRenderer();
	Input& GetInput();
	Window& GetWindow();
	PhysicsWorld& GetPhysics();

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
