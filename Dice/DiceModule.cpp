#include "DiceModule.h"
#include "DiceVisuals.h"
#include "DiceControls.h"
#include <fstream>
#include <sstream>
#include <Core/Log.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace DirectX;

CHERRY_IMPLEMENT_GAME_MODULE(DiceModule)

namespace
{
	// Development switches, read at start-up (Tools/Screenshot.ps1 sets them):
	//   DICE_DEV_FACE=1..6   deal every die with that face up (checks the face-up mapping)
	//   DICE_DEV_SEED=N      a repeatable deal
	//   DICE_DEV_DENSITY=P   dice density in percent (100: every slot, 1,600 dice on the default board)
	//   DICE_DEV_BOARD=N     board size
	//   DICE_DEV_SESSION=S   start a session of S seconds at once (no click on Start)
	//   DICE_DEV_MODE=M      collision mode: 0 nudge, 1 bump, 2 stack
	//   DICE_DEV_LAYOUT=L    0 square, 1 hex, 2 scatter;  DICE_DEV_SHAPE=S  0 full, 1 circle, 2 ring, 3 islands
	//   DICE_DEV_PRESET=name load Content/Presets/<name>.json first (the switches above then apply on top)
	//   DICE_DEV_STORM=P     launch P% of the dice per second at random (profiling load)
	//   DICE_DEV_PROFILE=1   vsync off; log "DICE_PROFILE ..." timings every 2 s
	//   DICE_DEV_POINTER="px py [px2 py2 seconds]"  a pretend mouse at view pixels (moving to the
	//                        second point and back over `seconds`), for hover checks without a person
	// An environment variable, or "" when it isn't set.
	std::string GetEnv(const char* name)
	{
#if defined(_MSC_VER)
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0 || !value) return {};
		std::string result(value);
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name);
		return value ? std::string(value) : std::string();
#endif
	}

	int EnvInt(const char* name, int fallback)
	{
		const std::string value = GetEnv(name);
		return value.empty() ? fallback : std::atoi(value.c_str());
	}

	// The reference's camera: orthographic, 30 degrees down, 45 degrees round, 300 units out.
	constexpr double kCameraPitch = 30.0 * dice::kPi / 180.0, kCameraYaw = 45.0 * dice::kPi / 180.0, kCameraDistance = 300.0;
}

EngineConfig DiceModule::GetConfig() const
{
	EngineConfig config;
	config.windowTitle = L"Dice Roll Feel Test - Cherry Engine";
	config.windowWidth = 1600;
	config.windowHeight = 900;
	config.startInEditor = false;   // --play: the full-window game view
	config.gameUiOverlayWindow = false;   // the game draws its own HUD and panel
	return config;
}

