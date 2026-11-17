#include "UndertimeModule.h"
#include "Meshes.h"
#include "ColorUtil.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>

using namespace DirectX;
using cfg::Block;

CHERRY_IMPLEMENT_GAME_MODULE(UndertimeModule)

namespace
{
	ImU32 ToImColor(uint32_t hex, int alpha = 255)
	{
		return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha);
	}
}

EngineConfig UndertimeModule::GetConfig() const
{
	EngineConfig config;
	config.windowTitle = L"UNDERTIME";
	config.windowWidth = 1600;
	config.windowHeight = 900;
	config.startInEditor = false;
	config.gameUiOverlayWindow = false;   // UNDERTIME draws its own HUD
	return config;
}

// =============================================================================
// Setup
// =============================================================================

void UndertimeModule::OnStartup(Engine& engine)
{
	world.Generate(cfg::kWorldSeed);

	MaterialDesc material;
	material.baseColor = { 1, 1, 1, 1 };     // colour comes from the vertices
	material.roughness = 0.85f;
	material.fresnelR0 = { 0.02f, 0.02f, 0.02f };
	vertexColorMaterial = engine.GetRenderer().CreateMaterial(material);

	CreateWorldMeshes(engine);
	CreateScenery(engine);
	CreateViewModels(engine);

	Camera& camera = engine.GetScene().Get<Camera>(engine.MainCamera());
	camera.nearZ = cfg::kNearPlane;
	camera.farZ = cfg::kFarPlane;

	UpdateCamera(engine);
	UpdateLighting(engine);
}

void UndertimeModule::CreateWorldMeshes(Engine& engine)
{
	Scene& scene = engine.GetScene();
	const int chunks = VoxelWorld::ChunkCount();
	chunkEntities.resize(chunks);
	chunkMeshes.resize(chunks);
	for (int i = 0; i < chunks; ++i)
	{
		chunkEntities[i] = scene.CreateEntity();
		scene.Add(chunkEntities[i], Transform{});
		scene.Add(chunkEntities[i], MeshRenderer{ {}, vertexColorMaterial });
		RebuildChunk(engine, i);
	}
}

void UndertimeModule::RebuildChunk(Engine& engine, int chunk)
{
	if (chunk < 0 || chunk >= static_cast<int>(chunkMeshes.size())) return;
	Renderer& renderer = engine.GetRenderer();
	renderer.DestroyMesh(chunkMeshes[chunk]);   // the GPU copy is released once in-flight frames are done

	const MeshData data = world.BuildChunkMesh(chunk);
	chunkMeshes[chunk] = data.indices.empty() ? MeshHandle{} : renderer.CreateMesh(data);

	MeshRenderer& meshRenderer = engine.GetScene().Get<MeshRenderer>(chunkEntities[chunk]);
	meshRenderer.mesh = chunkMeshes[chunk];
	meshRenderer.visible = chunkMeshes[chunk].IsValid();
}

void UndertimeModule::CreateScenery(Engine& engine)
{
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();
	auto spawn = [&](MeshHandle mesh, const XMFLOAT3& position, float yaw = 0.0f)
	{
		Entity entity = scene.CreateEntity();
		Transform transform;
		transform.position = position;
		transform.SetRotationEuler(0.0f, yaw, 0.0f);
		scene.Add(entity, transform);
		scene.Add(entity, MeshRenderer{ mesh, vertexColorMaterial });
		sceneryEntities.push_back(entity);
		return entity;
	};

	const MeshHandle ground = renderer.CreateMesh(meshes::Ground());
	const MeshHandle mountain = renderer.CreateMesh(meshes::Mountain());
	sceneryMeshes = { ground, mountain };
	spawn(ground, { 0, 0, 0 });
	mountainEntity = spawn(mountain, { 0, 0, 0 });

	// A few tree variants scattered in front of the mountain (seeded: same every run).
	MeshHandle trees[3];
	for (int i = 0; i < 3; ++i)
	{
		trees[i] = renderer.CreateMesh(meshes::Tree(4.5f + i * 1.3f, i + 1));
		sceneryMeshes.push_back(trees[i]);
	}
	std::mt19937 rng(cfg::kWorldSeed ^ 0x7EE5u);
	std::uniform_real_distribution<float> xDist(-45.0f, 66.0f), zDist(-55.0f, -4.0f), yawDist(0.0f, XM_2PI);
	int placed = 0;
	for (int attempt = 0; attempt < 200 && placed < 24; ++attempt)
	{
		const float x = xDist(rng), z = zDist(rng);
		const bool onPath = std::fabs(x - (cfg::kCenterX + 0.5f)) < 5.0f && z > -30.0f;
		const bool hutArea = x > 14.0f && x < 27.0f && z > -18.0f && z < -5.0f;   // reserved for the trader (milestone 2)
		const bool nearSpawn = std::fabs(x - cfg::kSpawnX) < 4.0f && std::fabs(z - cfg::kSpawnZ) < 4.0f;
		if (onPath || hutArea || nearSpawn) continue;
		spawn(trees[placed % 3], { x, cfg::kGroundY, z }, yawDist(rng));
		++placed;
	}
}

