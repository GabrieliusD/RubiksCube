#include "UndertimeModule.h"
#include "Meshes.h"
#include "ColorUtil.h"
#include "Structure.h"
#include <cmath>
#include <cstdio>
#include <random>
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

void UndertimeModule::OnStartup(Engine& engine)
{
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

	// A fresh mountain every game.
	NewWorld(engine, RandomSeed());

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
	world.Generate(seed);
	generationMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

	// Loose rock belongs to the old world.
	fragments.Clear(engine);

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

	// Back outside, facing the mountain.
	player.Teleport({ cfg::kSpawnX, cfg::kGroundY, cfg::kSpawnZ }, 0.0f);
	target = {};
	swingTimer = 0.0f;
	CHERRY_LOG("UNDERTIME: generated seed %u in %.0f ms (collision %.0f ms)", seed, generationMs, collision.BuildMs());
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
	if (auto loose = engine.GetPhysics().Raycast(eye, dir, cfg::kReach, true))
	{
		if (fragments.Owns(loose->body) && (!next.valid || loose->distance < hit.distance))
		{
			next = Target{};
			next.valid = true;
			next.block = Block::Stone;
			next.point = loose->point;
			next.fragment = loose->body;
		}
	}
	target = next;

	const bool mining = !waitForRelease && input.IsMouseDown(MouseButton::Left);
	swingAmount = std::clamp(swingAmount + (mining ? dt : -dt) * 6.0f, 0.0f, 1.0f);
	if (!mining)
	{
		swingTimer = 0.0f;
		return;
	}

	// Holding the button keeps swinging; each time the pick lands, it bites into the rock.
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

	// Hitting a loose chunk knocks it away.
	if (target.fragment != kInvalidBody)
	{
		engine.GetPhysics().AddImpulse(target.fragment,
			{ dir.x * cfg::kPickImpulse, dir.y * cfg::kPickImpulse, dir.z * cfg::kPickImpulse }, target.point);
		return;
	}

	if (target.block == Block::Bedrock)
	{
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
	if (result.removed == 0) return;
	voxelsMined += result.removed;

	SpawnDebris(engine, result);
	ApplyWorldChange(engine, result.dirty, true);

	// Ore collection, bag and selling arrive in milestone 2.
}

void UndertimeModule::SpawnDebris(Engine& engine, const VoxelWorld::CarveResult& carve)
{
	// A few small clusters of the voxels the bite removed fly out as tumbling chunks;
	// the rest is dust.
	std::mt19937 rng(swingCount * 7919u + 17u);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	const int count = static_cast<int>(carve.voxels.size());
	std::vector<uint8_t> used(count, 0);

	const XMFLOAT3 out{ static_cast<float>(target.normal[0]), static_cast<float>(target.normal[1]), static_cast<float>(target.normal[2]) };
	for (int spawned = 0, attempt = 0; spawned < cfg::kDebrisPerSwing && attempt < cfg::kDebrisPerSwing * 4 && count > 0; ++attempt)
	{
		const int seed = static_cast<int>(rng() % static_cast<uint32_t>(count));
		if (used[seed]) continue;
		const int size = 1 + static_cast<int>(rng() % static_cast<uint32_t>(cfg::kDebrisMaxSize));
		const VoxelWorld::RemovedVoxel& origin = carve.voxels[seed];

		FragmentVoxels fragment;
		fragment.size = { size, size, size };
		fragment.worldOrigin = { origin.x, origin.y, origin.z };
		fragment.voxels.assign(static_cast<size_t>(size) * size * size, Block::Air);
		for (int i = 0; i < count; ++i)
		{
			const VoxelWorld::RemovedVoxel& v = carve.voxels[i];
			const int lx = v.x - origin.x, ly = v.y - origin.y, lz = v.z - origin.z;
			if (used[i] || lx < 0 || ly < 0 || lz < 0 || lx >= size || ly >= size || lz >= size) continue;
			fragment.voxels[lx + size * (ly + size * lz)] = v.block;
			used[i] = 1;
		}

		const float s = cfg::kVoxelSize;
		const XMFLOAT3 position{ origin.x * s, origin.y * s, origin.z * s };
		const XMFLOAT3 velocity{
			(out.x + 0.6f * unit(rng)) * cfg::kDebrisSpeed,
			(out.y + 0.6f * unit(rng)) * cfg::kDebrisSpeed + 1.0f,
			(out.z + 0.6f * unit(rng)) * cfg::kDebrisSpeed };
		const XMFLOAT3 spin{ unit(rng) * 6.0f, unit(rng) * 6.0f, unit(rng) * 6.0f };
		if (fragments.Spawn(engine, std::move(fragment), position, velocity, spin)) ++spawned;
	}
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

void UndertimeModule::DebugDropRock(Engine& engine)
{
	// A 1.5 m block of stone dropped two metres in front of the player.
	const int n = 6;
	FragmentVoxels fragment;
	fragment.size = { n, n, n };
	const XMFLOAT3 eye = player.EyePosition();
	const XMFLOAT3 dir = player.LookDirection();
	const float s = cfg::kVoxelSize;
	const XMFLOAT3 position{ eye.x + dir.x * 2.0f - n * s * 0.5f, eye.y + 0.8f, eye.z + dir.z * 2.0f - n * s * 0.5f };
	fragment.worldOrigin = { static_cast<int>(position.x / s), static_cast<int>(position.y / s), static_cast<int>(position.z / s) };
	fragment.voxels.assign(static_cast<size_t>(n) * n * n, Block::Stone);
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
	hud::Panel(frame, 0, { "UNDERTIME", depthLine, renderLine });

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
		ImGui::Text("Target  %s (voxel %d, %d, %d)", target.valid ? cfg::Info(target.block).name : "-", target.x, target.y, target.z);
		ImGui::Text("Voxels mined  %d", voxelsMined);
		ImGui::Separator();
		ImGui::Text("Loose rock    %d  (baked %d, dust %d, broke off %d)", fragments.Count(), fragments.Baked(), fragments.Vanished(), detachedCount);
		ImGui::Text("Collision     %.2f ms for %d chunk(s)   bake %.2f ms   support check %d voxels", collision.LastFlushMs(), collision.LastFlushChunks(), fragments.LastBakeMs(), lastSupportVisited);
		ImGui::Text("Seed %u   generated in %.0f ms   collision built in %.0f ms", seed, generationMs, collision.BuildMs());
		ImGui::TextDisabled("F6 drop a test rock   F7 cave-in test   F8 new world   F4 physics bounds");
		ImGui::SliderInt("Pick level", &pickLevel, 0, 3);
		ImGui::SliderInt("Lantern level", &lanternLevel, 0, 3);
	}
}