void DiceModule::OnStartup(Engine& engine)
{
	Renderer& renderer = engine.GetRenderer();
	// A preset first, then the hot-reloaded config (the previous module's SaveState), then the dev switches.
	{
		if (const std::string value = GetEnv("DICE_DEV_PRESET"); !value.empty())
		{
			std::ifstream file(engine.ContentPath(L"Presets/" + std::wstring(value.begin(), value.end()) + L".json"));
			std::stringstream text;
			text << file.rdbuf();
			std::vector<std::string> problems;
			DiceConfig config;
			if (file && dice::ConfigFromJson(text.str(), config, problems)) mSim.config = config;
			else CHERRY_WARN("Dice: preset %s not found or not valid", value.c_str());
		}
	}
	if (!engine.HotReloadState().empty())
	{
		std::vector<std::string> problems;
		DiceConfig config = mSim.config;
		if (dice::ConfigFromJson(engine.HotReloadState(), config, problems)) mSim.config = config;
	}
	mForcedFace = EnvInt("DICE_DEV_FACE", 0);
	mSim.stormPerSecond = EnvInt("DICE_DEV_STORM", 0) / 100.0;
	mProfile = EnvInt("DICE_DEV_PROFILE", 0) != 0;
	const int seed = EnvInt("DICE_DEV_SEED", 0);
	if (seed != 0) mSim.rand.Seed(static_cast<uint64_t>(seed));
	const int density = EnvInt("DICE_DEV_DENSITY", 0);
	if (density > 0) mSim.config.density = std::clamp(density / 100.0f, 0.05f, 1.0f);
	if (const int mode = EnvInt("DICE_DEV_MODE", -1); mode >= 0 && mode <= 2) mSim.config.collisionMode = static_cast<CollisionMode>(mode);
	if (const int layout = EnvInt("DICE_DEV_LAYOUT", -1); layout >= 0 && layout <= 2) mSim.config.layout = static_cast<Layout>(layout);
	if (const int shape = EnvInt("DICE_DEV_SHAPE", -1); shape >= 0 && shape <= 3) mSim.config.boardShape = static_cast<BoardShape>(shape);
	const int board = EnvInt("DICE_DEV_BOARD", 0);
	if (board > 0) mSim.config.boardSize = std::clamp(board, 6, kMaxProfileSide);
	{
		if (const std::string value = GetEnv("DICE_DEV_POINTER"); !value.empty())
		{
			float p[5] = { 0, 0, 0, 0, 0 };
#if defined(_MSC_VER)
			const int n = sscanf_s(value.c_str(), "%f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4]);
#else
			const int n = std::sscanf(value.c_str(), "%f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4]);
#endif
			if (n >= 2)
			{
				mDevPointer = true;
				mDevFrom = { p[0], p[1] };
				mDevTo = n >= 5 ? XMFLOAT2(p[2], p[3]) : mDevFrom;
				mDevSeconds = n >= 5 ? std::max(p[4], 0.1f) : 1.0f;
			}
		}
	}

	// Die: the face atlas on a rough white plastic (MeshStandardMaterial roughness 0.45).
	const dice::Image atlas = dice::MakeAtlas();
	mAtlas = renderer.CreateTexture(atlas.width, atlas.height, atlas.rgba.data(), true);
	MaterialDesc die;
	die.roughness = 0.45f;
	die.albedoTexture = mAtlas;
	mDieMaterial = renderer.CreateMaterial(die);
	MeshData dieMesh = dice::MakeDieMesh(mSim.config.dieSize);
	dice::ToEngine(dieMesh);
	mDieMesh = renderer.CreateMesh(dieMesh);

	const dice::Image grid = dice::MakeGridTexture();
	mGrid = renderer.CreateTexture(grid.width, grid.height, grid.rgba.data(), true);
	MaterialDesc felt;
	felt.roughness = 0.95f;
	felt.albedoTexture = mGrid;
	mFeltMaterial = renderer.CreateMaterial(felt);
	MaterialDesc plain;
	plain.baseColor = dice::SrgbToLinear(0x2a5549);   // plain felt for every board but the classic one
	plain.roughness = 0.95f;
	mPlainFeltMaterial = renderer.CreateMaterial(plain);
	MaterialDesc marker;
	marker.baseColor = dice::SrgbToLinear(0x3b6e60);
	marker.roughness = 0.95f;
	mMarkerMaterial = renderer.CreateMaterial(marker);
	MaterialDesc base;
	base.baseColor = dice::SrgbToLinear(0x5a3e2b);
	base.roughness = 0.8f;
	mBaseMaterial = renderer.CreateMaterial(base);

	// Camera: the reference's position, mirrored into the engine's space (DiceVisuals.h).
	Scene& scene = engine.GetScene();
	const Entity camera = engine.MainCamera();
	if (scene.IsAlive(camera) && scene.Has<Camera>(camera) && scene.Has<Transform>(camera))
	{
		Camera& c = scene.Get<Camera>(camera);
		c.orthographic = true;
		c.nearZ = 1.0f;
		c.farZ = 1000.0f;
		const dice::Vec3 eye = dice::ToEngine({ kCameraDistance * std::cos(kCameraPitch) * std::sin(kCameraYaw),
			kCameraDistance * std::sin(kCameraPitch), kCameraDistance * std::cos(kCameraPitch) * std::cos(kCameraYaw) });
		Transform& t = scene.Get<Transform>(camera);
		t.position = { static_cast<float>(eye.x), static_cast<float>(eye.y), static_cast<float>(eye.z) };
		t.LookAt({ 0, 0, 0 });
	}
	else
		CHERRY_ERROR("Dice: no main camera");

	LoadFonts();
	BuildBoard(engine);
	if (const int seconds = EnvInt("DICE_DEV_SESSION", 0); seconds > 0)
	{
		mSim.config.sessionSeconds = static_cast<float>(seconds);
		StartSession();
	}
	CHERRY_LOG("DICE: %d dice on %d slots (board %d, face check %d)", mSim.diceCount, mSim.slotCount, mSim.N, mForcedFace);
}

// B4 "Rebuilding the board": slots, dice, board visuals, lights and the camera fit.
void DiceModule::BuildBoard(Engine& engine)
{
	mSim.BuildBoard(mForcedFace);
	mPopups.clear();   // B13.5: no stale effects after a rebuild
	ClearSparks();
	Renderer& renderer = engine.GetRenderer();
	RebuildBoardMeshes(renderer);
	SetupLighting(renderer);
	FitCamera(engine);
}

void DiceModule::RebuildBoardMeshes(Renderer& renderer)
{
	if (mFeltMesh.IsValid()) renderer.DestroyMesh(mFeltMesh);
	if (mBaseMesh.IsValid()) renderer.DestroyMesh(mBaseMesh);
	MeshData felt = dice::MakeFeltMesh(mSim.N);
	dice::ToEngine(felt);
	mFeltMesh = renderer.CreateMesh(felt);
	MeshData base = dice::MakeBaseMesh(mSim.N);
	dice::ToEngine(base);
	mBaseMesh = renderer.CreateMesh(base);

	// Classic square grid keeps the line texture; every other layout or shape gets plain felt
	// plus one flat marker per slot, so the shape and spacing read clearly.
	if (mMarkerMesh.IsValid()) { renderer.DestroyMesh(mMarkerMesh); mMarkerMesh = {}; }
	if (!ClassicBoard())
	{
		MeshData marker = mSim.config.layout == Layout::Hex ? dice::MakeDiscMesh(0.52, 6, dice::kPi / 2)
			: mSim.config.layout == Layout::Scatter ? dice::MakeDiscMesh(0.38, 18, 0) : dice::MakeDiscMesh(0.62, 4, dice::kPi / 4);
		dice::ToEngine(marker);
		mMarkerMesh = renderer.CreateMesh(marker);
	}
}

// B2 "Lighting". three.js (r160, physical lights) divides by pi where the engine's
// strengths already include it: the sun's 2.0 becomes 2 / pi, and the hemisphere light
// (1.1 x white from above, 1.1 x #4a5262 from below) becomes an ambient of 1.1 / pi
// split up/down by ambientContrast.
void DiceModule::SetupLighting(Renderer& renderer)
{
	RenderSettings& s = renderer.Settings();
	const float N = static_cast<float>(mSim.N);
	// The reference has no tone mapping (three.js NoToneMapping): colours are only clipped.
	s.toneCurve = false;
	s.clearColor = dice::SrgbToLinear(0x14161a);
	s.sky.enabled = false;
	s.fogEnabled = false;
	s.heightFog.enabled = false;
	s.ssao.enabled = false;
	s.lightShafts.enabled = false;
	s.edgeHighlights.enabled = false;
	s.clouds.enabled = false;
	s.bloom.enabled = false;
	s.eyeAdaptation.enabled = false;
	s.colorGrade.enabled = false;
	s.interior.enabled = false;
	s.rooms.clear();
	s.exposure = 1.0f;
	s.fillLightScale = 0.0f;

	const float sun = static_cast<float>(2.0 / dice::kPi);
	s.sunColor = { sun, sun, sun };
	// From (-0.45N, 1.2N, 0.3N) towards the origin, mirrored.
	const dice::Vec3 d = dice::ToEngine({ 0.45, -1.2, -0.3 });
	XMStoreFloat3(&s.sunDirection, XMVector3Normalize(XMVectorSet(static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z), 0)));

	// #4a5262 is about 15% of white (linear; the engine's ground ambient has one share for
	// all channels, so it comes out a touch less blue than the reference's).
	const float sky = static_cast<float>(1.1 / dice::kPi);
	const float contrast = 0.85f;
	s.ambientContrast = contrast;
	s.ambient = { sky / (1 + contrast), sky / (1 + contrast), sky / (1 + contrast) };

	// Shadows fitted to the board (the reference's shadow box is +-0.8N around the sun's view of it).
	s.shadows.enabled = true;
	s.shadows.boundsCenter = { 0.0f, 1.0f, 0.0f };
	s.shadows.boundsExtents = { N / 2 + 0.4f, 2.5f, N / 2 + 0.4f };
	s.shadows.normalBias = 1.0f;
	s.shadows.fadeSeconds = 0.0f;
	s.shadows.direction = { 0, 0, 0 };
}

// B2: S = N + 1, halfHeight = max(S x 0.74 / aspect, S x 0.42), divided by the zoom.
void DiceModule::FitCamera(Engine& engine)
{
	Scene& scene = engine.GetScene();
	const Entity camera = engine.MainCamera();
	if (!scene.IsAlive(camera) || !scene.Has<Camera>(camera)) return;
	Renderer& renderer = engine.GetRenderer();
	const float aspect = static_cast<float>(std::max(renderer.ViewWidth(), 1u)) / static_cast<float>(std::max(renderer.ViewHeight(), 1u));
	const float S = static_cast<float>(mSim.N + 1);
	const float halfHeight = std::max(S * 0.74f / aspect, S * 0.42f);
	scene.Get<Camera>(camera).orthoHalfHeight = halfHeight / mZoom;
}

void DiceModule::OnUpdate(Engine& engine, float deltaTime)
{
	// Mouse wheel zoom: zoom *= exp(-deltaY x 0.0012), a browser wheel notch being 100 px of deltaY.
	Input& input = engine.GetInput();
	if (input.HasMouse() && input.WheelDelta() != 0.0f)
		mZoom = std::clamp(mZoom * std::exp(input.WheelDelta() * 100.0f * 0.0012f), 0.5f, 8.0f);
	FitCamera(engine);

	// B6: the whole frame up to the rendering runs in the simulation.
	bool inside = false;
	double hitX = 0, hitZ = 0;
	PointerOnBoard(engine, inside, hitX, hitZ);
	// The reference starts its audio on the first click or tap.
	if (input.WasMousePressed(MouseButton::Left) || input.WasMousePressed(MouseButton::Right) || input.WasMousePressed(MouseButton::Middle))
		mAudio.Start();

	using Clock = std::chrono::steady_clock;
	const Clock::time_point t0 = Clock::now();
	mSim.Frame(deltaTime, inside, hitX, hitZ);
	const Clock::time_point t1 = Clock::now();
	WriteInstances(engine.GetRenderer());
	const Clock::time_point t2 = Clock::now();
	if (mProfile)
	{
		engine.GetRenderer().Settings().vsync = false;
		mProfileSimMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
		mProfileWriteMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
		mProfileFrameMaxMs = std::max(mProfileFrameMaxMs, deltaTime * 1000.0);
		mProfileGpuMs += engine.GetRenderer().Stats().gpuFrameMs;
		mProfileTime += deltaTime;
		if (++mProfileFrames >= 10 && mProfileTime >= 2.0)
		{
			const RenderStats& stats = engine.GetRenderer().Stats();
			const double n = mProfileFrames;
			CHERRY_LOG("DICE_PROFILE dice %d rolling %d | fps %.1f frame %.2f ms (max %.2f) | sim %.3f ms, instances %.3f ms, rest of the frame %.2f ms | gpu %.2f ms | draws %u, instances %u",
				mSim.diceCount, mSim.activeCount, n / mProfileTime, mProfileTime * 1000.0 / n, mProfileFrameMaxMs, mProfileSimMs / n, mProfileWriteMs / n,
				mProfileTime * 1000.0 / n - (mProfileSimMs + mProfileWriteMs) / n, mProfileGpuMs / n, stats.drawCalls, stats.instancesSubmitted);
			mProfileTime = mProfileSimMs = mProfileWriteMs = mProfileFrameMaxMs = mProfileGpuMs = 0;
			mProfileFrames = 0;
		}
	}

	// Effects the frame asked for.
	TakePopups();
	for (dice::Sound sound : mSim.soundsOut) mAudio.Play(sound, mSim.config.volume, mSim.config.landTicks);
	mSim.soundsOut.clear();
	for (const dice::SparkRequest& request : mSim.sparksOut) SpawnSparks(request);
	mSim.sparksOut.clear();
	UpdateSparks(engine, std::min(0.05, std::max(0.0, static_cast<double>(deltaTime))));

	mFpsFrames++;
	mFpsTime += deltaTime;
	if (mFpsTime >= 0.5f)
	{
		mFps = static_cast<int>(std::lround(mFpsFrames / mFpsTime));
		mFpsFrames = 0;
		mFpsTime = 0.0f;
	}
}

// B9 "Rendering": every die's matrix and colour, straight from the arrays.
void DiceModule::WriteInstances(Renderer& renderer)
{
	using namespace dice;
	std::vector<MeshInstance>& list = renderer.MeshInstances();
	list.clear();

	MeshInstance board;
	board.castShadows = false;
	XMStoreFloat4x4(&board.world, XMMatrixIdentity());
	board.mesh = mFeltMesh;
	board.material = ClassicBoard() ? mFeltMaterial : mPlainFeltMaterial;
	list.push_back(board);
	board.mesh = mBaseMesh;
	board.material = mBaseMaterial;
	list.push_back(board);
	if (mMarkerMesh.IsValid())
	{
		MeshInstance marker = board;
		marker.mesh = mMarkerMesh;
		marker.material = mMarkerMaterial;
		for (int s = 0; s < mSim.slotCount; ++s)
		{
			marker.world._41 = mSim.slotX[s];
			marker.world._42 = 0.004f;
			marker.world._43 = -mSim.slotZ[s];   // mirrored (DiceVisuals.h)
			list.push_back(marker);
		}
	}

	DiceState& D = mSim.D;
	const double now = mSim.now;
	const float sq = mSim.config.squash, half = mSim.Half();
	MeshInstance die;
	die.mesh = mDieMesh;
	die.material = mDieMaterial;
	for (int i = 0; i < mSim.diceCount; ++i)
	{
		const uint8_t st = D.state[i];

		// Landing squash and bounce (a damped spring in world Y).
		float sy = 1, sxz = 1;
		const double lt = now - D.landT[i];
		if (lt < 0.5 && st != S_ROLLING) { const float s = static_cast<float>(sq * std::exp(-lt * 11) * std::cos(lt * 26)); sy = 1 - s; sxz = 1 + s * 0.5f; }
		const double ht = now - D.hitT[i];   // got hit but stayed put: a quick jolt
		if (ht >= 0 && ht < 0.4 && st != S_ROLLING) { const float s = static_cast<float>(mSim.config.hitWobble * std::exp(-ht * 12) * std::sin(ht * 38)); sy -= s; sxz += s * 0.5f; }

		DieMatrix(&D.q[i * 4], sxz, sy, D.px[i], D.py[i] + half * sy, D.pz[i], die.world);

		// Colour: dim while cooling down, a warm glow when ready again.
		float r = 1, g = 1, b = 1;
		mSim.DieColor(i, r, g, b);
		die.tint = { r, g, b, 1.0f };
		list.push_back(die);
	}
}

// The main camera's view x projection, built as the engine builds it (Engine::UpdateMainView).
bool DiceModule::ViewProjection(Engine& engine, XMMATRIX& viewProj) const
{
	Scene& scene = engine.GetScene();
	const Entity camera = engine.MainCamera();
	if (!scene.IsAlive(camera) || !scene.Has<Camera>(camera) || !scene.Has<Transform>(camera)) return false;
	const Camera& c = scene.Get<Camera>(camera);
	const Transform& t = scene.Get<Transform>(camera);
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&t.rotation)) * XMMatrixTranslation(t.position.x, t.position.y, t.position.z);
	Renderer& renderer = engine.GetRenderer();
	const float aspect = static_cast<float>(std::max(renderer.ViewWidth(), 1u)) / static_cast<float>(std::max(renderer.ViewHeight(), 1u));
	viewProj = XMMatrixInverse(nullptr, world) * XMMatrixOrthographicLH(2.0f * c.orthoHalfHeight * aspect, 2.0f * c.orthoHalfHeight, c.nearZ, c.farZ);
	return true;
}

