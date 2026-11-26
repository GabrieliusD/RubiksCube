#include "Core/Engine.h"
#include "Core/Log.h"
#include "Game/GameModule.h"
#if CHERRY_EDITOR
#include "Core/ModuleHost.h"
#include "Core/BuildProcess.h"
#include "Editor/EditorLayer.h"
#else
#include "Core/NoEditor.h"
#endif
#include "Scene/SceneSerializer.h"
#include "ShaderSystem/ShaderCompiler.h"
#include "Game/Script.h"
#include "UI/Ui.h"
#include <RmlUi/Core/Context.h>
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cctype>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <cwchar>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
	// One scene with its own physics. The edit world lives for the whole session; a play
	// world is made on Play and destroyed on Stop, so every play starts from scratch.
	struct World
	{
		std::unique_ptr<Scene> scene;
		std::unique_ptr<PhysicsWorld> physics;
		Entity camera = INVALID_ENTITY;

		explicit operator bool() const { return scene != nullptr; }
	};

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	std::wstring Widen(const std::string& text) { return fs::path(text).wstring(); }

	bool EndsWithNoCase(const std::string& text, const char* suffix)
	{
		const size_t n = std::strlen(suffix);
		if (text.size() < n) return false;
		for (size_t i = 0; i < n; ++i)
		{
			if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
		}
		return true;
	}

	Transform DefaultCameraTransform()
	{
		Transform transform;
		transform.position = { 0.0f, 6.0f, -15.0f };
		transform.LookAt({ 0, 0, 0 });
		return transform;
	}

	fs::path SavedDirFor(const ProjectDesc& project) { return project.IsValid() ? project.saved : cherry::EngineUserDir(); }
}

struct Engine::Impl
{
	Window window;
	Input input;
	Renderer renderer;
	AssetManager assets;
	ParticleSystem particles;
	Ui ui;
	EditorLayer editor;

	// The scene document
	fs::path scenePath;               // absolute; empty until saved
	bool sceneDirty = false;
	bool wantNewScene = false;
	fs::path wantOpenScene;

	World editWorld;
	World playWorld;
	World* active = &editWorld;

	// Project and game module
	ProjectDesc project;
	ModuleHost host;
	IGameModule* staticGame = nullptr;
	IGameModule* game = nullptr;          // the loaded game, or null
	std::vector<std::unique_ptr<ISystem>> systems;   // the game's systems (made by RegisterTypes)
	Engine* self = nullptr;
	std::string pendingGameState;         // SaveState() of the module before a reload, for LoadState()
	std::vector<Entity> destroyQueue;     // DestroyLater
	std::string moduleError;
	BuildProcess build;
	BuildProcess androidExport;
	bool lastBuildFailed = false;
	uint32_t reloads = 0;
	float watchTimer = 0.0f;
	float assetWatchTimer = 0.0f;

	// Play mode
	PlayState playState = PlayState::Edit;
	bool gameSession = false;             // launched to play: quitting the game closes the window
	LaunchOptions launch;                 // how this run started (a restart starts the same way)
	bool wantPlay = false;
	bool wantStop = false;
	bool wantReload = false;
	bool wantPause = false;
	bool pauseRequested = false;
	RenderSettings settingsBeforePlay;
	// Leak check: GPU descriptors in use before Play, compared a few frames after Stop
	// (frees are deferred until the GPU is done with the frames in flight).
	uint32_t descriptorsBeforePlay = 0;
	int leakCheckFrames = -1;

	RenderView mainView{};
	ViewRect gameView{};
	bool editorMode = true;
	bool quitRequested = false;
	float deltaTime = 0.0f;
	float totalTime = 0.0f;
	std::wstring baseTitle;
	bool gameUiOverlayWindow = true;
	std::string imguiIniPath;

	bool ResolveProject(const LaunchOptions& options, std::string& error);
	void LoadModule();
	void UnloadModule();
	void NotifySystemsUnload();
	void RegisterGameTypes();
	std::string GameOwner() const { return staticGame ? std::string("game") : project.module; }
	World MakeWorld(bool withCamera);
	fs::path ResolveScenePath(const fs::path& file) const;
	void LoadEditScene(const fs::path& file);
	void MakeNewScene(bool templateContent);
	void LoadEditorState();
	void SaveEditorState();
	void ResolveAssets(Scene& scene);
	void RunScripts(Engine& engine, float deltaTime);
	void DestroyScriptsOf(Scene& scene, Entity root);
	void FlushDestroyQueue();
	void StopScripts();
	void ApplyEnvironment(Scene& scene);
	void DestroyWorld(World& world);
	void StartPlay(Engine& engine);
	void StopPlay(Engine& engine);
	void ProcessRequests(Engine& engine);
	void UpdateModuleWatch(float dt);
	bool UpdateMainView();
	int Loop(Engine& engine);
};

