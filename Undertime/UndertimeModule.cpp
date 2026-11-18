#include "UndertimeModule.h"
#include "Meshes.h"
#include "ColorUtil.h"
#include "Structure.h"
#include <cmath>
#include <cstdio>
#include <sstream>
#include <climits>
#include <random>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <Core/Log.h>

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

std::string UndertimeModule::BuildStamp()
{
#ifdef _DEBUG
	const char* config = "Debug";
#else
	const char* config = "Release";
#endif
	// When this module's file (the game DLL, or the exe in a monolithic build) was written.
	char built[48] = "unknown";
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA data;
	SYSTEMTIME utc, local;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&UndertimeModule::BuildStamp), &module)
		&& GetModuleFileNameW(module, path, MAX_PATH) > 0
		&& GetFileAttributesExW(path, GetFileExInfoStandard, &data)
		&& FileTimeToSystemTime(&data.ftLastWriteTime, &utc)
		&& SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
	{
		static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		std::snprintf(built, sizeof(built), "%d %s %02d:%02d", local.wDay, kMonths[(local.wMonth + 11) % 12], local.wHour, local.wMinute);
	}
	char text[128];
	std::snprintf(text, sizeof(text), "v%s  %s  built %s", cfg::kGameVersion, config, built);
	return text;
}

void UndertimeModule::RegisterTypes(GameTypes& types)
{
	types.Component<CaveWorld>("CaveWorld")
		.Field("seed", &CaveWorld::seed);
	types.Component<PlayerStart>("PlayerStart");
	types.System<CavePreviewSystem>();
}

std::string UndertimeModule::SaveState(Engine&)
{
	char text[96];
	std::snprintf(text, sizeof(text), "%u %.4f %.4f %.4f %.4f", seed, player.position.x, player.position.y, player.position.z, player.yaw);
	return text;
}

void UndertimeModule::LoadState(Engine& engine, const std::string& text)
{
	unsigned savedSeed = 0;
	DirectX::XMFLOAT3 feet{};
	float yaw = 0.0f;
	std::istringstream stream(text);
	if (!(stream >> savedSeed >> feet.x >> feet.y >> feet.z >> yaw)) return;
	if (savedSeed != seed) NewWorld(engine, savedSeed);
	player.Teleport(feet, yaw);
	UpdateCamera(engine);
	CHERRY_LOG("UNDERTIME: kept seed %u and the player's position across the reload", savedSeed);
}

void UndertimeModule::OnStartup(Engine& engine)
{
	// The module object lives across plays in the editor: start each one fresh.
	state = State::Title;

	// What the scene asks for.
	Scene& startScene = engine.GetScene();
	sceneSeed = 0;
	hasSceneSpawn = false;
	startScene.Each<CaveWorld>([&](Entity, CaveWorld& caveWorld) { sceneSeed = caveWorld.seed; });
	startScene.Each<PlayerStart>([&](Entity entity, PlayerStart&)
	{
		if (hasSceneSpawn) return;
		hasSceneSpawn = true;
		sceneSpawn = startScene.WorldPosition(entity);
		DirectX::XMVECTOR scale, rotation, translation;
		DirectX::XMMatrixDecompose(&scale, &rotation, &translation, startScene.WorldMatrix(entity));
		DirectX::XMFLOAT3 forward;
		DirectX::XMStoreFloat3(&forward, DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), rotation));
		sceneSpawnYaw = std::atan2(forward.x, forward.z);
	});

	buildStamp = BuildStamp();
	CHERRY_LOG("UNDERTIME %s", buildStamp.c_str());
	MaterialDesc material;
	material.baseColor = { 1, 1, 1, 1 };     // colour comes from the vertices
	material.roughness = 0.85f;
	material.fresnelR0 = { 0.02f, 0.02f, 0.02f };
	vertexColorMaterial = engine.GetRenderer().CreateMaterial(material);

	CreateWorldRendering(engine);
	CreateScenery(engine);
	CreateViewModels(engine);

	PhysicsWorld& physics = engine.GetPhysics();
	physics.SetGravity(cfg::kGravity);
	player.CreateBody(physics);

	// The scene's mountain, or a fresh one every game (after a hot reload: the same one
	// as before, so LoadState doesn't have to generate it a second time).
	uint32_t startSeed = sceneSeed != 0 ? sceneSeed : RandomSeed();
	unsigned reloadSeed = 0;
	if (std::istringstream(engine.HotReloadState()) >> reloadSeed && reloadSeed != 0) startSeed = reloadSeed;
	NewWorld(engine, startSeed);

	Camera& camera = engine.GetScene().Get<Camera>(engine.MainCamera());
	camera.nearZ = cfg::kNearPlane;
	camera.farZ = cfg::kFarPlane;

	UpdateCamera(engine);
	UpdateLighting(engine);
}

void UndertimeModule::CreateWorldRendering(Engine& engine)
{
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();

	// Ray-marched: the whole grid as one engine voxel volume (created by NewWorld).
	VoxelVolumeRenderer volume;
	volume.voxelSize = cfg::kVoxelSize;
	volume.colorJitter = cfg::kStoneJitter;
	volume.aoStrength = cfg::kVoxelAoStrength;
	voxelEntity = scene.CreateEntity();
	scene.Add(voxelEntity, Transform{});   // the volume's corner sits at the world origin
	scene.Add(voxelEntity, volume);

	// Meshes: one entity per chunk, built when that renderer is active.
	const int chunks = VoxelWorld::ChunkCount();
	chunkEntities.resize(chunks);
	chunkMeshes.resize(chunks);
	for (int i = 0; i < chunks; ++i)
	{
		chunkEntities[i] = scene.CreateEntity();
		scene.Add(chunkEntities[i], Transform{});
		MeshRenderer meshRenderer{ {}, vertexColorMaterial };
		meshRenderer.visible = false;
		scene.Add(chunkEntities[i], meshRenderer);
	}
	(void)renderer;
}