// B7 step 1: one ray against the plane y = 0 (never against dice).
void DiceModule::PointerOnBoard(Engine& engine, bool& onBoard, double& boardX, double& boardZ)
{
	Input& input = engine.GetInput();
	bool inside = input.HasMouse();
	XMFLOAT2 pixel = input.MousePosition();
	if (mDevPointer)
	{
		mDevTime += engine.DeltaTime();
		const float f = 0.5f - 0.5f * std::cos(mDevTime / mDevSeconds * XM_PI);   // there and back
		pixel = { mDevFrom.x + (mDevTo.x - mDevFrom.x) * f, mDevFrom.y + (mDevTo.y - mDevFrom.y) * f };
		inside = true;
	}
	double hitX = 0, hitZ = 0;
	if (inside)
	{
		const Ray ray = engine.ScreenPointToRay(pixel);
		if (ray.direction.y < -1e-6f)
		{
			const float t = -ray.origin.y / ray.direction.y;
			// Back from the engine's mirrored space to the reference's (z -> -z).
			hitX = ray.origin.x + ray.direction.x * t;
			hitZ = -(ray.origin.z + ray.direction.z * t);
		}
		else
			inside = false;
	}
	onBoard = inside;
	boardX = hitX;
	boardZ = hitZ;
}

// B7 step 2: a ring of hoverRadius on the board (band 0.93..1, plus a disc at 7%), drawn over
// the board and dice: #cfe3ff in free play (#ffd866 during a session, grey while it finishes: M4).
void DiceModule::DrawHoverRing(Engine& engine)
{
	if (!mSim.hoverVisible) return;
	XMMATRIX viewProj;
	if (!ViewProjection(engine, viewProj)) return;
	const ViewRect view = engine.GameViewRect();
	const float R = std::max(0.05f, mSim.config.hoverRadius);
	constexpr int kSegments = 72;
	ImVec2 inner[kSegments], outer[kSegments];
	for (int k = 0; k < kSegments; ++k)
	{
		const double a = k * 2 * dice::kPi / kSegments;
		for (int ring = 0; ring < 2; ++ring)
		{
			const double r = ring == 0 ? 0.93 * R : R;
			const dice::Vec3 p = dice::ToEngine({ mSim.hoverX + std::cos(a) * r, 0.02, mSim.hoverZ + std::sin(a) * r });
			XMFLOAT3 ndc;
			XMStoreFloat3(&ndc, XMVector3TransformCoord(XMVectorSet(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1), viewProj));
			const ImVec2 screen(view.x + (ndc.x * 0.5f + 0.5f) * view.width, view.y + (0.5f - ndc.y * 0.5f) * view.height);
			(ring == 0 ? inner : outer)[k] = screen;
		}
	}
	ImDrawList* draw = SceneOverlay(engine);
	draw->PushClipRect(ImVec2(view.x, view.y), ImVec2(view.x + view.width, view.y + view.height), false);
	draw->AddConvexPolyFilled(inner, kSegments, IM_COL32(255, 255, 255, 18));   // 7%
	const ImU32 color = mSim.phase == dice::Phase::Finishing ? IM_COL32(0x77, 0x77, 0x77, 230)
		: mSim.phase == dice::Phase::Running ? IM_COL32(0xff, 0xd8, 0x66, 230) : IM_COL32(0xcf, 0xe3, 0xff, 230);   // opacity 0.9
	for (int k = 0; k < kSegments; ++k)
	{
		const int n = (k + 1) % kSegments;
		draw->AddQuadFilled(inner[k], outer[k], outer[n], inner[n], color);
	}
	draw->PopClipRect();
}