void UndertimeModule::CreateViewModels(Engine& engine)
{
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();
	pickMesh = renderer.CreateMesh(meshes::Pick());
	lanternMesh = renderer.CreateMesh(meshes::Lantern());

	auto spawn = [&](MeshHandle mesh)
	{
		Entity entity = scene.CreateEntity();
		scene.Add(entity, Transform{});
		MeshRenderer meshRenderer{ mesh, vertexColorMaterial };
		meshRenderer.layer = RenderLayer::ViewModel;   // drawn on top: never clips into walls
		scene.Add(entity, meshRenderer);
		return entity;
	};
	pickEntity = spawn(pickMesh);
	lanternEntity = spawn(lanternMesh);
}

void UndertimeModule::OnShutdown(Engine& engine)
{
	engine.GetInput().SetMouseCaptured(false);
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	for (Entity e : chunkEntities) scene.DestroyEntity(e);
	for (Entity e : sceneryEntities) scene.DestroyEntity(e);
	scene.DestroyEntity(pickEntity);
	scene.DestroyEntity(lanternEntity);
	for (MeshHandle m : chunkMeshes) renderer.DestroyMesh(m);
	for (MeshHandle m : sceneryMeshes) renderer.DestroyMesh(m);
	renderer.DestroyMesh(pickMesh);
	renderer.DestroyMesh(lanternMesh);
	renderer.DestroyMaterial(vertexColorMaterial);
}

// =============================================================================
// Frame
// =============================================================================

void UndertimeModule::OnUpdate(Engine& engine, float dt)
{
	time += dt;
	UpdateState(engine);

	Input& input = engine.GetInput();
	if (state == State::Playing)
	{
		player.Update(input, world, dt, 1.0f, input.IsMouseCaptured());
		UpdateMining(engine, dt);

		// Target outline (world-space lines, depth-tested so only the visible edges show).
		if (target.valid)
		{
			BoundingBox box({ target.x + 0.5f, target.y + 0.5f, target.z + 0.5f }, { 0.505f, 0.505f, 0.505f });
			const bool unbreakable = target.block == Block::Bedrock;
			engine.GetRenderer().Debug().Box(box, unbreakable ? DebugDraw::Color(0.9f, 0.3f, 0.25f) : DebugDraw::Color(1.0f, 0.95f, 0.8f));
		}
	}
	else
	{
		mineProgress = 0.0f;
	}

	messages.Update(dt);
	UpdateCamera(engine);
	UpdateViewModels(engine, dt);
	UpdateLighting(engine);
}

void UndertimeModule::UpdateState(Engine& engine)
{
	Input& input = engine.GetInput();
	switch (state)
	{
	case State::Title:
	case State::Paused:
		if (input.WasMousePressed(MouseButton::Left))
		{
			state = State::Playing;
			input.SetMouseCaptured(true);
			waitForRelease = true;
		}
		break;
	case State::Playing:
		if (input.WasKeyPressed(VK_ESCAPE))
		{
			state = State::Paused;
			input.SetMouseCaptured(false);
		}
		else if (!input.IsMouseCaptured() && input.WasMousePressed(MouseButton::Left))
		{
			// Capture was lost (alt-tab); a click takes it back without mining.
			input.SetMouseCaptured(true);
			waitForRelease = true;
		}
		break;
	}
}

void UndertimeModule::UpdateMining(Engine& engine, float dt)
{
	Input& input = engine.GetInput();
	if (waitForRelease && !input.IsMouseDown(MouseButton::Left))
	{
		waitForRelease = false;
	}

	// Find the targeted cell.
	const XMFLOAT3 eye = player.EyePosition();
	const XMFLOAT3 dir = player.LookDirection();
	const VoxelWorld::RayHit hit = world.Raycast(eye, dir, cfg::kReach);
	Target next;
	if (hit.hit && hit.inGrid)
	{
		next.valid = true;
		next.x = hit.x;
		next.y = hit.y;
		next.z = hit.z;
		next.block = world.Get(hit.x, hit.y, hit.z);
	}
	if (!next.valid || next.x != target.x || next.y != target.y || next.z != target.z)
	{
		mineProgress = 0.0f;
	}
	target = next;

	const bool mining = !waitForRelease && input.IsMouseDown(MouseButton::Left);
	swingAmount = std::clamp(swingAmount + (mining ? dt : -dt) * 6.0f, 0.0f, 1.0f);
	if (!mining || !target.valid)
	{
		mineProgress = 0.0f;
		return;
	}

	if (target.block == Block::Bedrock)
	{
		mineProgress = 0.0f;
		if (input.WasMousePressed(MouseButton::Left) || messages.Current().empty())
		{
			if (target.z == cfg::kGridZ - 1)
				messages.Show("The rock here is warm, and it hums.\nSomething is on the other side.", cfg::kMessageSeconds);
			else
				messages.Show("Too hard to break.", cfg::kMessageSeconds * 0.6f);
		}
		return;
	}

	const float hardness = world.EffectiveHardness(target.x, target.y, target.z);
	mineProgress += dt * cfg::kPickSpeed[pickLevel] / std::max(hardness, 0.01f);
	if (mineProgress >= 1.0f)
	{
		BreakCell(engine, target);
		mineProgress = 0.0f;
		target.valid = false;
	}
}