uint32_t UndertimeModule::RandomSeed()
{
	std::random_device device;
	return device() % 1000000000u;   // short enough to read out and type back in
}

void UndertimeModule::NewWorld(Engine& engine, uint32_t newSeed)
{
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	PhysicsWorld& physics = engine.GetPhysics();

	seed = newSeed;
	std::snprintf(seedInput, sizeof(seedInput), "%u", seed);
	const auto start = std::chrono::steady_clock::now();
	layout = GenerateCaves(world, seed);
	teleportZone = -1;
	map.Rebuild(world, layout);
	generationMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

	// Loose rock belongs to the old world.
	fragments.Clear(engine);
	world.ClearDamage();
	collected.fill(0);
	recentPickup.fill(0);
	pickupToast = 0.0f;

	// GPU volume.
	renderer.DestroyVoxelVolume(voxelVolume);
	VoxelVolumeDesc desc;
	desc.size = { static_cast<uint32_t>(cfg::kVoxX), static_cast<uint32_t>(cfg::kVoxY), static_cast<uint32_t>(cfg::kVoxZ) };
	desc.voxels = world.PaletteVoxels();
	desc.palette = VoxelWorld::BuildPalette();
	voxelVolume = renderer.CreateVoxelVolume(desc);
	scene.Get<VoxelVolumeRenderer>(voxelEntity).volume = voxelVolume;
	fragments.Initialize(voxelVolume);

	// Physics and meshes.
	collision.Build(physics, world);
	SetWorldRenderer(engine, worldRenderer);

	// Back outside, facing the mountain (or at the scene's PlayerStart).
	if (hasSceneSpawn) player.Teleport(sceneSpawn, sceneSpawnYaw);
	else player.Teleport({ cfg::kSpawnX, cfg::kGroundY, cfg::kSpawnZ }, 0.0f);
	target = {};
	swingTimer = 0.0f;
	CHERRY_LOG("UNDERTIME: seed %u: %d zones, %d pockets, %d floating voxels removed, generated in %.0f ms (collision %.0f ms)",
		seed, static_cast<int>(layout.zones.size()), static_cast<int>(layout.pockets.size()), layout.floatingVoxelsRemoved, generationMs, collision.BuildMs());
}