void DiceModule::OnImGui(Engine& engine)
{
	DrawHoverRing(engine);
	DrawPopups(engine);
	DrawHud(engine);
	// The reference hides the cursor over the board (the ring is the cursor).
	if (engine.GetInput().HasMouse()) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

	// The debug panel (B12), filled in milestone by milestone; M9 completes it.
	// The reference's panel: 300 px down the right-hand side, placed once per run (as soon as
	// the view has a size; it can be moved after that).
	if (!mPanelPlaced)
	{
		const ViewRect view = engine.GameViewRect();
		if (view.width >= 400 && view.height >= 300)
		{
			const float d = ImGui::GetStyle().FontScaleDpi > 0 ? ImGui::GetStyle().FontScaleDpi : 1.0f;
			ImGui::SetNextWindowPos(ImVec2(view.x + view.width - 312 * d, view.y + 52 * d), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(300 * d, std::max(200.0f, view.height - 64 * d)), ImGuiCond_Always);
			mPanelPlaced = true;
		}
	}
	DrawPanel(engine);
}

float DiceModule::PixelWorldSize(Engine& engine) const
{
	Scene& scene = engine.GetScene();
	const Entity camera = engine.MainCamera();
	if (!scene.IsAlive(camera) || !scene.Has<Camera>(camera)) return 0.01f;
	return 2.0f * scene.Get<Camera>(camera).orthoHalfHeight / static_cast<float>(std::max(engine.GetRenderer().ViewHeight(), 1u));
}