void UndertimeModule::BreakCell(Engine& engine, const Target& cell)
{
	world.Set(cell.x, cell.y, cell.z, Block::Air);

	// The broken cell and its six neighbours may change visibility; neighbours along Z
	// can live in the next/previous chunk.
	const int chunk = VoxelWorld::ChunkOf(cell.z);
	RebuildChunk(engine, chunk);
	if (cell.z > 0 && VoxelWorld::ChunkOf(cell.z - 1) != chunk) RebuildChunk(engine, chunk - 1);
	if (cell.z + 1 < cfg::kGridZ && VoxelWorld::ChunkOf(cell.z + 1) != chunk) RebuildChunk(engine, chunk + 1);

	// Ore collection, bag and selling arrive in milestone 2.
}

void UndertimeModule::UpdateCamera(Engine& engine)
{
	Scene& scene = engine.GetScene();
	const Entity cameraEntity = engine.MainCamera();
	Transform& transform = scene.Get<Transform>(cameraEntity);
	transform.position = player.EyePosition();
	XMStoreFloat4(&transform.rotation, player.Rotation());

	// Horizontal FOV from the config, converted to the vertical FOV the camera uses.
	const ViewRect view = engine.GameViewRect();
	const float aspect = view.height > 0.0f ? view.width / view.height : 16.0f / 9.0f;
	const float horizontal = XMConvertToRadians(cfg::kFovDegrees);
	scene.Get<Camera>(cameraEntity).fovY = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
}

void UndertimeModule::UpdateViewModels(Engine& engine, float dt)
{
	Scene& scene = engine.GetScene();
	const XMFLOAT3 eye = player.EyePosition();
	const XMVECTOR camRotation = player.Rotation();
	const XMMATRIX camWorld = XMMatrixRotationQuaternion(camRotation) * XMMatrixTranslation(eye.x, eye.y, eye.z);

	// Gentle walk bob.
	const float speed = std::sqrt(player.velocity.x * player.velocity.x + player.velocity.z * player.velocity.z);
	const float bob = (player.grounded ? std::min(speed / cfg::kWalkSpeed, 1.0f) : 0.0f) * 0.012f * std::sin(time * 9.0f);

	auto place = [&](Entity entity, const XMFLOAT3& offset, XMVECTOR localRotation, float scale)
	{
		Transform& t = scene.Get<Transform>(entity);
		XMStoreFloat3(&t.position, XMVector3TransformCoord(XMLoadFloat3(&offset), camWorld));
		XMStoreFloat4(&t.rotation, XMQuaternionMultiply(localRotation, camRotation));   // local, then camera
		t.scale = { scale, scale, scale };
	};

	// Pick (right hand): a chopping swing while mining.
	if (swingAmount > 0.0f) swingPhase += dt * cfg::kPickSwingRate;
	const float swing = swingAmount * (0.5f + 0.5f * std::sin(swingPhase)) * 0.9f;
	const XMVECTOR pickRotation = XMQuaternionRotationRollPitchYaw(0.55f + swing, 0.15f, -0.25f);
	const XMFLOAT3 pickOffset{ cfg::kPickOffset[0], cfg::kPickOffset[1] + bob, cfg::kPickOffset[2] };
	place(pickEntity, pickOffset, pickRotation, cfg::kPickScale);

	// Lantern (left hand), slight sway.
	const float sway = 0.03f * std::sin(time * 1.7f);
	const XMFLOAT3 lanternOffset{ cfg::kLanternOffset[0], cfg::kLanternOffset[1] - bob * 0.7f, cfg::kLanternOffset[2] };
	place(lanternEntity, lanternOffset, XMQuaternionRotationRollPitchYaw(0.0f, 0.0f, sway), cfg::kLanternScale);

	// The light sits a little in front of the lantern (towards the camera): there are no
	// emissive materials, so this is what makes the glass and brass you can see glow.
	const XMFLOAT3 lightOffset{ lanternOffset.x * 0.6f, lanternOffset.y + 0.05f, lanternOffset.z * 0.55f };
	XMStoreFloat3(&lanternWorld, XMVector3TransformCoord(XMLoadFloat3(&lightOffset), camWorld));
}