void UndertimeModule::SetWorldRenderer(Engine& engine, WorldRenderer mode)
{
	worldRenderer = mode;
	Scene& scene = engine.GetScene();
	scene.Get<VoxelVolumeRenderer>(voxelEntity).visible = mode == WorldRenderer::RayMarched;
	for (int i = 0; i < static_cast<int>(chunkEntities.size()); ++i)
	{
		if (mode == WorldRenderer::Meshes)
		{
			RebuildChunk(engine, i);   // meshes aren't kept up to date while hidden
		}
		else
		{
			engine.GetRenderer().DestroyMesh(chunkMeshes[i]);
			chunkMeshes[i] = {};
			scene.Get<MeshRenderer>(chunkEntities[i]).visible = false;
		}
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
	std::uniform_real_distribution<float> xDist(cfg::kCenterX - 55.0f, cfg::kCenterX + 55.0f), zDist(-55.0f, -4.0f), yawDist(0.0f, XM_2PI);
	int placed = 0;
	for (int attempt = 0; attempt < 200 && placed < 24; ++attempt)
	{
		const float x = xDist(rng), z = zDist(rng);
		const bool onPath = std::fabs(x - (cfg::kCenterX + 0.5f)) < 5.0f && z > -30.0f;
		const bool hutArea = x > cfg::kCenterX + 4.0f && x < cfg::kCenterX + 17.0f && z > -18.0f && z < -5.0f;   // reserved for the trader (milestone 2)
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
	fragments.Clear(engine);
	player.DestroyBody();
	collision.Destroy(engine.GetPhysics());
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	for (Entity e : chunkEntities) scene.DestroyEntity(e);
	scene.DestroyEntity(voxelEntity);
	renderer.DestroyVoxelVolume(voxelVolume);
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
		if (input.WasKeyPressed('V'))
		{
			SetWorldRenderer(engine, worldRenderer == WorldRenderer::RayMarched ? WorldRenderer::Meshes : WorldRenderer::RayMarched);
			messages.Show(worldRenderer == WorldRenderer::RayMarched ? "Renderer: ray-marched voxels" : "Renderer: voxel meshes", 1.5f);
		}
		if (input.WasKeyPressed(VK_F8)) NewWorld(engine, RandomSeed());
		if (input.WasKeyPressed(VK_F9)) DebugTeleportToNextZone(engine);
		if (input.WasKeyPressed('M')) mapOpen = !mapOpen;
		if (mapOpen)
		{
			// Mining changes the map; refresh it a couple of times a second while it's open.
			mapRefreshTimer -= dt;
			if (mapRefreshTimer <= 0.0f)
			{
				map.Rebuild(world, layout);
				mapRefreshTimer = 0.5f;
			}
		}
		if (input.WasKeyPressed(VK_F6)) DebugDropRock(engine);
		if (input.WasKeyPressed(VK_F7)) DebugCaveIn(engine);
		player.Update(input, dt, 1.0f, input.IsMouseCaptured());
		UpdateMining(engine, dt);

		// Target outline (world-space lines, depth-tested so only the visible edges show).
		if (target.valid && target.fragment == kInvalidBody)
		{
			const float s = cfg::kVoxelSize;
			BoundingBox box({ (target.x + 0.5f) * s, (target.y + 0.5f) * s, (target.z + 0.5f) * s }, { s * 0.52f, s * 0.52f, s * 0.52f });
			const bool unbreakable = target.block == Block::Bedrock;
			engine.GetRenderer().Debug().Box(box, unbreakable ? DebugDraw::Color(0.9f, 0.3f, 0.25f) : DebugDraw::Color(1.0f, 0.95f, 0.8f));
		}
	}
	else
	{
		swingTimer = 0.0f;
	}

	// Loose rock that has come to rest bakes back into the world (never into the player).
	const float r = cfg::kBodyWidth * 0.5f + 0.05f;
	const XMFLOAT3 keepMin{ player.position.x - r, player.position.y - 0.05f, player.position.z - r };
	const XMFLOAT3 keepMax{ player.position.x + r, player.position.y + cfg::kBodyHeight + 0.05f, player.position.z + r };
	changedScratch.clear();
	fragments.Update(engine, world, dt, keepMin, keepMax, changedScratch);
	UpdatePickups(engine, dt);
	for (const VoxelWorld::Box& box : changedScratch)
	{
		ApplyWorldChange(engine, box, true);   // rubble that landed unsupported falls again
	}
	// All of this frame's edits: rebuild each touched collision chunk once.
	collision.Flush(engine.GetPhysics(), world);

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
		if (regenerateRequested)
		{
			regenerateRequested = false;
			NewWorld(engine, pendingSeed);
		}
		if (input.WasKeyPressed('R'))
		{
			NewWorld(engine, RandomSeed());
		}
		[[fallthrough]];
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

	// Find what we're aiming at: a loose chunk if one is closer than the wall.
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
		next.point = hit.point;
		for (int a = 0; a < 3; ++a) next.normal[a] = hit.normal[a];
	}
	// Loose boulders. The player's own capsule is a physics body too (so falling rock hits
	// you), so the ray starts where it leaves the capsule, and skips past chips and nuggets.
	float along = 0.0f;
	{
		const float r = cfg::kBodyWidth * 0.5f + 0.03f;
		const float bottom = player.position.y + r, top = player.position.y + cfg::kBodyHeight - r;
		for (; along < cfg::kReach; along += 0.02f)
		{
			const float px = eye.x + dir.x * along - player.position.x, pz = eye.z + dir.z * along - player.position.z;
			const float py = eye.y + dir.y * along;
			const float dy = py < bottom ? bottom - py : (py > top ? py - top : 0.0f);
			if (px * px + pz * pz + dy * dy > r * r) break;
		}
	}
	for (int attempt = 0; attempt < 4 && along < cfg::kReach; ++attempt)
	{
		const XMFLOAT3 from{ eye.x + dir.x * along, eye.y + dir.y * along, eye.z + dir.z * along };
		const auto loose = engine.GetPhysics().Raycast(from, dir, cfg::kReach - along, true);
		if (!loose) break;
		const float distance = along + loose->distance;
		if (next.valid && distance >= hit.distance) break;
		if (fragments.Owns(loose->body))
		{
			next = Target{};
			next.valid = true;
			next.block = Block::Stone;
			next.point = loose->point;
			next.fragment = loose->body;
			break;
		}
		along = distance + 0.3f;   // a chip or nugget in the way: look past it
	}
	target = next;

	hitFeedback = std::max(hitFeedback - dt, 0.0f);
	const bool mining = !waitForRelease && input.IsMouseDown(MouseButton::Left);
	swingAmount = std::clamp(swingAmount + (mining ? dt : -dt) * 6.0f, 0.0f, 1.0f);
	if (!mining)
	{
		swingTimer = 0.0f;
		wasMining = false;
		return;
	}
	if (!wasMining)
	{
		// A fresh click skips most of the wind-up so the first hit lands quickly.
		swingTimer = std::max(swingTimer, cfg::kFirstSwingStart);
		wasMining = true;
	}

	// Holding the button keeps swinging; each time the pick lands, it bites into the rock
	// (and sticks in it for a moment).
	if (hitStop > 0.0f)
	{
		hitStop -= dt;
		return;
	}
	swingTimer += dt / cfg::kSwingPeriod;
	if (swingTimer >= 1.0f)
	{
		swingTimer -= 1.0f;
		Swing(engine);
	}
}

void UndertimeModule::Swing(Engine& engine)
{
	if (!target.valid) return;   // swung at air
	const XMFLOAT3 dir = player.LookDirection();

	// Hitting a loose boulder breaks a bite out of it and splits what's left.
	if (target.fragment != kInvalidBody)
	{
		const ChipResult chip = fragments.Chip(engine, target.fragment, target.point, cfg::kPickSpeed[pickLevel] / cfg::kPickSpeed[0], ++swingCount);
		if (!chip.hit) return;
		hitFeedback = cfg::kHitFeedbackTime;
		hitStrength = 1.0f;
		hitStop = cfg::kHitStop;
		voxelsMined += static_cast<int>(chip.voxels.size());
		SpawnChips(engine, chip.voxels, chip.worldOrigin, chip.position, chip.rotation, chip.velocity, { -dir.x, -dir.y, -dir.z }, target.point);
		return;
	}

	if (target.block == Block::Bedrock)
	{
		hitFeedback = cfg::kHitFeedbackTime;
		hitStrength = cfg::kHollowHitScale;
		const bool farWall = target.z >= (cfg::kGridZ - 1) * cfg::kVoxelsPerMeter;
		if (farWall)
			messages.Show("The rock here is warm, and it hums.\nSomething is on the other side.", cfg::kMessageSeconds);
		else
			messages.Show("Too hard to break.", cfg::kMessageSeconds * 0.6f);
		return;
	}

	// Bite centred just inside the surface we hit.
	const XMFLOAT3 center{ target.point.x + dir.x * 0.08f, target.point.y + dir.y * 0.08f, target.point.z + dir.z * 0.08f };
	const VoxelWorld::CarveResult result = world.Carve(center, cfg::kPickSpeed[pickLevel], target.x, target.y, target.z, ++swingCount);
	hitFeedback = cfg::kHitFeedbackTime;
	hitStrength = result.removed > 0 ? 1.0f : (result.cracked.Empty() ? cfg::kHollowHitScale : cfg::kCrackHitScale);
	if (!result.cracked.Empty())
	{
		// Cracked but still standing: just recolour.
		const std::vector<uint8_t> voxels = world.PaletteVoxels(result.cracked);
		const VoxelWorld::Box& box = result.cracked;
		engine.GetRenderer().UpdateVoxelVolume(voxelVolume,
			{ static_cast<uint32_t>(box.min[0]), static_cast<uint32_t>(box.min[1]), static_cast<uint32_t>(box.min[2]) },
			{ static_cast<uint32_t>(box.max[0] - box.min[0] + 1), static_cast<uint32_t>(box.max[1] - box.min[1] + 1), static_cast<uint32_t>(box.max[2] - box.min[2] + 1) },
			voxels.data());
	}
	if (result.removed == 0 && result.cracked.Empty()) return;
	hitStop = cfg::kHitStop;
	if (result.removed == 0) return;
	voxelsMined += result.removed;

	SpawnChips(engine, result.voxels, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0 },
		{ static_cast<float>(target.normal[0]), static_cast<float>(target.normal[1]), static_cast<float>(target.normal[2]) }, target.point);
	BreakOutVein(engine, result, center);
	ApplyWorldChange(engine, result.dirty, true);

	// Ore collection, bag and selling arrive in milestone 2.
}