Engine::Engine() : mImpl(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

Scene& Engine::GetScene() { return *mImpl->active->scene; }
Scene& Engine::GetEditScene() { return *mImpl->editWorld.scene; }
AssetManager& Engine::GetAssets() { return mImpl->assets; }
Renderer& Engine::GetRenderer() { return mImpl->renderer; }
ParticleSystem& Engine::GetParticles() { return mImpl->particles; }
Ui& Engine::GetUi() { return mImpl->ui; }
Input& Engine::GetInput() { return mImpl->input; }
Window& Engine::GetWindow() { return mImpl->window; }
PhysicsWorld& Engine::GetPhysics() { return *mImpl->active->physics; }
float Engine::DeltaTime() const { return mImpl->deltaTime; }
float Engine::TotalTime() const { return mImpl->totalTime; }
Entity Engine::MainCamera() const { return mImpl->active->camera; }
void Engine::SetMainCamera(Entity camera) { mImpl->active->camera = camera; }
bool Engine::IsEditorMode() const { return mImpl->editorMode; }
ViewRect Engine::GameViewRect() const { return mImpl->gameView; }
void Engine::SetEditorMode(bool editor) { mImpl->editorMode = editor; }

void Engine::RequestQuit()
{
	Impl& e = *mImpl;
	if (!e.gameSession && e.playState != PlayState::Edit) e.wantStop = true;   // back to the editor
	else e.quitRequested = true;
}

const ProjectDesc& Engine::Project() const { return mImpl->project; }
fs::path Engine::SavedDir() const { return mImpl->project.IsValid() ? mImpl->project.saved : cherry::EngineUserDir(); }
std::wstring Engine::ContentPath(const std::wstring& relative) const { return (mImpl->project.content / relative).wstring(); }

PlayState Engine::GetPlayState() const { return mImpl->playState; }
const std::string& Engine::HotReloadState() const { return mImpl->pendingGameState; }
void Engine::Play() { mImpl->wantPlay = true; }
void Engine::Stop() { mImpl->wantStop = true; }
void Engine::SetPaused(bool paused) { mImpl->wantPause = true; mImpl->pauseRequested = paused; }

const std::vector<std::unique_ptr<ISystem>>& Engine::GameSystems() const { return mImpl->systems; }

ModuleStatus Engine::GetModuleStatus() const
{
	const Impl& e = *mImpl;
	ModuleStatus status;
	status.hasProject = e.project.IsValid();
	status.isStatic = e.staticGame != nullptr;
	status.loaded = e.game != nullptr;
	status.compiling = e.build.IsRunning();
	status.lastBuildFailed = e.lastBuildFailed;
	status.moduleFile = e.project.module + ".dll";
	status.error = e.moduleError;
	status.reloads = e.reloads;
	return status;
}

void Engine::CompileModule(bool reconfigure)
{
	Impl& e = *mImpl;
	if (!e.project.IsValid() || e.staticGame || e.build.IsRunning()) return;
#if CHERRY_EDITOR && defined(CHERRY_CMAKE_COMMAND) && defined(CHERRY_BUILD_DIR) && defined(CHERRY_BUILD_CONFIG)
	// /nodeReuse:false: no MSBuild node outlives the build holding our output pipe.
	// A PDB name of its own for every editor build: a debugger attached to the editor keeps
	// the loaded module's PDB open, and relinking over it would fail (LNK1201).
	const std::wstring pdbSuffix = L"-" + std::to_wstring(static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
	std::wstring command = L"\"" + Widen(CHERRY_CMAKE_COMMAND) + L"\" --build \"" + Widen(CHERRY_BUILD_DIR)
		+ L"\" --config " + Widen(CHERRY_BUILD_CONFIG) + L" --target " + Widen(e.project.module)
		+ L" -- /nodeReuse:false /verbosity:minimal /nologo /p:CherryPdbSuffix=" + pdbSuffix;
	if (reconfigure)
	{
		// cmd /S /C ""configure" && "build"": configure, and build only if it worked.
		const std::wstring configure = L"\"" + Widen(CHERRY_CMAKE_COMMAND) + L"\" \"" + Widen(CHERRY_BUILD_DIR) + L"\"";
		command = L"cmd.exe /S /C \"" + configure + L" && " + command + L"\"";
	}
	CHERRY_LOG("[build] %s", fs::path(command).string().c_str());
	std::string error;
	if (!e.build.Start(command, error))
	{
		CHERRY_ERROR("[build] %s", error.c_str());
		e.lastBuildFailed = true;
	}
#else
	CHERRY_ERROR("This engine build doesn't know its CMake build folder; build the module from Visual Studio.");
#endif
}

void Engine::ReloadModule() { mImpl->wantReload = true; }

bool Engine::IsExportingAndroid() const { return mImpl->androidExport.IsRunning(); }

namespace
{
	fs::path ExecutablePath()
	{
#if defined(_WIN32)
		wchar_t buffer[MAX_PATH];
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return fs::path(buffer);
#else
		return {};
#endif
	}
}

void Engine::ExportAndroid(bool release)
{
	Impl& e = *mImpl;
	if (!e.project.IsValid() || e.project.file.empty() || e.androidExport.IsRunning()) return;
#if CHERRY_EDITOR && defined(CHERRY_SOURCE_DIR)
	const std::wstring script = (fs::path(CHERRY_SOURCE_DIR) / "Tools" / "Android" / "Package.ps1").wstring();
	const std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Project \""
		+ e.project.file.wstring() + L"\" -Config " + (release ? L"Release" : L"Debug") + L" -Abi arm64-v8a"
		+ L" -CookExe \"" + ExecutablePath().wstring() + L"\"";   // this editor cooks the shaders
	CHERRY_LOG("[build] Android export: %s", fs::path(command).string().c_str());
	std::string error;
	if (!e.androidExport.Start(command, error)) CHERRY_ERROR("[build] Android export: %s", error.c_str());
#else
	(void)release;
	CHERRY_ERROR("Exporting to Android needs the editor build on Windows");
#endif
}

namespace
{
	// Starts this executable again with `arguments`; the caller then quits.
	bool StartEditorProcess(const std::wstring& arguments)
	{
#if defined(_WIN32)
		// A fresh process is the simplest way to get a clean engine, ImGui layout and module.
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		std::wstring commandLine = L"\"" + std::wstring(exePath) + L"\" " + arguments;
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION info{};
		if (!CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
		{
			CHERRY_ERROR("Couldn't start the editor again (error %lu)", GetLastError());
			return false;
		}
		CloseHandle(info.hThread);
		CloseHandle(info.hProcess);
		return true;
#else
		(void)arguments;
		CHERRY_ERROR("Restarting needs the editor on Windows");
		return false;
#endif
	}

	void SaveGraphicsApiChoice(GraphicsApi api)
	{
		std::ofstream out(cherry::EngineUserDir() / "GraphicsApi.txt", std::ios::trunc);
		out << (api == GraphicsApi::Vulkan ? "vulkan" : "d3d12") << "\n";
	}
}

void Engine::OpenProject(const fs::path& projectFile)
{
	if (!ConfirmDiscardSceneChanges()) return;
	mImpl->sceneDirty = false;   // answered: don't ask again on the way out
	if (StartEditorProcess(L"\"" + fs::absolute(projectFile).wstring() + L"\"")) mImpl->quitRequested = true;
}

void Engine::SwitchGraphicsApi(GraphicsApi api)
{
	Impl& e = *mImpl;
	if (api == e.renderer.GetGraphicsApi()) return;
	if (!ConfirmDiscardSceneChanges()) return;
	e.sceneDirty = false;
	SaveGraphicsApiChoice(api);
	e.SaveEditorState();   // the new editor opens this scene
	std::wstring arguments;
	if (e.project.IsValid()) arguments = L"\"" + fs::absolute(e.project.file).wstring() + L"\" ";
	if (e.launch.play) arguments += L"--play ";
	arguments += api == GraphicsApi::Vulkan ? L"--rhi vulkan" : L"--rhi d3d12";
	CHERRY_LOG("Restarting on %s", GraphicsApiName(api));
	if (StartEditorProcess(arguments)) e.quitRequested = true;
}

void Engine::NewScene() { mImpl->wantNewScene = true; }
void Engine::OpenScene(const fs::path& file) { mImpl->wantOpenScene = mImpl->ResolveScenePath(file); }
const fs::path& Engine::ScenePath() const { return mImpl->scenePath; }
bool Engine::IsSceneDirty() const { return mImpl->sceneDirty; }
void Engine::MarkSceneDirty() { mImpl->sceneDirty = true; }

bool Engine::SaveScene(const fs::path& file)
{
	Impl& e = *mImpl;
	const fs::path target = file.empty() ? e.scenePath : e.ResolveScenePath(file);
	if (target.empty() || !e.editWorld) return false;
	std::string error;
	if (!cherry::SaveScene(*e.editWorld.scene, target, error))
	{
		CHERRY_ERROR("Saving the scene failed: %s", error.c_str());
		return false;
	}
	e.scenePath = target;
	e.sceneDirty = false;
	e.SaveEditorState();
	CHERRY_LOG("Saved scene %s", target.string().c_str());
	return true;
}

bool Engine::SetStartupScene(const fs::path& file)
{
	Impl& e = *mImpl;
	if (!e.project.IsValid() || e.project.file.empty()) return false;
	std::ifstream in(e.project.file);
	nlohmann::ordered_json json = nlohmann::ordered_json::parse(in, nullptr, false);
	if (json.is_discarded() || !json.is_object()) return false;
	const fs::path relative = e.ResolveScenePath(file).lexically_relative(e.project.root);
	json["startupScene"] = relative.generic_string();
	in.close();
	std::ofstream out(e.project.file, std::ios::trunc);
	out << json.dump(2) << "\n";
	if (!out) return false;
	e.project.startupScene = relative.generic_string();
	CHERRY_LOG("Startup scene: %s", e.project.startupScene.c_str());
	return true;
}

bool Engine::ConfirmDiscardSceneChanges()
{
	Impl& e = *mImpl;
	if (!e.sceneDirty) return true;
#if !defined(_WIN32)
	return true;   // no editor: nothing edits the scene document
#else
	const std::string name = e.scenePath.empty() ? std::string("the untitled scene") : e.scenePath.filename().string();
	const std::string question = "Save changes to " + name + "?";
	const int answer = MessageBoxA(e.window.Handle(), question.c_str(), "Cherry Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
	if (answer == IDCANCEL) return false;
	if (answer == IDYES)
	{
		if (e.scenePath.empty())
		{
			// No file yet: put it in Content/Scenes under a free name.
			fs::path target = e.project.content / "Scenes" / "Untitled.scene";
			for (int i = 2; fs::exists(target); ++i) target = e.project.content / "Scenes" / ("Untitled" + std::to_string(i) + ".scene");
			return SaveScene(target);
		}
		return SaveScene();
	}
	return true;
#endif
}

fs::path Engine::Impl::ResolveScenePath(const fs::path& file) const
{
	if (file.empty()) return {};
	if (file.is_absolute()) return file.lexically_normal();
	return (project.root / file).lexically_normal();
}

void Engine::Impl::MakeNewScene(bool templateContent)
{
	Scene& scene = *editWorld.scene;
	scene.Clear();
	const Entity environment = scene.CreateNamedEntity("Environment");
	scene.Add(environment, Environment{});
	if (templateContent)
	{
		const Entity floor = scene.CreateNamedEntity("Floor");
		scene.Get<Transform>(floor).scale = { 20.0f, 1.0f, 20.0f };
		MeshRenderer floorMesh;
		floorMesh.meshAsset = "engine/Plane";
		floorMesh.materialAsset = "engine/Stone";
		scene.Add(floor, floorMesh);

		const Entity cube = scene.CreateNamedEntity("Cube");
		scene.Get<Transform>(cube).position = { 0.0f, 0.5f, 0.0f };
		MeshRenderer cubeMesh;
		cubeMesh.meshAsset = "engine/Cube";
		cubeMesh.materialAsset = "engine/Default";
		scene.Add(cube, cubeMesh);

		const Entity light = scene.CreateNamedEntity("Light");
		scene.Get<Transform>(light).position = { 2.0f, 3.0f, -2.0f };
		scene.Add(light, PointLightComponent{});
	}
	scenePath.clear();
	sceneDirty = false;
}

void Engine::Impl::LoadEditScene(const fs::path& file)
{
	std::string error;
	if (!cherry::LoadScene(*editWorld.scene, file, error))
	{
		CHERRY_ERROR("Opening the scene failed: %s", error.c_str());
		MakeNewScene(false);
		return;
	}
	scenePath = file;
	sceneDirty = false;
	CHERRY_LOG("Opened scene %s (%zu entities)", file.string().c_str(), editWorld.scene->Entities().size());
}

void Engine::Impl::LoadEditorState()
{
	fs::path last;
	std::ifstream in(SavedDirFor(project) / "EditorState.json");
	if (in)
	{
		nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
		if (json.is_object() && json.contains("scene") && json["scene"].is_string())
		{
			last = ResolveScenePath(fs::path(json["scene"].get<std::string>()));
		}
	}
	if (last.empty() && !project.startupScene.empty()) last = ResolveScenePath(project.startupScene);
	if (!last.empty() && fs::exists(last)) LoadEditScene(last);
	else MakeNewScene(false);
}

void Engine::Impl::SaveEditorState()
{
	if (!project.IsValid() || staticGame || !launch.cookShaders.empty()) return;
	nlohmann::json json = nlohmann::json::object();
	json["scene"] = scenePath.empty() ? std::string() : scenePath.lexically_relative(project.root).generic_string();
	std::ofstream out(SavedDirFor(project) / "EditorState.json", std::ios::trunc);
	out << json.dump(2) << "\n";
}

void Engine::Impl::ResolveAssets(Scene& scene)
{
	for (Entity entity : scene.MeshEntities())
	{
		MeshRenderer& renderer = scene.Get<MeshRenderer>(entity);
		if (!renderer.mesh.IsValid() && !renderer.meshAsset.empty()) renderer.mesh = assets.GetMesh(renderer.meshAsset);
		if (!renderer.material.IsValid() && !renderer.materialAsset.empty()) renderer.material = assets.GetMaterial(renderer.materialAsset);
		// Voxel models (.vox) default to the material that shows their colours and glow.
		if (!renderer.material.IsValid() && renderer.materialAsset.empty() && renderer.meshAsset.size() > 4
			&& EndsWithNoCase(renderer.meshAsset, ".vox"))
		{
			renderer.material = assets.GetMaterial("engine/VoxelModel");
		}
		// ... and their glass part, if they have one.
		if (renderer.glassCheckedFor != renderer.meshAsset)
		{
			renderer.glassCheckedFor = renderer.meshAsset;
			renderer.glassMesh = {};
			const std::string& asset = renderer.meshAsset;
			if (asset.size() > 4 && EndsWithNoCase(asset, ".vox")) renderer.glassMesh = assets.GetMesh(asset + "#glass");
		}
		if (renderer.glassMesh.IsValid() && !renderer.glassMaterial.IsValid())
		{
			renderer.glassMaterial = assets.GetMaterial(renderer.glassMaterialAsset.empty() ? std::string("engine/VoxelGlass") : renderer.glassMaterialAsset);
		}
	}
	for (Entity entity : scene.VoxelEntities())
	{
		VoxelVolumeRenderer& renderer = scene.Get<VoxelVolumeRenderer>(entity);
		if (!renderer.material.IsValid() && !renderer.materialAsset.empty()) renderer.material = assets.GetMaterial(renderer.materialAsset);
	}
}

void Engine::Impl::ApplyEnvironment(Scene& scene)
{
	for (Entity entity : scene.Entities())
	{
		const Environment* environment = scene.TryGet<Environment>(entity);
		if (!environment) continue;
		RenderSettings& settings = renderer.Settings();
		settings.clearColor = environment->skyColor;
		settings.ambient = environment->ambient;
		settings.ambientContrast = environment->ambientContrast;
		settings.sunDirection = environment->sunDirection;
		settings.sunColor = environment->sunColor;
		settings.fillLightScale = environment->fillLightScale;
		settings.fogEnabled = environment->fogEnabled;
		settings.fogColor = environment->fogColor;
		settings.fogStart = environment->fogStart;
		settings.fogEnd = environment->fogEnd;
		settings.exposure = environment->exposure;
		settings.sky.enabled = environment->skyEnabled;
		settings.sky.zenith = environment->skyZenith;
		settings.sky.horizon = environment->skyHorizon;
		settings.sky.ground = environment->skyGround;
		settings.sky.gradientPower = environment->skyGradientPower;
		settings.sky.sunSize = environment->sunSize;
		settings.sky.sunIntensity = environment->sunIntensity;
		settings.sky.starIntensity = environment->starIntensity;
		settings.shadows.enabled = environment->shadowsEnabled;
		settings.shadows.nearDistance = environment->shadowNearDistance;
		settings.shadows.farDistance = environment->shadowFarDistance;
		settings.bloom.enabled = environment->bloomEnabled;
		settings.bloom.intensity = environment->bloomIntensity;
		settings.bloom.threshold = environment->bloomThreshold;
		return;   // the first one wins
	}
}

Entity Engine::Spawn(const std::string& prefab, const XMFLOAT3& position, Entity parent)
{
	Impl& e = *mImpl;
	std::string text, error;
	if (!e.assets.ReadPrefab(prefab, text)) return INVALID_ENTITY;
	Scene& scene = *e.active->scene;
	const Entity root = cherry::InstantiatePrefab(scene, text, error);
	if (root == INVALID_ENTITY)
	{
		CHERRY_ERROR("Spawn %s: %s", prefab.c_str(), error.c_str());
		return INVALID_ENTITY;
	}
	if (!scene.Has<Transform>(root)) scene.Add(root, Transform{});
	if (parent != INVALID_ENTITY) scene.SetParent(root, parent, false);
	// The position is in world space: convert it under the parent.
	const XMMATRIX parentWorld = parent != INVALID_ENTITY ? scene.WorldMatrix(parent) : XMMatrixIdentity();
	XMStoreFloat3(&scene.Get<Transform>(root).position, XMVector3TransformCoord(XMLoadFloat3(&position), XMMatrixInverse(nullptr, parentWorld)));
	scene.Get<EntityInfo>(root).prefab = prefab;
	return root;
}

void Engine::DestroyLater(Entity entity)
{
	if (entity != INVALID_ENTITY) mImpl->destroyQueue.push_back(entity);
}

void Engine::Impl::RunScripts(Engine& engine, float deltaTime)
{
	Scene& scene = *active->scene;
	const TypeRegistry& registry = TypeRegistry::Get();

	// Who has which script, collected first: scripts may spawn entities while running
	// (they start next frame) and destroying waits for the end of the pass.
	std::vector<std::pair<const ComponentInfo*, Entity>> scripts;
	for (const auto& info : registry.Components())
	{
		if (!info->Is(Component_Script) || !info->asScript) continue;
		for (Entity entity : scene.Entities())
		{
			if (info->has(scene, entity)) scripts.emplace_back(info.get(), entity);
		}
	}
	for (const auto& [info, entity] : scripts)
	{
		if (!scene.IsAlive(entity)) continue;
		void* component = info->get(scene, entity);
		if (!component) continue;
		Script& script = *info->asScript(component);
		try
		{
			if (!script.HasStarted())
			{
				ScriptBinding::Bind(script, &engine, entity);
				ScriptBinding::SetStarted(script, true);
				script.OnStart();
			}
			script.OnUpdate(deltaTime);
		}
		catch (const std::exception& ex)
		{
			if (gameSession) throw;
			CHERRY_ERROR("Script %s on %s threw: %s (stopping)", info->name.c_str(), scene.DisplayName(entity).c_str(), ex.what());
			wantStop = true;
			break;
		}
	}
	FlushDestroyQueue();
}

void Engine::Impl::DestroyScriptsOf(Scene& scene, Entity root)
{
	std::vector<Entity> tree{ root };
	for (size_t i = 0; i < tree.size(); ++i)
	{
		for (Entity child : scene.Children(tree[i])) tree.push_back(child);
	}
	for (Entity entity : tree)
	{
		for (const auto& info : TypeRegistry::Get().Components())
		{
			if (!info->Is(Component_Script) || !info->asScript) continue;
			void* component = info->get(scene, entity);
			if (!component) continue;
			Script& script = *info->asScript(component);
			if (!script.HasStarted()) continue;
			ScriptBinding::SetStarted(script, false);
			try { script.OnDestroy(); }
			catch (const std::exception& ex) { CHERRY_ERROR("Script %s OnDestroy threw: %s", info->name.c_str(), ex.what()); }
		}
	}
}

void Engine::Impl::FlushDestroyQueue()
{
	Scene& scene = *active->scene;
	// OnDestroy may queue more: keep going until it settles.
	for (int round = 0; round < 8 && !destroyQueue.empty(); ++round)
	{
		std::vector<Entity> queue;
		queue.swap(destroyQueue);
		for (Entity entity : queue)
		{
			if (!scene.IsAlive(entity)) continue;
			DestroyScriptsOf(scene, entity);
			scene.DestroyTree(entity);
		}
	}
	destroyQueue.clear();
}

void Engine::Impl::StopScripts()
{
	if (!playWorld) return;
	Scene& scene = *playWorld.scene;
	for (Entity entity : std::vector<Entity>(scene.Entities()))
	{
		if (scene.IsAlive(entity) && scene.GetParent(entity) == INVALID_ENTITY) DestroyScriptsOf(scene, entity);
	}
	destroyQueue.clear();
}

bool Engine::Impl::ResolveProject(const LaunchOptions& options, std::string& error)
{
	if (options.staticGame)
	{
		// Packaged game: content (and the project file, for the startup scene) were copied
		// next to the executable.
		const fs::path exeDir = cherry::ExecutableDir();
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(exeDir, ec))
		{
			std::string ignored;
			if (entry.path().extension() == ".cherryproject" && cherry::LoadProjectFile(entry.path(), project, ignored)) break;
		}
		if (project.name.empty()) project.name = "Game";
		project.module = "(static)";
		project.root = exeDir;
		project.content = exeDir / "Content";
		project.saved = exeDir / "Saved";
		return true;
	}
	if (!options.projectFile.empty())
	{
		return cherry::LoadProjectFile(options.projectFile, project, error);
	}
	if (!options.moduleName.empty())
	{
		std::string module = Lower(fs::path(options.moduleName).stem().string());
		for (const ProjectDesc& known : cherry::FindKnownProjects())
		{
			if (Lower(known.module) == module)
			{
				project = known;
				return true;
			}
		}
		error = "No .cherryproject found for module " + options.moduleName;
		return false;
	}
	for (const fs::path& recent : cherry::LoadRecentProjects())
	{
		std::string ignored;
		if (cherry::LoadProjectFile(recent, project, ignored)) return true;
	}
	return true;   // no project: the editor shows the project picker
}

void Engine::Impl::LoadModule()
{
	game = nullptr;
	moduleError.clear();
	if (staticGame)
	{
		game = staticGame;
		RegisterGameTypes();
		return;
	}
	if (!project.IsValid()) return;
	const fs::path dll = cherry::ExecutableDir() / (project.module + ".dll");
	if (host.Load(dll, project.saved / "Modules", moduleError))
	{
		game = host.Game();
		RegisterGameTypes();
	}
	else
	{
		CHERRY_WARN("Game module: %s", moduleError.c_str());
	}
}

void Engine::Impl::RegisterGameTypes()
{
	TypeRegistry& registry = TypeRegistry::Get();
	registry.SetCurrentOwner(GameOwner());
	GameTypes types(registry);
	try
	{
		game->RegisterTypes(types);
		game->RegisterScriptsFromFiles(types);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Game RegisterTypes threw: %s", ex.what());
	}
	registry.ResolvePendingBases();
	registry.SetCurrentOwner("engine");
	systems = std::move(types.Systems());
	size_t components = 0;
	for (const auto& info : registry.Components()) if (info->owner == GameOwner()) ++components;
	if (components || !systems.empty()) CHERRY_LOG("Game types: %zu components, %zu systems", components, systems.size());
}

void Engine::Impl::NotifySystemsUnload()
{
	// While the scenes still exist: systems free what they made outside play.
	for (auto& system : systems)
	{
		try { if (self) system->OnUnload(*self); }
		catch (const std::exception& ex) { CHERRY_ERROR("System OnUnload threw: %s", ex.what()); }
	}
}

void Engine::Impl::UnloadModule()
{
	// Everything made by code in the DLL goes before the DLL: systems, then the
	// reflected types (their scene operations), then the module itself.
	systems.clear();
	TypeRegistry::Get().Unregister(GameOwner());
	game = nullptr;
	host.Unload();
}

World Engine::Impl::MakeWorld(bool withCamera)
{
	World world;
	world.physics = std::make_unique<PhysicsWorld>();
	world.physics->Initialize();
	world.scene = std::make_unique<Scene>();
	if (!withCamera) return world;

	world.camera = world.scene->CreateEntity();
	world.scene->Add(world.camera, DefaultCameraTransform());
	world.scene->Add(world.camera, Camera{});
	return world;
}

void Engine::Impl::DestroyWorld(World& world)
{
	// The scene goes first and completely: component arrays a game registered were made
	// by code in the game DLL, which may be unloaded next.
	if (world.scene) world.scene->Clear();
	world.scene.reset();
	if (world.physics) world.physics->Shutdown();
	world.physics.reset();
	world.camera = INVALID_ENTITY;
}

void Engine::Impl::StartPlay(Engine& engine)
{
	particles.Clear();
	if (!game || playState != PlayState::Edit) return;
	editor.OnWorldChanging(engine);
	settingsBeforePlay = renderer.Settings();
	descriptorsBeforePlay = renderer.Stats().descriptorsInUse;

	// The play world starts as a copy of the edit scene. Its camera is the scene's first
	// Camera entity, or (scenes without one) a default camera the game can move.
	playWorld = MakeWorld(false);
	cherry::CopyScene(*editWorld.scene, *playWorld.scene);
	for (Entity entity : playWorld.scene->Entities())
	{
		if (playWorld.scene->Has<Camera>(entity) && playWorld.scene->Has<Transform>(entity))
		{
			playWorld.camera = entity;
			break;
		}
	}
	if (playWorld.camera == INVALID_ENTITY)
	{
		playWorld.camera = playWorld.scene->CreateEntity();
		playWorld.scene->Add(playWorld.camera, DefaultCameraTransform());
		playWorld.scene->Add(playWorld.camera, Camera{});
	}
	active = &playWorld;
	ApplyEnvironment(*playWorld.scene);   // before OnStartup, so the game can override it
	playState = PlayState::Playing;
	CHERRY_LOG("Play");
	ui.BeginSession(project.content, renderer.ViewWidth(), renderer.ViewHeight(), ImGui::GetStyle().FontScaleDpi);
	try
	{
		game->OnStartup(engine);
		for (auto& system : systems) system->OnPlayBegin(engine);
		if (!pendingGameState.empty()) game->LoadState(engine, pendingGameState);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Game OnStartup threw: %s", ex.what());
		wantStop = true;
	}
	pendingGameState.clear();
}

void Engine::Impl::StopPlay(Engine& engine)
{
	particles.Clear();
	if (playState == PlayState::Edit) return;
	StopScripts();
	try
	{
		for (auto& system : systems) system->OnPlayEnd(engine);
		if (game) game->OnShutdown(engine);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Game OnShutdown threw: %s", ex.what());
	}
	ui.EndSession();   // before the module can unload: its data models and listeners go with it
	input.SetMouseCaptured(false);
	editor.OnWorldChanging(engine);
	DestroyWorld(playWorld);
	active = &editWorld;
	playState = PlayState::Edit;

	// Undo what the game did to the lighting, but keep the editor's own toggles.
	RenderSettings& settings = renderer.Settings();
	const bool vsync = settings.vsync, culling = settings.cullingEnabled, bounds = settings.drawBounds;
	settings = settingsBeforePlay;
	settings.vsync = vsync;
	settings.cullingEnabled = culling;
	settings.drawBounds = bounds;
	leakCheckFrames = 4;
	CHERRY_LOG("Stop");
}

void Engine::Impl::ProcessRequests(Engine& engine)
{
	if (wantReload)
	{
		wantReload = false;
		if (!staticGame && project.IsValid())
		{
			const bool wasPlaying = playState != PlayState::Edit;
			if (wasPlaying && game)
			{
				try { pendingGameState = game->SaveState(engine); }
				catch (const std::exception& ex) { CHERRY_ERROR("Game SaveState threw: %s", ex.what()); }
			}
			StopPlay(engine);

			// The edit scene may hold components whose code is in the old DLL. Save it with
			// the old types, drop it, swap the module, and load it back with the new types
			// (fields are matched by name, so changed component layouts load too).
			editor.OnSceneReplaced(engine);
			NotifySystemsUnload();
			const std::string editJson = cherry::SceneToJson(*editWorld.scene);
			editWorld.scene.reset();
			UnloadModule();
			LoadModule();
			editWorld.scene = std::make_unique<Scene>();
			std::string error;
			if (!cherry::SceneFromJson(*editWorld.scene, editJson, error)) CHERRY_ERROR("Reloading the scene failed: %s", error.c_str());
			if (game)
			{
				++reloads;
				const EngineConfig config = game->GetConfig();
				gameUiOverlayWindow = config.gameUiOverlayWindow;
				CHERRY_LOG("Reloaded %s", project.module.c_str());
			}
			if (wasPlaying) wantPlay = true;
		}
	}
	if (wantNewScene || !wantOpenScene.empty())
	{
		StopPlay(engine);
		editor.OnSceneReplaced(engine);
		if (wantNewScene) MakeNewScene(true);
		else LoadEditScene(wantOpenScene);
		wantNewScene = false;
		wantOpenScene.clear();
		SaveEditorState();
	}
	if (wantStop)
	{
		wantStop = false;
		wantPlay = false;
		StopPlay(engine);
	}
	if (wantPlay)
	{
		wantPlay = false;
		StartPlay(engine);
	}
	if (wantPause)
	{
		wantPause = false;
		if (playState == PlayState::Playing && pauseRequested) playState = PlayState::Paused;
		else if (playState == PlayState::Paused && !pauseRequested) playState = PlayState::Playing;
	}
}

void Engine::Impl::UpdateModuleWatch(float dt)
{
	int exitCode = 0;
	if (build.Poll(exitCode))
	{
		lastBuildFailed = exitCode != 0;
		if (lastBuildFailed) CHERRY_ERROR("[build] failed (exit code %d)", exitCode);
		else CHERRY_LOG("[build] succeeded");
	}
	int exportCode = 0;
	if (androidExport.Poll(exportCode))
	{
		if (exportCode != 0) CHERRY_ERROR("[build] Android export failed (exit code %d)", exportCode);
#if defined(CHERRY_SOURCE_DIR)
		else CHERRY_LOG("[build] Android export done: the APK is in %s", (fs::path(CHERRY_SOURCE_DIR) / "out" / "android").string().c_str());
#else
		else CHERRY_LOG("[build] Android export done");
#endif
	}
	if (staticGame || !project.IsValid() || build.IsRunning()) return;

	watchTimer += dt;
	if (watchTimer < 0.25f) return;
	watchTimer = 0.0f;
	if (host.HasNewBuild()) wantReload = true;
}

bool Engine::Impl::UpdateMainView()
{
	Scene& scene = *active->scene;
	const Entity camera = active->camera;
	Transform transform;
	Camera cameraData;
	if (playState == PlayState::Edit)
	{
		// Editing: the editor's own fly camera, not an entity.
		editor.GetEditorCamera(transform, cameraData);
	}
	else
	{
		if (!scene.IsAlive(camera) || !scene.Has<Camera>(camera) || !scene.Has<Transform>(camera))
		{
			return false;
		}
		cameraData = scene.Get<Camera>(camera);
		XMVECTOR scale, rotation, translation;
		XMMatrixDecompose(&scale, &rotation, &translation, scene.WorldMatrix(camera));
		XMStoreFloat4(&transform.rotation, rotation);
		XMStoreFloat3(&transform.position, translation);
	}

	// View = inverse of the camera's rigid transform (scale ignored).
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&transform.rotation))
		* XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
	const float aspect = static_cast<float>(renderer.ViewWidth()) / static_cast<float>(std::max(renderer.ViewHeight(), 1u));

	XMStoreFloat4x4(&mainView.view, XMMatrixInverse(nullptr, world));
	const XMMATRIX proj = cameraData.orthographic
		? XMMatrixOrthographicLH(2.0f * cameraData.orthoHalfHeight * aspect, 2.0f * cameraData.orthoHalfHeight, cameraData.nearZ, cameraData.farZ)
		: XMMatrixPerspectiveFovLH(cameraData.fovY, aspect, cameraData.nearZ, cameraData.farZ);
	XMStoreFloat4x4(&mainView.proj, proj);
	mainView.position = transform.position;
	mainView.nearZ = cameraData.nearZ;
	mainView.farZ = cameraData.farZ;
	return true;
}

namespace
{
	// The graphics API to start with: the launch option, CHERRY_RHI, the saved choice, D3D12.
	GraphicsApi ChooseGraphicsApi(const std::string& requested)
	{
		auto parse = [](std::string text, GraphicsApi& api)
		{
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
			if (text == "vulkan" || text == "vk") { api = GraphicsApi::Vulkan; return true; }
			if (text == "d3d12" || text == "dx12" || text == "directx12") { api = GraphicsApi::D3D12; return true; }
			return false;
		};
		GraphicsApi api = GraphicsApi::D3D12;
		if (!requested.empty())
		{
			if (parse(requested, api)) return api;
			CHERRY_WARN("Unknown graphics API '%s' (d3d12 or vulkan)", requested.c_str());
		}
#if defined(_MSC_VER)
		char* env = nullptr;
		size_t envLength = 0;
		const bool fromEnv = _dupenv_s(&env, &envLength, "CHERRY_RHI") == 0 && env && parse(env, api);
		free(env);
		if (fromEnv) return api;
#else
		if (const char* env = std::getenv("CHERRY_RHI"); env && parse(env, api)) return api;
#endif
		std::ifstream saved(cherry::EngineUserDir() / "GraphicsApi.txt");
		std::string line;
		if (saved && std::getline(saved, line) && parse(line, api)) return api;
		return GraphicsApi::D3D12;
	}
}

int Engine::Run(IGameModule& game)
{
	LaunchOptions options;
	options.staticGame = &game;
	options.play = true;
	return Run(options);
}

int Engine::Run(const LaunchOptions& options)
{
	Impl& e = *mImpl;
	e.self = this;
	e.staticGame = options.staticGame;
	e.launch = options;
	e.gameSession = options.play || options.staticGame || !options.moduleName.empty();

	std::string error;
	if (!e.ResolveProject(options, error))
	{
		CHERRY_ERROR("%s", error.c_str());
		cherry::ShowError("Cherry Engine", error.c_str());
		return -1;
	}
	if (e.project.IsValid())
	{
		std::error_code ec;
		fs::create_directories(e.project.saved, ec);
		if (!e.staticGame)
		{
			cherry::AddRecentProject(e.project.file);
			ModuleHost::CleanShadowRoot(e.project.saved / "Modules");
		}
		CHERRY_LOG("Project %s (%s)", e.project.name.c_str(), e.project.root.string().c_str());
	}
	e.LoadModule();

	EngineConfig config = e.game ? e.game->GetConfig() : EngineConfig{};
	if (!e.game) config.windowTitle = e.project.IsValid() ? Widen(e.project.name) : L"Cherry Editor";
	e.editorMode = e.gameSession ? config.startInEditor : true;
	e.baseTitle = config.windowTitle;
	e.gameUiOverlayWindow = config.gameUiOverlayWindow;

	if (!e.window.Create(config.windowTitle, config.windowWidth, config.windowHeight))
	{
		CHERRY_ERROR("Failed to create the window");
		return -1;
	}
	e.input.AttachWindow(e.window.Handle());
#if defined(_WIN32)
	e.window.SetMessageHandler([&e](UINT msg, WPARAM wParam, LPARAM lParam) { e.input.HandleMessage(msg, wParam, lParam); });
#else
	e.window.SetInput(&e.input);
	e.window.SetSurfaceHandler([&e](void* native, uint32_t width, uint32_t height) { e.renderer.SetNativeWindow(native, width, height); });
#endif
	e.window.SetResizeHandler([&e](uint32_t width, uint32_t height) { e.renderer.OnWindowResized(width, height); });

	int exitCode = 0;
	try
	{
		if (!e.launch.cookShaders.empty())
		{
			ShaderCompiler::SetCookDirectory(fs::absolute(e.launch.cookShaders));
			CHERRY_LOG("Cooking shaders into %s", fs::absolute(e.launch.cookShaders).string().c_str());
		}
		const GraphicsApi api = ChooseGraphicsApi(e.launch.graphicsApi);
		e.renderer.SetGraphicsApi(api);
		bool initialized = e.renderer.Initialize(e.window.Handle(), e.window.Width(), e.window.Height(), error);
		if (!initialized && api == GraphicsApi::Vulkan)
		{
			// Vulkan didn't start (no driver, an old GPU): D3D12 instead, and from now on.
			CHERRY_ERROR("Vulkan couldn't start (%s); using D3D12", error.c_str());
			const std::string vulkanError = error;
			error.clear();
			e.renderer.SetGraphicsApi(GraphicsApi::D3D12);
			initialized = e.renderer.Initialize(e.window.Handle(), e.window.Width(), e.window.Height(), error);
			if (initialized)
			{
				SaveGraphicsApiChoice(GraphicsApi::D3D12);
				cherry::ShowError("Cherry Engine", ("Vulkan couldn't start, so the editor is using D3D12.\n\n" + vulkanError).c_str(), e.window.Handle());
			}
		}
		if (!initialized)
		{
			CHERRY_ERROR("Renderer initialization failed: %s", error.c_str());
			cherry::ShowError("Cherry Engine", error.c_str(), e.window.Handle());
			e.window.Destroy();
			return -2;
		}
		// Each project keeps its own window layout. ImGui reads the file on the first frame.
		e.imguiIniPath = (SavedDir() / "imgui.ini").string();
		ImGui::GetIO().IniFilename = e.launch.cookShaders.empty() ? e.imguiIniPath.c_str() : nullptr;   // a cook leaves the layout alone

		e.assets.Initialize(e.renderer);
		if (e.project.IsValid()) e.assets.SetContentRoot(e.project.content);
		e.ui.Initialize(e.renderer.UiRenderInterface());
		e.editWorld = e.MakeWorld(false);
		e.active = &e.editWorld;
		e.editor.LoadSettings(SavedDir() / "EditorCamera.txt");
		if (e.gameSession && !e.project.startupScene.empty()) e.LoadEditScene(e.ResolveScenePath(e.project.startupScene));
		else if (e.gameSession) e.MakeNewScene(false);
		else e.LoadEditorState();
		e.ResolveAssets(*e.editWorld.scene);
		if (e.gameSession) e.StartPlay(*this);
		exitCode = e.Loop(*this);
		e.StopPlay(*this);
		if (!e.gameSession)
		{
			ConfirmDiscardSceneChanges();   // the window is closing either way: Cancel means don't save
			e.SaveEditorState();
			e.editor.SaveSettings(SavedDir() / "EditorCamera.txt");
		}
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Fatal: %s", ex.what());
		cherry::ShowError("Cherry Engine - fatal error", ex.what(), e.window.Handle());
		exitCode = -3;
	}

	if (e.editWorld) e.NotifySystemsUnload();
	e.ui.Shutdown();
	e.editor.Shutdown(*this);
	e.DestroyWorld(e.playWorld);
	e.active = &e.editWorld;
	e.DestroyWorld(e.editWorld);
	e.UnloadModule();
	e.assets.Shutdown();
	e.renderer.Shutdown();
	e.window.Destroy();
	return exitCode;
}

int Engine::Impl::Loop(Engine& engine)
{
	using clock = std::chrono::steady_clock;
	auto start = clock::now();
	auto previous = start;
	float titleTimer = 0.0f;
	uint32_t titleFrames = 0;

	bool previousEditorMode = editorMode;
	uint32_t cookFrames = 0;
	while (!quitRequested)
	{
		// Shader cooking (--cook-shaders): every shader in Content, then quit once the game has
		// run a while and nothing is compiling any more.
		if (!launch.cookShaders.empty())
		{
			++cookFrames;
			if (cookFrames == 5)
			{
				for (const char* type : { "shader", "shadergraph" })
					for (const std::string& path : assets.List(type)) assets.GetSurfaceShader(path);
			}
			if (cookFrames == 10) renderer.BuildAllShaderVariants();
			if (cookFrames > 10 && std::chrono::duration<float>(clock::now() - start).count() > 6.0f && !renderer.IsCompilingShaders())
			{
				CHERRY_LOG("Cooked %u shaders into %s", ShaderCompiler::CookedCount(), ShaderCompiler::CookDirectory().string().c_str());
				quitRequested = true;
			}
		}

		input.BeginFrame();
		if (!window.PumpMessages())
		{
			break;
		}
		if (window.IsMinimized())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			previous = clock::now();
			continue;
		}

		const auto now = clock::now();
		deltaTime = std::min(std::chrono::duration<float>(now - previous).count(), 0.25f);
		totalTime = std::chrono::duration<float>(now - start).count();
		previous = now;

		// Play / stop / reload happen here, between frames, never in the middle of one.
		UpdateModuleWatch(deltaTime);
		ProcessRequests(engine);

		renderer.BeginFrame(totalTime, deltaTime);

		if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
		{
			editorMode = !editorMode;
		}
		if (editorMode != previousEditorMode)
		{
			// The view moves and the editor needs the cursor; the game can capture again.
			input.SetMouseCaptured(false);
			previousEditorMode = editorMode;
		}

		// UI first: it decides where the game view is and who owns input this frame.
		IGameModule* runningGame = playState != PlayState::Edit ? game : nullptr;
		editor.Draw(engine, runningGame, editorMode, gameUiOverlayWindow, deltaTime * 1000.0f);
		// Keyboard goes to the game unless the user is typing into an ImGui text field.
		// (WantCaptureKeyboard is too broad: with keyboard navigation enabled it is set
		// whenever any ImGui window has focus.)
		const ImGuiIO& io = ImGui::GetIO();
		if (editorMode)
		{
			gameView = { editor.viewportPosition.x, editor.viewportPosition.y, editor.viewportSize.x, editor.viewportSize.y };
			input.SetRouting(editor.viewportHovered, editor.viewportFocused || editor.viewportHovered,
				gameView.x, gameView.y, gameView.width, gameView.height, io.WantTextInput);
		}
		else
		{
			gameView = { 0.0f, 0.0f, static_cast<float>(window.Width()), static_cast<float>(window.Height()) };
			input.SetRouting(!io.WantCaptureMouse, true, gameView.x, gameView.y, gameView.width, gameView.height, io.WantTextInput);
		}
		renderer.SetViewSize(static_cast<uint32_t>(gameView.width), static_cast<uint32_t>(gameView.height));
		// The game's UI gets the mouse before the game does (and keeps it from the game while over it).
		ui.ProcessInput(input, static_cast<uint32_t>(gameView.width), static_cast<uint32_t>(gameView.height), ImGui::GetStyle().FontScaleDpi);
		if (playState != PlayState::Edit && ImGui::IsKeyPressed(ImGuiKey_F11, false)) ui.SetDebuggerVisible(!ui.IsDebuggerVisible());

		if (playState == PlayState::Playing && game)
		{
			try
			{
				game->OnUpdate(engine, deltaTime);
				for (auto& system : systems) system->OnUpdate(engine, deltaTime);
			}
			catch (const std::exception& ex)
			{
				if (gameSession) throw;
				CHERRY_ERROR("Game OnUpdate threw: %s (stopping)", ex.what());
				wantStop = true;
			}

			if (playState == PlayState::Playing) RunScripts(engine, deltaTime);

			// Fixed-step physics, then move rigid-body entities to their (interpolated) poses.
			PhysicsWorld& physics = *active->physics;
			physics.Update(deltaTime);
			Scene& scene = *active->scene;
			for (Entity entity : scene.RigidBodyEntities())
			{
				const RigidBody& body = scene.Get<RigidBody>(entity);
				Transform& transform = scene.Get<Transform>(entity);
				physics.GetTransform(body.body, transform.position, transform.rotation, true);
			}
		}

		// Asset files changed on disk: reload, and point both worlds at replaced meshes.
		assetWatchTimer += deltaTime;
		if (assetWatchTimer > 1.0f)
		{
			assetWatchTimer = 0.0f;
			std::vector<std::pair<MeshHandle, MeshHandle>> replaced;
			assets.PollChanges(replaced);
			for (World* world : { &editWorld, &playWorld })
			{
				if (!*world) continue;
				for (Entity entity : world->scene->MeshEntities())
				{
					MeshRenderer& renderer = world->scene->Get<MeshRenderer>(entity);
					for (const auto& [old, fresh] : replaced)
					{
						if (renderer.mesh == old) renderer.mesh = fresh;
						if (renderer.glassMesh == old) renderer.glassMesh = fresh;
					}
				}
			}
		}
		if (playState == PlayState::Edit)
		{
			for (auto& system : systems)
			{
				if (!system->RunsInEditMode()) continue;
				try { system->OnEditUpdate(engine, deltaTime); }
				catch (const std::exception& ex) { CHERRY_ERROR("System OnEditUpdate threw: %s", ex.what()); }
			}
		}
		ResolveAssets(*active->scene);
		if (playState == PlayState::Edit) ApplyEnvironment(*active->scene);
		ui.Update(deltaTime);   // after the game's update: its data changes show this frame
		particles.Update(*active->scene, deltaTime);
		if (UpdateMainView())
		{
			particles.Submit(*active->scene, renderer);
			renderer.RenderScene(*active->scene, mainView);
			if (Rml::Context* uiContext = ui.GetContext()) renderer.RenderUi(*uiContext);
		}
		renderer.EndFrame(!editorMode);

		if (leakCheckFrames >= 0 && leakCheckFrames-- == 0 && playState == PlayState::Edit)
		{
			const uint32_t now = renderer.Stats().descriptorsInUse;
			if (now > descriptorsBeforePlay) CHERRY_WARN("After Stop: %u GPU descriptors in use, %u before Play (the game leaked resources)", now, descriptorsBeforePlay);
			else CHERRY_LOG("After Stop: %u GPU descriptors in use (%u before Play)", now, descriptorsBeforePlay);
		}

		titleTimer += deltaTime;
		++titleFrames;
		if (titleTimer >= 0.5f)
		{
			const wchar_t* state = playState == PlayState::Playing ? L"playing" : playState == PlayState::Paused ? L"paused" : L"editing";
			wchar_t title[256];
			std::wstring sceneName = scenePath.empty() ? std::wstring(L"untitled") : scenePath.stem().wstring();
			if (sceneDirty) sceneName += L"*";
			std::swprintf(title, sizeof(title) / sizeof(title[0]), L"%ls - %ls  |  %.0f fps  %.2f ms  |  GPU %.2f ms  |  %ls, %ls (F1)", baseTitle.c_str(), sceneName.c_str(),
				titleFrames / titleTimer, 1000.0f * titleTimer / titleFrames, renderer.Stats().gpuFrameMs,
				state, editorMode ? L"editor" : L"game view");
			window.SetTitle(title);
			titleTimer = 0.0f;
			titleFrames = 0;
		}
	}
	return 0;
}