void DiceModule::SpawnSparks(const dice::SparkRequest& request)
{
	dice::Random& rand = mSparkRand;
	for (int n = 0; n < request.count; ++n)
	{
		const int p = mSparkNext;
		mSparkNext = (mSparkNext + 1) % kSparks;
		mSparkBase[p * 3] = request.r; mSparkBase[p * 3 + 1] = request.g; mSparkBase[p * 3 + 2] = request.b;
		const double a = rand() * dice::kPi * 2, e = rand() * 1.2 - 0.2, s = rand.Range(2.5, 6);
		mSparkPos[p * 3] = request.x; mSparkPos[p * 3 + 1] = request.y; mSparkPos[p * 3 + 2] = request.z;
		mSparkVel[p * 3] = static_cast<float>(std::cos(a) * std::cos(e) * s);
		mSparkVel[p * 3 + 1] = static_cast<float>(std::sin(e) * s + 2);
		mSparkVel[p * 3 + 2] = static_cast<float>(std::sin(a) * std::cos(e) * s);
		mSparkLife[p] = static_cast<float>(rand.Range(0.25, 0.45));
	}
}

// B15 sparks: gravity -18, colour fading over the last 0.25 s, 5 px points (not distance scaled).
void DiceModule::UpdateSparks(Engine& engine, double dt)
{
	std::vector<ParticleInstance>& particles = engine.GetRenderer().Particles();
	// Engine particles are lit like the scene at their position (colour x (light + emissive)):
	// a small colour with a big emissive makes that within ~4% of the colour itself, like the
	// reference's unlit points.
	constexpr float kGlow = 20.0f;
	const float size = 5.0f * ImGui::GetStyle().FontScaleDpi * PixelWorldSize(engine);
	for (int p = 0; p < kSparks; ++p)
	{
		if (mSparkLife[p] <= 0) continue;
		mSparkLife[p] -= static_cast<float>(dt);
		const int k = p * 3;
		if (mSparkLife[p] <= 0) continue;
		mSparkVel[k + 1] -= static_cast<float>(18 * dt);
		mSparkPos[k] += static_cast<float>(mSparkVel[k] * dt);
		mSparkPos[k + 1] += static_cast<float>(mSparkVel[k + 1] * dt);
		mSparkPos[k + 2] += static_cast<float>(mSparkVel[k + 2] * dt);
		const float f = std::min(1.0f, mSparkLife[p] / 0.25f);
		ParticleInstance spark;
		spark.position = { mSparkPos[k], mSparkPos[k + 1], -mSparkPos[k + 2] };   // mirrored (DiceVisuals.h)
		spark.size = size;
		// three.js treats vertex colours as linear, so these go straight to the HDR target.
		spark.color = { mSparkBase[k] * f / kGlow, mSparkBase[k + 1] * f / kGlow, mSparkBase[k + 2] * f / kGlow, 1.0f };
		spark.emissive = kGlow;
		spark.additive = true;
		spark.square = true;
		particles.push_back(spark);
	}
}