void UndertimeModule::SpawnChips(Engine& engine, const std::vector<VoxelWorld::RemovedVoxel>& voxels, const XMINT3& gridOrigin,
	const XMFLOAT3& gridPosition, const XMFLOAT4& gridRotation, const XMFLOAT3& baseVelocity, const XMFLOAT3& out, const XMFLOAT3& facePoint)
{
	// Ore comes out as nuggets (every ore voxel); stone as a bigger chunk plus a scatter of
	// chips that roll down and crumble away (the rest of the stone is dust).
	std::mt19937 rng(swingCount * 7919u + 17u);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	const int count = static_cast<int>(voxels.size());
	std::vector<uint8_t> used(count, 0);
	const float s = cfg::kVoxelSize;
	const XMMATRIX grid = XMMatrixRotationQuaternion(XMLoadFloat4(&gridRotation)) * XMMatrixTranslation(gridPosition.x, gridPosition.y, gridPosition.z);
	auto isOre = [](Block b) { return b != Block::Stone && b != Block::Bedrock && b != Block::Air; };

	// The player's box: pieces that would start inside it are left as dust.
	const float pr = cfg::kBodyWidth * 0.5f + 0.05f;
	const XMFLOAT3 pMin{ player.position.x - pr, player.position.y, player.position.z - pr };
	const XMFLOAT3 pMax{ player.position.x + pr, player.position.y + cfg::kBodyHeight, player.position.z + pr };

	// Gathers the unused voxels (of the wanted sort) in a size^3 box at `seed` into one piece.
	auto gather = [&](int seed, int size, bool ore, FragmentVoxels& fragment)
	{
		const VoxelWorld::RemovedVoxel& origin = voxels[seed];
		fragment.size = { size, size, size };
		fragment.worldOrigin = { gridOrigin.x + origin.x, gridOrigin.y + origin.y, gridOrigin.z + origin.z };
		fragment.voxels.assign(static_cast<size_t>(size) * size * size, Block::Air);
		for (int i = 0; i < count; ++i)
		{
			const VoxelWorld::RemovedVoxel& v = voxels[i];
			const int lx = v.x - origin.x, ly = v.y - origin.y, lz = v.z - origin.z;
			if (used[i] || isOre(v.block) != ore || lx < 0 || ly < 0 || lz < 0 || lx >= size || ly >= size || lz >= size) continue;
			fragment.voxels[lx + size * (ly + size * lz)] = v.block;
			used[i] = 1;
		}
	};
	auto launch = [&](const VoxelWorld::RemovedVoxel& origin, FragmentVoxels&& fragment, FragmentKind kind, float speed)
	{
		// Where the piece was, then slid out along the face normal until it's in front of the
		// original surface: starting inside the bite it would just jam against the hole's sides.
		XMFLOAT3 position;
		XMStoreFloat3(&position, XMVector3TransformCoord(XMVectorSet(origin.x * s, origin.y * s, origin.z * s, 1.0f), grid));
		const float half = fragment.size.x * s * 0.5f;
		XMFLOAT3 center;
		XMStoreFloat3(&center, XMVector3TransformCoord(XMVectorSet(half, half, half, 1.0f), XMMatrixRotationQuaternion(XMLoadFloat4(&gridRotation))
			* XMMatrixTranslation(position.x, position.y, position.z)));
		const float depth = (center.x - facePoint.x) * out.x + (center.y - facePoint.y) * out.y + (center.z - facePoint.z) * out.z;
		const float need = half * 1.75f + cfg::kDebrisSpawnOffset;   // half the diagonal, so any rotation clears
		if (depth < need)
		{
			const float push = need - depth;
			position = { position.x + out.x * push, position.y + out.y * push, position.z + out.z * push };
			center = { center.x + out.x * push, center.y + out.y * push, center.z + out.z * push };
		}
		if (center.x + half > pMin.x && center.x - half < pMax.x && center.y + half > pMin.y && center.y - half < pMax.y
			&& center.z + half > pMin.z && center.z - half < pMax.z)
		{
			return false;   // would start inside the player
		}
		// Out of the face and a sideways scatter; gravity rolls them down the wall.
		XMFLOAT3 side{ unit(rng), unit(rng) * 0.3f, unit(rng) };
		const float along = side.x * out.x + side.y * out.y + side.z * out.z;
		side = { side.x - out.x * along, side.y - out.y * along, side.z - out.z * along };
		const XMFLOAT3 velocity{
			baseVelocity.x + out.x * speed + side.x * cfg::kDebrisScatter,
			baseVelocity.y + out.y * speed + side.y * cfg::kDebrisScatter,
			baseVelocity.z + out.z * speed + side.z * cfg::kDebrisScatter };
		const XMFLOAT3 spin{ unit(rng) * 6.0f, unit(rng) * 6.0f, unit(rng) * 6.0f };
		return fragments.Spawn(engine, std::move(fragment), position, velocity, spin, kind, gridRotation);
	};

	// Every ore voxel leaves as part of a nugget.
	for (int i = 0; i < count; ++i)
	{
		if (used[i] || !isOre(voxels[i].block)) continue;
		FragmentVoxels nugget;
		gather(i, cfg::kNuggetMaxSize, true, nugget);
		launch(voxels[i], std::move(nugget), FragmentKind::Nugget, cfg::kDebrisSpeed * 1.5f);
	}

	// A few stone pieces as rubble.
	for (int spawned = 0, attempt = 0; spawned < cfg::kDebrisPerSwing && attempt < cfg::kDebrisPerSwing * 4 && count > 0; ++attempt)
	{
		const int seed = static_cast<int>(rng() % static_cast<uint32_t>(count));
		if (used[seed] || isOre(voxels[seed].block)) continue;
		const int size = spawned == 0 ? cfg::kDebrisBigSize : 1 + static_cast<int>(rng() % static_cast<uint32_t>(cfg::kDebrisMaxSize));
		FragmentVoxels chip;
		gather(seed, size, false, chip);
		if (launch(voxels[seed], std::move(chip), FragmentKind::Rubble, cfg::kDebrisSpeed)) ++spawned;
	}
}

