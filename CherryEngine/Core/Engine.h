#pragma once
#include "CherryAPI.h"
#include "Scene/Scene.h"
#include "Renderer/Renderer.h"
#include "Core/Input.h"
#include "Core/Window.h"
#include "Physics/PhysicsWorld.h"
#include "Core/Project.h"
#include "Core/Assets.h"
#include "Particles/ParticleSystem.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class IGameModule;
class ISystem;

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
	// The graphics API: "d3d12" or "vulkan" (--rhi). Empty: the CHERRY_RHI environment
	// variable, else the one picked in the editor (EngineUserDir()/GraphicsApi.txt), else D3D12.
	std::string graphicsApi;
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
	// During OnStartup after a hot reload: what the old module's SaveState returned (it is
	// also passed to LoadState right after). Empty otherwise.
	const std::string& HotReloadState() const;

	// ---- Editor-facing ----------------------------------------------------------
	ModuleStatus GetModuleStatus() const;
	// The game's systems (for the editor's inspector hooks).
	const std::vector<std::unique_ptr<ISystem>>& GameSystems() const;
	// Builds the project's game module with CMake in the background; a successful build
	// is reloaded automatically (also when the build comes from Visual Studio).
	// `reconfigure`: run CMake's configure step first, needed after adding source files
	// (a build regenerates the projects, but only the next build would compile them).
	void CompileModule(bool reconfigure = false);
	// Exports the project as an Android APK in the background (Tools/Android/Package.ps1, arm64;
	// its output goes to the log, the APK to out/android/). Editor builds only.
	void ExportAndroid(bool release);
	bool IsExportingAndroid() const;
	void ReloadModule();
	// Restarts the editor on another project (asks about unsaved scene changes first).
	void OpenProject(const std::filesystem::path& projectFile);

	// ---- Edit scene (the scene document the editor edits) -----------------------
	// Paths may be absolute or relative to the project root. Scene changes requested while
	// playing stop play first. All of these take effect at the start of the next frame.
	void NewScene();
	void OpenScene(const std::filesystem::path& file);
	// Saves the edit scene (an empty path: to its current file). Returns false on failure.
	bool SaveScene(const std::filesystem::path& file = {});
	const std::filesystem::path& ScenePath() const;   // empty for a scene never saved
	bool IsSceneDirty() const;
	void MarkSceneDirty();
	// The edit world's scene, whatever is playing.
	Scene& GetEditScene();
	// Makes `file` the scene the project opens with (written to the .cherryproject).
	bool SetStartupScene(const std::filesystem::path& file);
	// Shows a Save / Don't save / Cancel prompt if the scene has unsaved changes.
	// Returns false if the user cancelled.
	bool ConfirmDiscardSceneChanges();

	// ---- Spawning ------------------------------------------------------------------------
	// Places a prefab (Content path) in the active scene at a world position; returns its
	// root (INVALID_ENTITY if the prefab can't be read). Scripts in it start next frame.
	Entity Spawn(const std::string& prefab, const DirectX::XMFLOAT3& position, Entity parent = INVALID_ENTITY);
	// Destroys an entity and its children at the end of this frame's script update, after
	// calling OnDestroy on their scripts (safe from inside scripts).
	void DestroyLater(Entity entity);

	// The scene of the world being shown: the play world while playing, else the edit world.
	Scene& GetScene();
	AssetManager& GetAssets();
	Renderer& GetRenderer();
	Input& GetInput();
	Window& GetWindow();
	PhysicsWorld& GetPhysics();
	// Particles: the scene's ParticleEmitters, and one-off bursts (ParticleSystem::Burst).
	ParticleSystem& GetParticles();

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