void UndertimeModule::UpdateLighting(Engine& engine)
{
	RenderSettings& settings = engine.GetRenderer().Settings();

	// 0 outside, 1 once you're kSunFadeDistance into the mountain.
	const float inside = std::clamp(player.EyePosition().z / cfg::kSunFadeDistance, 0.0f, 1.0f);
	const float outside = 1.0f - inside;
	const float lanternRange = cfg::kLanternRange[lanternLevel];

	// Daytime sky for now; the outside clock drives it in milestone 2.
	const XMFLOAT3 sky = color::Linear(cfg::kDaySky);
	settings.clearColor = { sky.x, sky.y, sky.z, 1.0f };
	settings.sunDirection = { -0.35f, -0.8f, 0.45f };
	settings.sunColor = color::Scale({ 1.0f, 0.96f, 0.88f }, outside);
	settings.fillLightScale = outside;
	const float ambient = cfg::kCaveAmbient + (cfg::kDayAmbient - cfg::kCaveAmbient) * outside;
	settings.ambient = { ambient, ambient * 1.02f, ambient * 1.08f };

	// Sky-coloured haze outside; black fog at lantern range in the dark.
	settings.fogEnabled = true;
	settings.fogColor = color::Scale(color::Linear(cfg::kDayFog), outside);
	settings.fogStart = cfg::kOutsideFogStart + (lanternRange * cfg::kCaveFogStartFraction - cfg::kOutsideFogStart) * inside;
	settings.fogEnd = cfg::kOutsideFogEnd + (lanternRange - cfg::kOutsideFogEnd) * inside;

	// Lantern with a slight flicker.
	const float flicker = 1.0f + cfg::kLanternFlicker * (0.6f * std::sin(time * 11.3f) + 0.4f * std::sin(time * 23.7f + 1.3f));
	PointLight lantern;
	lantern.position = lanternWorld;
	lantern.color = color::Linear(cfg::kLanternColor);
	lantern.intensity = cfg::kLanternIntensity * flicker;
	lantern.range = lanternRange;
	settings.pointLights.assign(1, lantern);

	// The mountain shell is scenery for the outside; hide it inside so it never blocks the view.
	engine.GetScene().Get<MeshRenderer>(mountainEntity).visible = !IsInside();
}

// =============================================================================
// HUD
// =============================================================================

void UndertimeModule::OnImGui(Engine& engine)
{
	const hud::Frame frame = hud::Begin(engine);

	if (state == State::Title)
	{
		hud::Overlay(frame, "UNDERTIME",
			{
				"A crack in the mountain above town. Inside, time runs slower.",
				"",
				"WASD  move        Space  jump        Mouse  look",
				"Hold left mouse  mine        Esc  pause",
			},
			"Click to begin");
		hud::End(frame);
		return;
	}

	const char* label = target.valid ? cfg::Info(target.block).name : "";
	const ImU32 labelColor = (target.valid && target.block != Block::Stone && target.block != Block::Bedrock)
		? ToImColor(cfg::Info(target.block).color) : IM_COL32(230, 222, 210, 255);
	hud::Crosshair(frame, mineProgress, label, labelColor);

	char depthLine[64];
	if (IsInside())
		std::snprintf(depthLine, sizeof(depthLine), "Depth  %.0f m", Depth() * 10.0f);
	else
		std::snprintf(depthLine, sizeof(depthLine), "Outside");
	hud::Panel(frame, 0, { "UNDERTIME", depthLine });

	messages.Draw(frame);

	if (state == State::Paused)
	{
		hud::Overlay(frame, "Paused", { "Esc released the mouse." }, "Click to continue");
	}
	else if (!engine.GetInput().IsMouseCaptured() && !engine.IsEditorMode())
	{
		hud::Message(frame, "Click to capture the mouse", 0.8f);
	}

	hud::End(frame);

	// Editor-only diagnostics in the Game panel.
	if (engine.IsEditorMode())
	{
		ImGui::Text("Player  %.2f, %.2f, %.2f", player.position.x, player.position.y, player.position.z);
		ImGui::Text("Grounded %s  yaw %.2f  pitch %.2f", player.grounded ? "yes" : "no", player.yaw, player.pitch);
		ImGui::Text("Target  %s (%d, %d, %d)", target.valid ? cfg::Info(target.block).name : "-", target.x, target.y, target.z);
		ImGui::SliderInt("Pick level", &pickLevel, 0, 3);
		ImGui::SliderInt("Lantern level", &lanternLevel, 0, 3);
	}
}