void UndertimeModule::BreakOutVein(Engine& engine, const VoxelWorld::CarveResult& carve, const XMFLOAT3& center)
{
	const float s = cfg::kVoxelSize;
	auto isOre = [](Block b) { return b != Block::Air && b != Block::Stone && b != Block::Bedrock; };

	// Seeds: standing ore next to the ore this bite removed.
	std::vector<XMINT3> stack;
	std::unordered_set<int> seen;
	for (const VoxelWorld::RemovedVoxel& v : carve.voxels)
	{
		if (!isOre(v.block)) continue;
		const int n[6][3] = { { v.x - 1, v.y, v.z }, { v.x + 1, v.y, v.z }, { v.x, v.y - 1, v.z }, { v.x, v.y + 1, v.z }, { v.x, v.y, v.z - 1 }, { v.x, v.y, v.z + 1 } };
		for (const auto& q : n)
		{
			if (!isOre(world.Get(q[0], q[1], q[2]))) continue;
			if (seen.insert(VoxelWorld::Index(q[0], q[1], q[2])).second) stack.push_back({ q[0], q[1], q[2] });
		}
	}
	if (stack.empty()) return;

	// All the rock (ore and stone) connected to it within the radius: a solid lump of wall
	// with the vein in it, which slides out of its socket instead of interlocking with it.
	std::vector<XMINT3> chunk;
	int ore = 0;
	const float r2 = cfg::kVeinChunkRadius * cfg::kVeinChunkRadius;
	while (!stack.empty() && static_cast<int>(chunk.size()) < cfg::kVeinChunkMaxVoxels)
	{
		const XMINT3 c = stack.back();
		stack.pop_back();
		chunk.push_back(c);
		if (isOre(world.Get(c.x, c.y, c.z))) ++ore;
		const int n[6][3] = { { c.x - 1, c.y, c.z }, { c.x + 1, c.y, c.z }, { c.x, c.y - 1, c.z }, { c.x, c.y + 1, c.z }, { c.x, c.y, c.z - 1 }, { c.x, c.y, c.z + 1 } };
		for (const auto& q : n)
		{
			if (!VoxelWorld::InGrid(q[0], q[1], q[2])) continue;
			const Block b = world.Get(q[0], q[1], q[2]);
			if (b == Block::Air || b == Block::Bedrock) continue;
			const float dx = (q[0] + 0.5f) * s - center.x, dy = (q[1] + 0.5f) * s - center.y, dz = (q[2] + 0.5f) * s - center.z;
			if (dx * dx + dy * dy + dz * dz > r2) continue;
			if (seen.insert(VoxelWorld::Index(q[0], q[1], q[2])).second) stack.push_back({ q[0], q[1], q[2] });
		}
	}
	if (ore < cfg::kVeinChunkMinOre) return;

	// Cut it out of the world.
	VoxelWorld::Box box;
	box.min[0] = box.min[1] = box.min[2] = INT_MAX;
	box.max[0] = box.max[1] = box.max[2] = INT_MIN;
	for (const XMINT3& c : chunk)
	{
		box.min[0] = std::min(box.min[0], c.x); box.max[0] = std::max(box.max[0], c.x);
		box.min[1] = std::min(box.min[1], c.y); box.max[1] = std::max(box.max[1], c.y);
		box.min[2] = std::min(box.min[2], c.z); box.max[2] = std::max(box.max[2], c.z);
	}
	FragmentVoxels fragment;
	fragment.size = { box.max[0] - box.min[0] + 1, box.max[1] - box.min[1] + 1, box.max[2] - box.min[2] + 1 };
	fragment.worldOrigin = { box.min[0], box.min[1], box.min[2] };
	fragment.voxels.assign(static_cast<size_t>(fragment.size.x) * fragment.size.y * fragment.size.z, Block::Air);
	std::vector<VoxelWorld::RemovedVoxel> removed;
	for (const XMINT3& c : chunk)
	{
		const Block b = world.Get(c.x, c.y, c.z);
		fragment.voxels[(c.x - box.min[0]) + fragment.size.x * ((c.y - box.min[1]) + fragment.size.y * (c.z - box.min[2]))] = b;
		removed.push_back({ c.x, c.y, c.z, b });
		world.Set(c.x, c.y, c.z, Block::Air);
	}
	ApplyWorldChange(engine, box, true);

	// Slide it out along the face normal until its collision boxes are clear of the wall and
	// the player; spawning it overlapping rock makes the physics fling it.
	const XMFLOAT3 out{ static_cast<float>(target.normal[0]), static_cast<float>(target.normal[1]), static_cast<float>(target.normal[2]) };
	const std::vector<PhysicsBox> boxes = DecomposeIntoBoxes(fragment, cfg::kMaxFragmentBoxes);
	const float pr = cfg::kBodyWidth * 0.5f + 0.05f;
	const XMFLOAT3 pMin{ player.position.x - pr, player.position.y - 0.05f, player.position.z - pr };
	const XMFLOAT3 pMax{ player.position.x + pr, player.position.y + cfg::kBodyHeight + 0.05f, player.position.z + pr };
	const float shrink = 0.02f;
	float slide = -1.0f;
	for (float d = 0.0f; d <= cfg::kVeinChunkMaxSlide + 1e-3f; d += 0.05f)
	{
		bool clear = true;
		for (const PhysicsBox& pb : boxes)
		{
			const XMFLOAT3 c{ box.min[0] * s + pb.center.x + out.x * d, box.min[1] * s + pb.center.y + out.y * d, box.min[2] * s + pb.center.z + out.z * d };
			const XMFLOAT3 mn{ c.x - pb.halfExtents.x + shrink, c.y - pb.halfExtents.y + shrink, c.z - pb.halfExtents.z + shrink };
			const XMFLOAT3 mx{ c.x + pb.halfExtents.x - shrink, c.y + pb.halfExtents.y - shrink, c.z + pb.halfExtents.z - shrink };
			const bool hitsPlayer = mx.x > pMin.x && mn.x < pMax.x && mx.y > pMin.y && mn.y < pMax.y && mx.z > pMin.z && mn.z < pMax.z;
			if (hitsPlayer || world.BoxOverlapsSolid(mn, mx)) { clear = false; break; }
		}
		if (clear) { slide = d; break; }
	}

	if (slide < 0.0f)
	{
		// No clean way out (a tight spot): it shatters into nuggets and rubble instead.
		SpawnChips(engine, removed, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0 }, out, target.point);
		messages.Show("The vein shatters!", 1.4f);
		CHERRY_LOG("Vein chunk shattered: %d voxels (%d ore), no room to fall out", static_cast<int>(chunk.size()), ore);
		return;
	}
	const XMFLOAT3 position{ box.min[0] * s + out.x * slide, box.min[1] * s + out.y * slide, box.min[2] * s + out.z * slide };
	const float speed = cfg::kVeinChunkSpeed;
	fragments.Spawn(engine, std::move(fragment), position, { out.x * speed, out.y * speed, out.z * speed }, { 0.3f, 0.0f, -0.2f });
	messages.Show("The vein breaks loose!", 1.4f);
	CHERRY_LOG("Vein chunk broke loose: %d voxels (%d ore), slid %.2f m", static_cast<int>(chunk.size()), ore, slide);
}

