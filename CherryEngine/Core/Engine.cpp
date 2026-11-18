#include "Core/Engine.h"
#include "Core/Log.h"
#include "Core/ModuleHost.h"
#include "Core/BuildProcess.h"
#include "Game/GameModule.h"
#include "Editor/EditorLayer.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>

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
}

struct Engine::Impl
{
	Window window;
	Input input;
	Renderer renderer;
	EditorLayer editor;

	World editWorld;
	World playWorld;
	World* active = &editWorld;

	// Project and game module
	ProjectDesc project;
	ModuleHost host;
	IGameModule* staticGame = nullptr;
	IGameModule* game = nullptr;          // the loaded game, or null
	std::string moduleError;
	BuildProcess build;
	bool lastBuildFailed = false;
	uint32_t reloads = 0;
	float watchTimer = 0.0f;

	// Play mode
	PlayState playState = PlayState::Edit;
	bool gameSession = false;             // launched to play: quitting the game closes the window
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
	World MakeWorld();
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
Renderer& Engine::GetRenderer() { return mImpl->renderer; }
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
void Engine::Play() { mImpl->wantPlay = true; }
void Engine::Stop() { mImpl->wantStop = true; }
void Engine::SetPaused(bool paused) { mImpl->wantPause = true; mImpl->pauseRequested = paused; }

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

void Engine::CompileModule()
{
	Impl& e = *mImpl;
	if (!e.project.IsValid() || e.staticGame || e.build.IsRunning()) return;
#if defined(CHERRY_CMAKE_COMMAND) && defined(CHERRY_BUILD_DIR) && defined(CHERRY_BUILD_CONFIG)
	// /nodeReuse:false: no MSBuild node outlives the build holding our output pipe.
	const std::wstring command = L"\"" + Widen(CHERRY_CMAKE_COMMAND) + L"\" --build \"" + Widen(CHERRY_BUILD_DIR)
		+ L"\" --config " + Widen(CHERRY_BUILD_CONFIG) + L" --target " + Widen(e.project.module)
		+ L" -- /nodeReuse:false /verbosity:minimal /nologo";
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

void Engine::OpenProject(const fs::path& projectFile)
{
	// A fresh process is the simplest way to get a clean engine, ImGui layout and module.
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::wstring commandLine = L"\"" + std::wstring(exePath) + L"\" \"" + fs::absolute(projectFile).wstring() + L"\"";
	STARTUPINFOW startup{ sizeof(startup) };
	PROCESS_INFORMATION info{};
	if (!CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
	{
		CHERRY_ERROR("Couldn't start the editor for %s (error %lu)", projectFile.string().c_str(), GetLastError());
		return;
	}
	CloseHandle(info.hThread);
	CloseHandle(info.hProcess);
	mImpl->quitRequested = true;
}

bool Engine::Impl::ResolveProject(const LaunchOptions& options, std::string& error)
{
	if (options.staticGame)
	{
		// Packaged game: content was copied next to the executable.
		const fs::path exeDir = cherry::ExecutableDir();
		project.name = "Game";
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
		return;
	}
	if (!project.IsValid()) return;
	const fs::path dll = cherry::ExecutableDir() / (project.module + ".dll");
	if (host.Load(dll, project.saved / "Modules", moduleError))
	{
		game = host.Game();
	}
	else
	{
		CHERRY_WARN("Game module: %s", moduleError.c_str());
	}
}

World Engine::Impl::MakeWorld()
{
	World world;
	world.physics = std::make_unique<PhysicsWorld>();
	world.physics->Initialize();
	world.scene = std::make_unique<Scene>();

	world.camera = world.scene->CreateEntity();
	Transform transform;
	transform.position = { 0.0f, 6.0f, -15.0f };
	transform.LookAt({ 0, 0, 0 });
	world.scene->Add(world.camera, transform);
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
	if (!game || playState != PlayState::Edit) return;
	editor.OnWorldChanging(engine);
	settingsBeforePlay = renderer.Settings();
	descriptorsBeforePlay = renderer.Stats().descriptorsInUse;
	playWorld = MakeWorld();
	active = &playWorld;
	playState = PlayState::Playing;
	CHERRY_LOG("Play");
	try
	{
		game->OnStartup(engine);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Game OnStartup threw: %s", ex.what());
		wantStop = true;
	}
}

void Engine::Impl::StopPlay(Engine& engine)
{
	if (playState == PlayState::Edit) return;
	try
	{
		if (game) game->OnShutdown(engine);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Game OnShutdown threw: %s", ex.what());
	}
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
			StopPlay(engine);
			host.Unload();
			game = nullptr;
			LoadModule();
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
	if (!scene.IsAlive(camera) || !scene.Has<Camera>(camera) || !scene.Has<Transform>(camera))
	{
		return false;
	}
	const Transform& transform = scene.Get<Transform>(camera);
	const Camera& cameraData = scene.Get<Camera>(camera);

	// View = inverse of the camera's rigid transform (scale ignored).
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&transform.rotation))
		* XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
	const float aspect = static_cast<float>(renderer.ViewWidth()) / static_cast<float>(std::max(renderer.ViewHeight(), 1u));

	XMStoreFloat4x4(&mainView.view, XMMatrixInverse(nullptr, world));
	XMStoreFloat4x4(&mainView.proj, XMMatrixPerspectiveFovLH(cameraData.fovY, aspect, cameraData.nearZ, cameraData.farZ));
	mainView.position = transform.position;
	mainView.nearZ = cameraData.nearZ;
	mainView.farZ = cameraData.farZ;
	return true;
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
	e.staticGame = options.staticGame;
	e.gameSession = options.play || options.staticGame || !options.moduleName.empty();

	std::string error;
	if (!e.ResolveProject(options, error))
	{
		CHERRY_ERROR("%s", error.c_str());
		MessageBoxA(nullptr, error.c_str(), "Cherry Engine", MB_ICONERROR);
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
	e.window.SetMessageHandler([&e](UINT msg, WPARAM wParam, LPARAM lParam) { e.input.HandleMessage(msg, wParam, lParam); });
	e.window.SetResizeHandler([&e](uint32_t width, uint32_t height) { e.renderer.OnWindowResized(width, height); });

	int exitCode = 0;
	try
	{
		if (!e.renderer.Initialize(e.window.Handle(), e.window.Width(), e.window.Height(), error))
		{
			CHERRY_ERROR("Renderer initialization failed: %s", error.c_str());
			MessageBoxA(e.window.Handle(), error.c_str(), "Cherry Engine", MB_ICONERROR);
			e.window.Destroy();
			return -2;
		}
		// Each project keeps its own window layout. ImGui reads the file on the first frame.
		e.imguiIniPath = (SavedDir() / "imgui.ini").string();
		ImGui::GetIO().IniFilename = e.imguiIniPath.c_str();

		e.editWorld = e.MakeWorld();
		e.active = &e.editWorld;
		if (e.gameSession) e.StartPlay(*this);
		exitCode = e.Loop(*this);
		e.StopPlay(*this);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Fatal: %s", ex.what());
		MessageBoxA(e.window.Handle(), ex.what(), "Cherry Engine - fatal error", MB_ICONERROR);
		exitCode = -3;
	}

	e.editor.Shutdown(*this);
	e.DestroyWorld(e.playWorld);
	e.active = &e.editWorld;
	e.DestroyWorld(e.editWorld);
	e.game = nullptr;
	e.host.Unload();
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
	while (!quitRequested)
	{
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

		if (playState == PlayState::Playing && game)
		{
			try
			{
				game->OnUpdate(engine, deltaTime);
			}
			catch (const std::exception& ex)
			{
				if (gameSession) throw;
				CHERRY_ERROR("Game OnUpdate threw: %s (stopping)", ex.what());
				wantStop = true;
			}

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

		if (UpdateMainView())
		{
			renderer.RenderScene(*active->scene, mainView);
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
			swprintf_s(title, L"%s  |  %.0f fps  %.2f ms  |  GPU %.2f ms  |  %s, %s (F1)", baseTitle.c_str(),
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
		const XMMATRIX world = scene.Get<Transform>(entity).Matrix();
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