void DiceModule::ClearSparks()
{
	for (float& life : mSparkLife) life = 0;
}

// Hot reload while playing keeps every tunable (the board itself is dealt again).
std::string DiceModule::SaveState(Engine& /*engine*/)
{
	return dice::ConfigToJson(mSim.config);
}

void DiceModule::LoadState(Engine& /*engine*/, const std::string& /*state*/)
{
	// Applied in OnStartup (Engine::HotReloadState), before the board is built.
}

void DiceModule::OnShutdown(Engine& engine)
{
	Renderer& renderer = engine.GetRenderer();
	renderer.MeshInstances().clear();
	renderer.Settings().shadows.boundsExtents = { 0, 0, 0 };
	renderer.Settings().toneCurve = true;
	ReleaseFonts();
	mPopups.clear();
	ClearSparks();
	for (MeshHandle* mesh : { &mDieMesh, &mFeltMesh, &mBaseMesh, &mMarkerMesh })
		if (mesh->IsValid()) { renderer.DestroyMesh(*mesh); *mesh = {}; }
	for (MaterialHandle* material : { &mDieMaterial, &mFeltMaterial, &mPlainFeltMaterial, &mBaseMaterial, &mMarkerMaterial })
		if (material->IsValid()) { renderer.DestroyMaterial(*material); *material = {}; }
	for (TextureHandle* texture : { &mAtlas, &mGrid })
		if (texture->IsValid()) { renderer.DestroyTexture(*texture); *texture = {}; }
}