void UndertimeModule::UpdatePickups(Engine& engine, float dt)
{
	std::array<int, static_cast<int>(Block::Count)> got{};
	const XMFLOAT3 chest{ player.position.x, player.position.y + cfg::kBodyHeight * 0.6f, player.position.z };
	if (fragments.CollectNuggets(engine, chest, dt, got) > 0)
	{
		if (pickupToast <= 0.0f) recentPickup.fill(0);
		for (size_t b = 0; b < got.size(); ++b)
		{
			collected[b] += got[b];
			recentPickup[b] += got[b];
		}
		pickupToast = cfg::kPickupToastTime;
	}
	pickupToast = std::max(pickupToast - dt, 0.0f);
}

void UndertimeModule::ApplyWorldChange(Engine& engine, const VoxelWorld::Box& box, bool checkSupport)
{
	if (box.Empty()) return;

	// Ray-marched volume: upload just the changed box.
	const std::vector<uint8_t> voxels = world.PaletteVoxels(box);
	engine.GetRenderer().UpdateVoxelVolume(voxelVolume,
		{ static_cast<uint32_t>(box.min[0]), static_cast<uint32_t>(box.min[1]), static_cast<uint32_t>(box.min[2]) },
		{ static_cast<uint32_t>(box.max[0] - box.min[0] + 1), static_cast<uint32_t>(box.max[1] - box.min[1] + 1), static_cast<uint32_t>(box.max[2] - box.min[2] + 1) },
		voxels.data());

	// Static collision: rebuilt once per frame for every chunk touched (see OnUpdate).
	collision.MarkDirty(box);

	// Meshes: rebuild every chunk the change (and its neighbours' faces) touches.
	if (worldRenderer == WorldRenderer::Meshes)
	{
		const int first = VoxelWorld::ChunkOfVoxelZ(std::max(box.min[2] - 1, 0));
		const int last = VoxelWorld::ChunkOfVoxelZ(std::min(box.max[2] + 1, cfg::kVoxZ - 1));
		for (int chunk = first; chunk <= last; ++chunk) RebuildChunk(engine, chunk);
	}

	if (!checkSupport) return;

	// Anything left hanging free breaks off and falls.
	DetachResult detached = DetachUnsupported(world, box, cfg::kSupportSearchBudget);
	lastSupportVisited = detached.visited;
	for (const VoxelWorld::Box& removed : detached.removed)
	{
		ApplyWorldChange(engine, removed, false);
	}
	std::mt19937 rng(swingCount * 31u + 5u);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	for (FragmentVoxels& fragment : detached.fragments)
	{
		const float s = cfg::kVoxelSize;
		const XMFLOAT3 position{ fragment.worldOrigin.x * s, fragment.worldOrigin.y * s, fragment.worldOrigin.z * s };
		fragments.Spawn(engine, std::move(fragment), position, { 0, 0, 0 }, { unit(rng) * 0.3f, 0, unit(rng) * 0.3f });
		++detachedCount;
	}
}

