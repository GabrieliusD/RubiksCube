#include "Core/Engine.h"
#include "Core/Log.h"
#include "Game/GameModule.h"
#include "Editor/EditorLayer.h"
#include "imgui.h"

#include <chrono>
#include <thread>
#include <cmath>

using namespace DirectX;

struct Engine::Impl
{
	Window window;
	Input input;
	Renderer renderer;
	PhysicsWorld physics;
	std::unique_ptr<Scene> scene;
	EditorLayer editor;

	Entity mainCamera = INVALID_ENTITY;
	RenderView mainView{};
	ViewRect gameView{};
	bool editorMode = true;
	bool quitRequested = false;
	float deltaTime = 0.0f;
	float totalTime = 0.0f;
	std::wstring baseTitle;
	bool gameUiOverlayWindow = true;

	void CreateDefaultCamera();
	bool UpdateMainView();
	int Loop(Engine& engine, IGameModule& game);
};

Engine::Engine() : mImpl(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

Scene& Engine::GetScene() { return *mImpl->scene; }
Renderer& Engine::GetRenderer() { return mImpl->renderer; }
Input& Engine::GetInput() { return mImpl->input; }
Window& Engine::GetWindow() { return mImpl->window; }
PhysicsWorld& Engine::GetPhysics() { return mImpl->physics; }
float Engine::DeltaTime() const { return mImpl->deltaTime; }
float Engine::TotalTime() const { return mImpl->totalTime; }
Entity Engine::MainCamera() const { return mImpl->mainCamera; }
void Engine::SetMainCamera(Entity camera) { mImpl->mainCamera = camera; }
bool Engine::IsEditorMode() const { return mImpl->editorMode; }
ViewRect Engine::GameViewRect() const { return mImpl->gameView; }
void Engine::SetEditorMode(bool editor) { mImpl->editorMode = editor; }
void Engine::RequestQuit() { mImpl->quitRequested = true; }

void Engine::Impl::CreateDefaultCamera()
{
	mainCamera = scene->CreateEntity();
	Transform transform;
	transform.position = { 0.0f, 6.0f, -15.0f };
	transform.LookAt({ 0, 0, 0 });
	scene->Add(mainCamera, transform);
	scene->Add(mainCamera, Camera{});
}

bool Engine::Impl::UpdateMainView()
{
	if (!scene->IsAlive(mainCamera) || !scene->Has<Camera>(mainCamera) || !scene->Has<Transform>(mainCamera))
	{
		return false;
	}
	const Transform& transform = scene->Get<Transform>(mainCamera);
	const Camera& camera = scene->Get<Camera>(mainCamera);

	// View = inverse of the camera's rigid transform (scale ignored).
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&transform.rotation))
		* XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
	const float aspect = static_cast<float>(renderer.ViewWidth()) / static_cast<float>(std::max(renderer.ViewHeight(), 1u));

	XMStoreFloat4x4(&mainView.view, XMMatrixInverse(nullptr, world));
	XMStoreFloat4x4(&mainView.proj, XMMatrixPerspectiveFovLH(camera.fovY, aspect, camera.nearZ, camera.farZ));
	mainView.position = transform.position;
	mainView.nearZ = camera.nearZ;
	mainView.farZ = camera.farZ;
	return true;
}

int Engine::Run(IGameModule& game)
{
	Impl& e = *mImpl;
	const EngineConfig config = game.GetConfig();
	e.editorMode = config.startInEditor;
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
		std::string error;
		if (!e.renderer.Initialize(e.window.Handle(), e.window.Width(), e.window.Height(), error))
		{
			CHERRY_ERROR("Renderer initialization failed: %s", error.c_str());
			MessageBoxA(e.window.Handle(), error.c_str(), "Cherry Engine", MB_ICONERROR);
			e.window.Destroy();
			return -2;
		}

		e.physics.Initialize();
		e.scene = std::make_unique<Scene>();
		e.CreateDefaultCamera();
		game.OnStartup(*this);
		exitCode = e.Loop(*this, game);
		game.OnShutdown(*this);
	}
	catch (const std::exception& ex)
	{
		CHERRY_ERROR("Fatal: %s", ex.what());
		MessageBoxA(e.window.Handle(), ex.what(), "Cherry Engine - fatal error", MB_ICONERROR);
		exitCode = -3;
	}

	e.editor.Shutdown(*this);
	if (e.scene) e.scene->Clear();
	e.physics.Shutdown();
	e.renderer.Shutdown();
	e.scene.reset();
	e.window.Destroy();
	return exitCode;
}

int Engine::Impl::Loop(Engine& engine, IGameModule& game)
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
		editor.Draw(engine, game, editorMode, gameUiOverlayWindow, deltaTime * 1000.0f);
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

		game.OnUpdate(engine, deltaTime);

		// Fixed-step physics, then move rigid-body entities to their (interpolated) poses.
		physics.Update(deltaTime);
		for (Entity entity : scene->RigidBodyEntities())
		{
			const RigidBody& body = scene->Get<RigidBody>(entity);
			Transform& transform = scene->Get<Transform>(entity);
			physics.GetTransform(body.body, transform.position, transform.rotation, true);
		}

		if (UpdateMainView())
		{
			renderer.RenderScene(*scene, mainView);
		}
		renderer.EndFrame(!editorMode);

		titleTimer += deltaTime;
		++titleFrames;
		if (titleTimer >= 0.5f)
		{
			wchar_t title[256];
			swprintf_s(title, L"%s  |  %.0f fps  %.2f ms  |  GPU %.2f ms  |  %s (F1)", baseTitle.c_str(),
				titleFrames / titleTimer, 1000.0f * titleTimer / titleFrames, renderer.Stats().gpuFrameMs,
				editorMode ? L"editor" : L"game");
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
	Scene& scene = *mImpl->scene;
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