Ray Engine::ScreenPointToRay(const XMFLOAT2& viewPixel) const
{
	const Impl& e = *mImpl;
	const float width = static_cast<float>(std::max(e.renderer.ViewWidth(), 1u));
	const float height = static_cast<float>(std::max(e.renderer.ViewHeight(), 1u));
	const XMMATRIX viewProj = XMLoadFloat4x4(&e.mainView.view) * XMLoadFloat4x4(&e.mainView.proj);
	const XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

	const float ndcX = (viewPixel.x + 0.5f) / width * 2.0f - 1.0f;
	const float ndcY = 1.0f - (viewPixel.y + 0.5f) / height * 2.0f;
	const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
	const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);

	Ray ray;
	ray.origin = e.mainView.position;
	if (e.mainView.proj._44 == 1.0f)   // orthographic: parallel rays, each from its own point
		XMStoreFloat3(&ray.origin, nearPoint);
	XMStoreFloat3(&ray.direction, XMVector3Normalize(farPoint - nearPoint));
	return ray;
}

std::optional<RaycastHit> Engine::Raycast(const Ray& ray) const
{
	Scene& scene = *mImpl->active->scene;
	const Renderer& renderer = mImpl->renderer;
	std::optional<RaycastHit> best;

	const XMVECTOR originW = XMLoadFloat3(&ray.origin);
	const XMVECTOR dirW = XMVector3Normalize(XMLoadFloat3(&ray.direction));

	for (Entity entity : scene.MeshEntities())
	{
		const MeshRenderer& meshRenderer = scene.Get<MeshRenderer>(entity);
		if (!meshRenderer.visible) continue;
		const BoundingBox* bounds = renderer.GetMeshBounds(meshRenderer.mesh);
		if (!bounds) continue;

		// Intersect in the mesh's local space. The local direction is not renormalized,
		// so t is a world-space distance.
		const XMMATRIX world = scene.WorldMatrix(entity);
		const XMMATRIX invWorld = XMMatrixInverse(nullptr, world);
		XMFLOAT3 o, d;
		XMStoreFloat3(&o, XMVector3TransformCoord(originW, invWorld));
		XMStoreFloat3(&d, XMVector3TransformNormal(dirW, invWorld));

		const float origin[3] = { o.x, o.y, o.z };
		const float dir[3] = { d.x, d.y, d.z };
		const float minB[3] = { bounds->Center.x - bounds->Extents.x, bounds->Center.y - bounds->Extents.y, bounds->Center.z - bounds->Extents.z };
		const float maxB[3] = { bounds->Center.x + bounds->Extents.x, bounds->Center.y + bounds->Extents.y, bounds->Center.z + bounds->Extents.z };

		float tEnter = -FLT_MAX, tExit = FLT_MAX;
		int enterAxis = 0;
		float enterSign = -1.0f;
		bool miss = false;
		for (int axis = 0; axis < 3 && !miss; ++axis)
		{
			if (std::fabs(dir[axis]) < 1e-8f)
			{
				miss = origin[axis] < minB[axis] || origin[axis] > maxB[axis];
				continue;
			}
			float t0 = (minB[axis] - origin[axis]) / dir[axis];
			float t1 = (maxB[axis] - origin[axis]) / dir[axis];
			float sign = -1.0f;              // entering through the min face
			if (t0 > t1) { std::swap(t0, t1); sign = 1.0f; }
			if (t0 > tEnter) { tEnter = t0; enterAxis = axis; enterSign = sign; }
			tExit = std::min(tExit, t1);
			miss = tEnter > tExit;
		}
		if (miss || tExit < 0.0f) continue;
		const float t = std::max(tEnter, 0.0f);
		if (best && t >= best->distance) continue;

		XMFLOAT3 normalLocal{ 0, 0, 0 };
		(&normalLocal.x)[enterAxis] = enterSign;
		RaycastHit hit;
		hit.entity = entity;
		hit.distance = t;
		XMStoreFloat3(&hit.point, originW + dirW * t);
		XMStoreFloat3(&hit.normal, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&normalLocal), XMMatrixTranspose(invWorld))));
		best = hit;
	}
	return best;
}