void UndertimeModule::DebugTeleportToNextZone(Engine& engine)
{
	if (layout.zones.empty()) return;
	teleportZone = (teleportZone + 1) % static_cast<int>(layout.zones.size());
	const CaveZone& zone = layout.zones[teleportZone];
	const CaveRoom& entry = zone.rooms[0];
	// Face the exit cavern.
	const CaveRoom& exit = zone.rooms[1];
	const float yaw = std::atan2(exit.center.x - entry.center.x, exit.center.z - entry.center.z);
	player.Teleport(RoomStandPoint(entry), yaw);
	char text[96];
	std::snprintf(text, sizeof(text), "Debug: zone %d of %d (%.0f-%.0f m deep)", teleportZone + 1, static_cast<int>(layout.zones.size()), zone.z0 * 10.0f, zone.z1 * 10.0f);
	messages.Show(text, 2.0f);
	(void)engine;
}

void UndertimeModule::DebugDropRock(Engine& engine)
{
	// A 2 m boulder of stone with copper and silver flecks, dropped in front of the player.
	const int n = 8;
	FragmentVoxels fragment;
	fragment.size = { n, n, n };
	const XMFLOAT3 eye = player.EyePosition();
	const XMFLOAT3 dir = player.LookDirection();
	const float s = cfg::kVoxelSize;
	const XMFLOAT3 position{ eye.x + dir.x * 2.6f - n * s * 0.5f, eye.y + 0.8f, eye.z + dir.z * 2.6f - n * s * 0.5f };
	fragment.worldOrigin = { static_cast<int>(position.x / s), static_cast<int>(position.y / s), static_cast<int>(position.z / s) };
	fragment.voxels.assign(static_cast<size_t>(n) * n * n, Block::Stone);
	for (int i = 0; i < n * n * n; ++i)
	{
		const float h = color::Hash01(i, 7, 3, swingCount);
		if (h < 0.10f) fragment.voxels[i] = Block::Copper;
		else if (h < 0.14f) fragment.voxels[i] = Block::Silver;
	}
	fragments.Spawn(engine, std::move(fragment), position, { 0, 0, 0 }, { 0.8f, 0.0f, 0.5f });
	messages.Show("Debug: dropped a test rock", 1.5f);
}

void UndertimeModule::DebugCaveIn(Engine& engine)
{
	// Hollow out a room in front of the player, leaving a 1 m block hanging inside it with
	// nothing holding it up: the support check should drop it as one chunk.
	const int V = cfg::kVoxelsPerMeter;
	const XMFLOAT3 feet = player.position;
	const int cx = static_cast<int>(std::floor(feet.x * V));
	const int zFront = static_cast<int>(std::floor((feet.z + 0.35f) * V));
	const int floorY = static_cast<int>(std::floor((feet.y + 0.01f) * V));

	VoxelWorld::Box room;
	room.min[0] = cx - V;           room.max[0] = cx + V - 1;                // 2 m wide
	room.min[1] = floorY;           room.max[1] = floorY + 10 * V / 4 - 1;   // 2.5 m tall
	room.min[2] = zFront;           room.max[2] = zFront + 10 * V / 4 - 1;   // 2.5 m deep
	VoxelWorld::Box block;
	block.min[0] = cx - V / 2;      block.max[0] = cx + V / 2 - 1;
	block.max[1] = room.max[1] - 1; block.min[1] = block.max[1] - V + 1;
	block.min[2] = zFront + 3;      block.max[2] = block.min[2] + V - 1;

	for (int z = room.min[2]; z <= room.max[2]; ++z)
	for (int y = room.min[1]; y <= room.max[1]; ++y)
	for (int x = room.min[0]; x <= room.max[0]; ++x)
	{
		if (!VoxelWorld::InGrid(x, y, z) || world.Get(x, y, z) == Block::Bedrock) continue;
		const bool inBlock = x >= block.min[0] && x <= block.max[0] && y >= block.min[1] && y <= block.max[1] && z >= block.min[2] && z <= block.max[2];
		world.Set(x, y, z, inBlock ? Block::Stone : Block::Air);
	}
	for (int a = 0; a < 3; ++a) room.min[a] = std::max(room.min[a], 0);
	room.max[0] = std::min(room.max[0], cfg::kVoxX - 1);
	room.max[1] = std::min(room.max[1], cfg::kVoxY - 1);
	room.max[2] = std::min(room.max[2], cfg::kVoxZ - 1);
	ApplyWorldChange(engine, room, true);
	messages.Show("Debug: cave-in test", 1.5f);
}

void UndertimeModule::UpdateCamera(Engine& engine)
{
	Scene& scene = engine.GetScene();
	const Entity cameraEntity = engine.MainCamera();
	Transform& transform = scene.Get<Transform>(cameraEntity);
	transform.position = player.EyePosition();
	XMVECTOR rotation = player.Rotation();

	// Pick impact: a quick upward jolt and shake that settle over kHitFeedbackTime.
	if (hitFeedback > 0.0f)
	{
		const float k = hitFeedback / cfg::kHitFeedbackTime;
		const float amount = k * k * hitStrength;
		rotation = XMQuaternionMultiply(XMQuaternionRotationRollPitchYaw(-cfg::kHitKick * amount, 0.0f, 0.0f), rotation);
		const float shake = cfg::kHitShake * amount;
		transform.position.x += shake * std::sin(time * 91.0f);
		transform.position.y += shake * std::sin(time * 77.0f + 1.3f);
		transform.position.z += shake * std::sin(time * 83.0f + 2.1f);
	}
	XMStoreFloat4(&transform.rotation, rotation);

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

	// Pick (right hand): wind up, then strike so it lands when swingTimer wraps to 0
	// (the curve starts and ends at the same angle, so repeated swings loop smoothly).
	const float t = swingTimer;
	float swing;
	if (t < 0.6f)
	{
		const float u = t / 0.6f;
		swing = 0.6f + (-0.75f - 0.6f) * (u * u * (3.0f - 2.0f * u));   // ease back
	}
	else
	{
		const float u = (t - 0.6f) / 0.4f;
		swing = -0.75f + (0.6f + 0.75f) * (u * u);                       // accelerate into the rock
	}
	// Bounces back off the rock on impact.
	const float k = hitFeedback / cfg::kHitFeedbackTime;
	swing -= cfg::kPickRecoil * hitStrength * k * k;
	const XMVECTOR pickRotation = XMQuaternionRotationRollPitchYaw(0.55f + swingAmount * swing, 0.15f, -0.25f);
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
		char seedLine[64];
		std::snprintf(seedLine, sizeof(seedLine), "Mountain seed %u   (R for a new one)", seed);
		hud::Overlay(frame, "UNDERTIME",
			{
				"A crack in the mountain above town. Inside, time runs slower.",
				"",
				"WASD  move        Space  jump        Mouse  look",
				"Hold left mouse  mine        Esc  pause",
				"",
				seedLine,
				"",
				buildStamp,
			},
			"Click to begin");
		hud::End(frame);

		// Type a seed to replay a mountain.
		const ViewRect view = engine.GameViewRect();
		ImGui::SetNextWindowPos(ImVec2(view.x + view.width * 0.5f, view.y + view.height * 0.86f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		ImGui::Begin("##seed", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted("Seed");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
		const bool entered = ImGui::InputText("##seedvalue", seedInput, sizeof(seedInput), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Generate") || entered)
		{
			pendingSeed = static_cast<uint32_t>(std::strtoul(seedInput, nullptr, 10));
			regenerateRequested = true;
		}
		ImGui::End();
		return;
	}

	const char* label = !target.valid ? "" : (target.fragment != kInvalidBody ? "Loose rock" : cfg::Info(target.block).name);
	const ImU32 labelColor = (target.valid && target.block != Block::Stone && target.block != Block::Bedrock)
		? ToImColor(cfg::Info(target.block).color) : IM_COL32(230, 222, 210, 255);
	// The ring shows the swing: it fills as the pick winds up and lands when it closes.
	const bool swinging = swingAmount > 0.0f && target.valid && target.block != Block::Bedrock;
	hud::Crosshair(frame, swinging ? std::max(swingTimer, 0.001f) : 0.0f, label, labelColor);

	char depthLine[64];
	if (IsInside())
		std::snprintf(depthLine, sizeof(depthLine), "Depth  %.0f m", Depth() * 10.0f);
	else
		std::snprintf(depthLine, sizeof(depthLine), "Outside");
	char renderLine[64];
	std::snprintf(renderLine, sizeof(renderLine), "%s  (V)", worldRenderer == WorldRenderer::RayMarched ? "Ray-marched voxels" : "Voxel meshes");
	hud::Panel(frame, 0, { "UNDERTIME  " + buildStamp, depthLine, renderLine });

	// Collected ore, with what you just picked up on top.
	std::vector<std::string> bag;
	if (pickupToast > 0.0f)
	{
		std::string line;
		for (int b = 0; b < static_cast<int>(Block::Count); ++b)
		{
			if (recentPickup[b] == 0) continue;
			char part[48];
			std::snprintf(part, sizeof(part), "%s+%d %s", line.empty() ? "" : "   ", recentPickup[b], cfg::Info(static_cast<Block>(b)).name);
			line += part;
		}
		bag.push_back(line);
	}
	for (int b = 0; b < static_cast<int>(Block::Count); ++b)
	{
		if (collected[b] == 0) continue;
		char line[48];
		std::snprintf(line, sizeof(line), "%-14s %d", cfg::Info(static_cast<Block>(b)).name, collected[b]);
		bag.push_back(line);
	}
	if (!bag.empty()) hud::Panel(frame, 1, bag);

	messages.Draw(frame);
	if (mapOpen)
	{
		map.Draw(frame, layout, player.position, player.yaw);
	}

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
		ImGui::Text("Target  %s (voxel %d, %d, %d)", target.valid ? cfg::Info(target.block).name : "-", target.x, target.y, target.z);
		ImGui::Text("Voxels mined  %d", voxelsMined);
		ImGui::Separator();
		ImGui::Text("Loose rock    %d  (baked %d, dust %d, broke off %d)", fragments.Count(), fragments.Baked(), fragments.Vanished(), detachedCount);
		ImGui::Text("Collision     %.2f ms for %d chunk(s)   bake %.2f ms   support check %d voxels", collision.LastFlushMs(), collision.LastFlushChunks(), fragments.LastBakeMs(), lastSupportVisited);
		ImGui::Text("Seed %u   generated in %.0f ms   collision built in %.0f ms", seed, generationMs, collision.BuildMs());
		for (size_t i = 0; i < layout.zones.size(); ++i)
		{
			const CaveZone& zone = layout.zones[i];
			int pockets = 0;
			for (const CavePocket& p : layout.pockets) pockets += p.zone == static_cast<int>(i) ? 1 : 0;
			ImGui::Text("Zone %zu  %3.0f-%3.0f m  rooms %zu  tunnels %zu  pockets %d", i + 1, zone.z0, zone.z1, zone.rooms.size(), zone.tunnels.size(), pockets);
		}
		ImGui::TextDisabled("F6 drop a test rock   F7 cave-in test   F8 new world   F9 next zone   F4 physics bounds");
		ImGui::SliderInt("Pick level", &pickLevel, 0, 3);
		ImGui::SliderInt("Lantern level", &lanternLevel, 0, 3);
	}
}
