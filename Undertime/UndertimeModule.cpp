#include "UndertimeModule.h"
#include "Meshes.h"
#include "ColorUtil.h"
#include "Structure.h"
#include "TerrainMesh.h"
#include "ValleyProps.h"
#include <Voxel/VoxelModel.h>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <climits>
#include <random>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
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
		.Field("seed", &CaveWorld::seed)
		.Asset("rockMaterial", &CaveWorld::rockMaterial, "material")
		.Field("showcase", &CaveWorld::showcase);
	types.Component<PlayerStart>("PlayerStart");
	types.System<CavePreviewSystem>();
	types.Component<ValleyWorld>("ValleyWorld")
		.Field("previewEra", &ValleyWorld::previewEra, 1.0f, 6.0f);
	types.Component<ValleyProp>("ValleyProp")
		.Field("eraFrom", &ValleyProp::eraFrom, 1.0f, 6.0f)
		.Field("eraTo", &ValleyProp::eraTo, 1.0f, 6.0f)
		.Field("snapToGround", &ValleyProp::snapToGround)
		.Field("groundOffset", &ValleyProp::groundOffset, -10.0f, 50.0f).Speed(0.05f)
		.Field("collide", &ValleyProp::collide)
		.Field("flattenGround", &ValleyProp::flattenGround);
	types.Component<ValleyPreview>("ValleyPreview", Component_NotAddable);
	types.System<ValleyPreviewSystem>();
}

std::string UndertimeModule::SaveState(Engine&)
{
	char text[200];
	const LooseBlocks::Options& o = blocks.GetOptions();
	std::snprintf(text, sizeof(text), "%u %.4f %.4f %.4f %.4f %.9f %.3f %d %d %d %d %d", seed, player.position.x, player.position.y, player.position.z, player.yaw,
		clock.Days(), clock.PersonalSeconds(), era, o.cap ? 1 : 0, o.piles ? 1 : 0, o.solidBlocks ? 1 : 0, o.solidPiles ? 1 : 0);
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
	double days = 0.0, personal = 0.0;
	int savedEra = 0;
	if (stream >> days >> personal >> savedEra)
	{
		clock.SetDays(days);
		SetEra(engine, savedEra);
		wasInside = IsInside();
	}
	int cap = 0, piles = 0, solidBlocks = 0, solidPiles = 0;
	if (stream >> cap >> piles >> solidBlocks >> solidPiles)
	{
		blocks.SetOptions(engine, LooseBlocks::Options{ cap != 0, piles != 0, solidBlocks != 0, solidPiles != 0 });
	}
	UpdateCamera(engine);
	CHERRY_LOG("UNDERTIME: kept seed %u and the player's position across the reload", savedSeed);
}

void UndertimeModule::OnStartup(Engine& engine)
{
	// The module object lives across plays in the editor: start each one fresh.
	state = State::Title;
	devHold = false;
	lanternMotes = INVALID_ENTITY;   // a fresh play world
	rainEmitter = INVALID_ENTITY;
	mineLamps.clear();
	debugTimeScale = 0;
	player.flying = false;

	// What the scene asks for.
	Scene& startScene = engine.GetScene();
	sceneSeed = 0;
	showcase = false;
	hasSceneSpawn = false;
	sceneRockMaterial.clear();
	startScene.Each<CaveWorld>([&](Entity, CaveWorld& caveWorld)
	{
		sceneSeed = caveWorld.seed;
		sceneRockMaterial = caveWorld.rockMaterial;
		showcase = caveWorld.showcase;
	});
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
	blocks.Initialize(vertexColorMaterial);

	clock.Reset();
	wasInside = false;
	grade = {};
	gradeTime = -1.0f;
	strobe = 0.0f;
	CreateWorldRendering(engine);
	CreateScenery(engine);   // the scene's preview era moves the clock to that era's start
	CreateViewModels(engine);

	collision.SetValleyOutside(true);
	PhysicsWorld& physics = engine.GetPhysics();
	physics.SetGravity(cfg::kGravity);
	player.CreateBody(physics);

	// The scene's mountain, or a fresh one every game (after a hot reload: the same one
	// as before, so LoadState doesn't have to generate it a second time).
	// Development: UNDERTIME_DEV_SHOWCASE=1 builds the showcase cave (as CaveWorld::showcase).
	char* devShowcase = nullptr;
	size_t devShowcaseLength = 0;
	if (_dupenv_s(&devShowcase, &devShowcaseLength, "UNDERTIME_DEV_SHOWCASE") == 0 && devShowcase)
	{
		showcase = showcase || std::string(devShowcase) == "1";
		free(devShowcase);
	}
	uint32_t startSeed = sceneSeed != 0 ? sceneSeed : (showcase ? cfg::kShowcaseSeed : RandomSeed());
	unsigned reloadSeed = 0;
	if (std::istringstream(engine.HotReloadState()) >> reloadSeed && reloadSeed != 0) startSeed = reloadSeed;
	// Development: UNDERTIME_DEV_SEED=N always builds that mountain (screenshots of the cave).
	char* devSeed = nullptr;
	size_t devSeedLength = 0;
	if (_dupenv_s(&devSeed, &devSeedLength, "UNDERTIME_DEV_SEED") == 0 && devSeed)
	{
		unsigned value = 0;
		if (std::istringstream(devSeed) >> value && value != 0) startSeed = value;
		free(devSeed);
	}
	NewWorld(engine, startSeed);

	Camera& camera = engine.GetScene().Get<Camera>(engine.MainCamera());
	camera.nearZ = cfg::kNearPlane;
	camera.farZ = cfg::kFarPlane;

	// Development: UNDERTIME_DEV_BLOCKS=N drops N loose blocks in front of the cave and logs
	// where frame time goes every two seconds (finding what slows down with many blocks).
	devPerf = false;
	char* devBlocks = nullptr;
	size_t devBlocksLength = 0;
	if (_dupenv_s(&devBlocks, &devBlocksLength, "UNDERTIME_DEV_BLOCKS") == 0 && devBlocks)
	{
		const int count = std::atoi(devBlocks);
		std::free(devBlocks);
		devPerf = true;
		DropTestBlocks(engine, count);
	}

	// Development: UNDERTIME_DEV_VIEW="x y z yaw pitch" (metres, degrees) skips the title and
	// starts there, for screenshots of a particular view.
	// UNDERTIME_DEV_SUN="x y z": the direction sunlight travels (checking shadows).
	char* sun = nullptr;
	size_t sunLength = 0;
	devSun = {};
	if (_dupenv_s(&sun, &sunLength, "UNDERTIME_DEV_SUN") == 0 && sun)
	{
		std::istringstream in(sun);
		std::free(sun);
		XMFLOAT3 d{};
		if (in >> d.x >> d.y >> d.z) devSun = d;
	}
	// UNDERTIME_DEV_TIME="hour [years]": start at that hour, optionally that many years after
	// arriving (and in that era), for checking the light and the town.
	char* devTime = nullptr;
	size_t devTimeLength = 0;
	if (_dupenv_s(&devTime, &devTimeLength, "UNDERTIME_DEV_TIME") == 0 && devTime)
	{
		std::istringstream in(devTime);
		std::free(devTime);
		double hour = 0.0, years = -1.0;
		if (in >> hour)
		{
			if (in >> years) clock.SetDays(std::floor(years * 365.0));
			clock.SetDays(std::floor(clock.Days()) + hour / 24.0);
			SetEra(engine, clock.Era());
		}
	}
	// UNDERTIME_DEV_WEATHER="overcast rain wet" (0..1 each): fixed weather (screenshots).
	char* devWeatherText = nullptr;
	size_t devWeatherLength = 0;
	if (_dupenv_s(&devWeatherText, &devWeatherLength, "UNDERTIME_DEV_WEATHER") == 0 && devWeatherText)
	{
		std::istringstream in(devWeatherText);
		std::free(devWeatherText);
		in >> devWeather[0] >> devWeather[1] >> devWeather[2];
	}
	char* view = nullptr;
	size_t viewLength = 0;
	if (_dupenv_s(&view, &viewLength, "UNDERTIME_DEV_VIEW") == 0 && view)
	{
		float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
		std::istringstream in(view);
		std::free(view);
		if (in >> x >> y >> z)
		{
			in >> yaw >> pitch;
			std::string hold;
			devHold = (in >> hold) && hold == "hold";
			player.Teleport({ x, y, z }, XMConvertToRadians(yaw));
			player.pitch = XMConvertToRadians(pitch);
			state = State::Playing;
		}
	}

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
	world.ClearDamage();   // (before the showcase cracks its blocks)
	layout = GenerateCaves(world, seed);
	if (showcase)
	{
		for (const ShowcaseView& view : BuildShowcase(world, layout))
			CHERRY_LOG("UNDERTIME showcase: %-8s UNDERTIME_DEV_VIEW=\"%.1f %.1f %.1f %.0f %.0f\"", view.name, view.x, view.y, view.z, view.yaw, view.pitch);
	}
	skylight.Build(world);
	CreateShaftDust(engine);
	CreateCaveLife(engine);
	teleportZone = -1;
	map.Rebuild(world, layout);
	generationMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

	// Loose rock belongs to the old world.
	fragments.Clear(engine);
	blocks.Clear(engine);
	backpack.fill(0);
	recentPickup.fill(0);
	pickupToast = 0.0f;

	// GPU volume.
	renderer.DestroyVoxelVolume(voxelVolume);
	renderer.DestroyLightVolume(skylightVolume);
	{
		const std::vector<uint8_t> levels = skylight.GpuLevels(Skylight::All());
		skylightVolume = renderer.CreateLightVolume({ Skylight::kX, Skylight::kY, Skylight::kZ }, levels.data());
	}
	VoxelVolumeDesc desc;
	desc.size = { static_cast<uint32_t>(cfg::kVoxX), static_cast<uint32_t>(cfg::kVoxY), static_cast<uint32_t>(cfg::kVoxZ) };
	desc.voxels = world.PaletteVoxels();
	desc.palette = VoxelWorld::BuildPalette();
	voxelVolume = renderer.CreateVoxelVolume(desc);
	const MaterialHandle rock = sceneRockMaterial.empty() ? MaterialHandle{} : engine.GetAssets().GetMaterial(sceneRockMaterial);
	scene.Get<VoxelVolumeRenderer>(voxelEntity).volume = voxelVolume;
	scene.Get<VoxelVolumeRenderer>(voxelEntity).material = rock;
	// The cave rock shader (Content/Shaders/CaveRock.hlsl) takes its colours from here: a
	// material without parameters of its own gets the rock and ore colours (sRGB) and amounts.
	if (const MaterialDesc* desc = renderer.GetMaterial(rock); desc && desc->params.empty())
	{
		MaterialDesc filled = *desc;
		auto add = [&](uint32_t hex) { const XMFLOAT3 c = color::Srgb(hex); filled.params.insert(filled.params.end(), { c.x, c.y, c.z }); };
		add(cfg::kStoneNear);
		add(cfg::kStoneDeep);
		add(cfg::kBlocks[static_cast<int>(Block::Copper)].color);
		add(cfg::kBlocks[static_cast<int>(Block::Silver)].color);
		add(cfg::kBlocks[static_cast<int>(Block::Gold)].color);
		add(cfg::kGoldQuartz);
		add(cfg::kCopperVerdigris);
		filled.params.insert(filled.params.end(), { cfg::kOreRichness, cfg::kGoldRichness, cfg::kSilverTarnish, static_cast<float>(cfg::kGridZ) });
		renderer.SetMaterial(rock, filled);
	}
	fragments.Initialize(voxelVolume, rock);

	// Physics and meshes.
	collision.Build(physics, world);
	SetWorldRenderer(engine, worldRenderer);

	// Back outside, facing the mountain (or at the scene's PlayerStart).
	if (hasSceneSpawn) player.Teleport(sceneSpawn, sceneSpawnYaw);
	else player.Teleport({ cfg::kSpawnX, cfg::kGroundY, cfg::kSpawnZ }, 0.0f);
	target = {};
	swingTimer = 0.0f;
	scoopPreview.clear();
	CHERRY_LOG("UNDERTIME: seed %u: %d zones, %d pockets, %d floating voxels removed, generated in %.0f ms (collision %.0f ms)",
		seed, static_cast<int>(layout.zones.size()), static_cast<int>(layout.pockets.size()), layout.floatingVoxelsRemoved, generationMs, collision.BuildMs());
	for (const CaveShaft& shaft : layout.shafts)   // for UNDERTIME_DEV_VIEW
		CHERRY_LOG("UNDERTIME: shaft into zone %d, foot at %.1f %.1f %.1f", shaft.zone + 1, shaft.bottom.x, shaft.bottom.y, shaft.bottom.z);
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
	// The valley (Valley.h, ValleyScenery.h): terrain, water and forest with collision, the
	// same every run; then the scene's ValleyProps (the town) for the current era.
	Scene& scene = engine.GetScene();
	PhysicsWorld& physics = engine.GetPhysics();
	if (!valley) valley = std::make_unique<Valley>();
	valley->SetPads(props.Pads(engine, scene, *valley));
	scenery.Build(engine, scene, *valley, &physics);
	era = era::kFirst;
	if (scene.IsRegistered<ValleyWorld>())
	{
		scene.Each<ValleyWorld>([&](Entity, ValleyWorld& world) { era = std::clamp(world.previewEra, era::kFirst, era::kLast); });
	}
	ValleyProps::SnapToGround(scene, *valley);
	props.Clear(&physics);
	props.ApplyEra(engine, scene, era, &physics);
	if (era > era::kFirst) clock.SetDays(OutsideClock::EraStartYears(era) * 365.0 + cfg::kArrivalHour / 24.0);
}

void UndertimeModule::CreateViewModels(Engine& engine)
{
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();
	pickMesh = renderer.CreateMesh(meshes::Pick());
	shovelMesh = renderer.CreateMesh(meshes::Shovel());
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
	shovelEntity = spawn(shovelMesh);
	scene.Get<MeshRenderer>(shovelEntity).visible = false;
	lanternEntity = spawn(lanternMesh);
}

void UndertimeModule::OnShutdown(Engine& engine)
{
	engine.GetInput().SetMouseCaptured(false);
	fragments.Clear(engine);
	blocks.Shutdown(engine);
	player.DestroyBody();
	collision.Destroy(engine.GetPhysics());
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	for (Entity e : chunkEntities) scene.DestroyEntity(e);
	scene.DestroyEntity(voxelEntity);
	renderer.DestroyVoxelVolume(voxelVolume);
	scene.DestroyEntity(pickEntity);
	scene.DestroyEntity(shovelEntity);
	scene.DestroyEntity(lanternEntity);
	for (Entity e : shaftDust) scene.DestroyEntity(e);
	shaftDust.clear();
	for (MeshHandle m : chunkMeshes) renderer.DestroyMesh(m);
	scenery.Destroy(engine, scene, &engine.GetPhysics());
	props.Clear(&engine.GetPhysics());
	renderer.DestroyMesh(pickMesh);
	renderer.DestroyMesh(shovelMesh);
	renderer.DestroyMesh(lanternMesh);
	renderer.DestroyMaterial(vertexColorMaterial);
	renderer.DestroyLightVolume(skylightVolume);
	skylightVolume = {};
	// The editor's view has no cave lighting, clouds, SSAO or light shafts.
	renderer.Settings().interior = {};
	renderer.Settings().clouds.enabled = false;
	renderer.Settings().ssao.enabled = false;
	renderer.Settings().lightShafts.enabled = false;
	renderer.Settings().colorGrade = {};
	renderer.Settings().heightFog.enabled = false;
	renderer.Settings().edgeHighlights.enabled = false;
	renderer.Settings().eyeAdaptation = {};
	renderer.Settings().rooms.clear();
}

// =============================================================================
// Frame
// =============================================================================

void UndertimeModule::OnUpdate(Engine& engine, float dt)
{
	const auto updateStart = std::chrono::steady_clock::now();
	time += dt;
	UpdateState(engine);
	struct PerfLog
	{
		UndertimeModule& m;
		Engine& engine;
		std::chrono::steady_clock::time_point start;
		float dt;
		~PerfLog() { m.LogPerf(engine, dt, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count()); }
	} perfLog{ *this, engine, updateStart, dt };

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
		if (input.WasKeyPressed('N'))
		{
			// Debug: jump the clock to the next era (after Gone, back to the day you arrived).
			const int next = era >= era::kLast ? era::kFirst : era + 1;
			if (next == era::kFirst) clock.Reset();
			else clock.SetDays(OutsideClock::EraStartYears(next) * 365.0 + std::fmod(clock.Days(), 1.0));
			SetEra(engine, next);
			daysAtEntry = clock.Days();
			messages.Show(std::string("Era: ") + era::Name(era), 1.5f);
		}
		if (input.WasKeyPressed('T'))
		{
			// Debug: skip ahead a couple of hours (checking the light through the day).
			clock.SkipHours(cfg::kSkipHours);
			messages.Show(clock.DateText(), 1.5f);
		}
		if (input.WasKeyPressed('J'))
		{
			// Debug: shadows fading between steps (kShadowFadeSeconds) or jumping.
			shadowFade = !shadowFade;
			messages.Show(shadowFade ? "Shadow steps: fade" : "Shadow steps: jump", 1.5f);
		}
		if (input.WasKeyPressed('G'))
		{
			// Debug: fly through everything (Space up, Ctrl down, Shift faster).
			player.flying = !player.flying;
			devHold = false;
			messages.Show(player.flying ? "Flying (G to land)" : "Walking", 1.5f);
		}
		if (input.WasKeyPressed(VK_OEM_6) || input.WasKeyPressed(VK_OEM_4))
		{
			// Debug: ] speeds outside time up ten times, [ slows it back down.
			debugTimeScale = std::clamp(debugTimeScale + (input.WasKeyPressed(VK_OEM_6) ? 1 : -1), 0, cfg::kDebugTimeScaleMax);
			const int factor = static_cast<int>(std::lround(std::pow(10.0, debugTimeScale)));
			messages.Show(factor == 1 ? std::string("Time: normal") : "Time: x" + std::to_string(factor), 1.5f);
		}
		if (input.WasKeyPressed(VK_F10)) player.pitch = player.pitch > 0.4f ? 0.0f : 0.85f;   // debug: look at your feet (scripts can't move the mouse)
		// Tools: 1 pick, 2 shovel, or the mouse wheel.
		const Tool before = tool;
		if (input.WasKeyPressed('1')) tool = Tool::Pick;
		if (input.WasKeyPressed('2')) tool = Tool::Shovel;
		if (input.WheelDelta() != 0.0f) tool = tool == Tool::Pick ? Tool::Shovel : Tool::Pick;
		if (tool != before)
		{
			swingTimer = shovelTimer = 0.0f;
			wasMining = wasShovelling = false;
			target = {};
			scoopPreview.clear();
		}
		if (!devHold) player.Update(input, dt, 1.0f, input.IsMouseCaptured());
		UpdateClock(engine, dt);
		if (tool == Tool::Pick) UpdateMining(engine, dt);
		else UpdateShovel(engine, dt);

		// Target outline (world-space lines, depth-tested so only the visible edges show).
		if (target.valid && target.fragment == kInvalidBody)
		{
			const float s = cfg::kVoxelSize;
			BoundingBox box({ (target.x + 0.5f) * s, (target.y + 0.5f) * s, (target.z + 0.5f) * s }, { s * 0.52f, s * 0.52f, s * 0.52f });
			const bool unbreakable = cfg::Unbreakable(target.block);
			engine.GetRenderer().Debug().Box(box, unbreakable ? DebugDraw::Color(0.9f, 0.3f, 0.25f) : DebugDraw::Color(1.0f, 0.95f, 0.8f));
		}
	}
	else
	{
		swingTimer = shovelTimer = 0.0f;
		scoopPreview.clear();
	}

	// Loose rock never goes back into the world. Past the boulder cap the oldest boulder
	// comes apart into blocks; scooped blocks fly into the backpack.
	fragments.Update(engine, dt);
	for (ChipResult over = fragments.ShatterOldestOverCap(engine); over.hit; over = fragments.ShatterOldestOverCap(engine))
	{
		SpawnBlocksFromBoulder(engine, over, { 0, 0, 0 });
	}
	const XMFLOAT3 chest{ player.position.x, player.position.y + cfg::kBodyHeight * 0.55f, player.position.z };
	blocks.Update(engine, dt, chest, player.position);
	pickupToast = std::max(pickupToast - dt, 0.0f);
	fullWarning = std::max(fullWarning - dt, 0.0f);
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
	// Loose boulders (loose blocks don't stop the pick: it reaches past them).
	float along = RayStartOutsidePlayer();
	for (int attempt = 0; attempt < 8 && along < cfg::kReach; ++attempt)
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
		along = distance + 0.36f;   // a loose block in the way: look past it (rays starting inside one hit it at once)
	}
	target = next;

	hitFeedback = std::max(hitFeedback - dt, 0.0f);
	rumble = std::max(rumble - dt / cfg::kRumbleSeconds, 0.0f);
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
		SpawnBlocksFromBoulder(engine, chip, { -dir.x, -dir.y, -dir.z });
		return;
	}

	if (cfg::Unbreakable(target.block))
	{
		hitFeedback = cfg::kHitFeedbackTime;
		hitStrength = cfg::kHollowHitScale;
		Sparks(engine, target, 3.0f);
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
	if (cfg::Info(target.block).hardness >= cfg::kSparkHardness) Sparks(engine, target, cfg::Info(target.block).hardness);
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

	SpawnBlocksFromFace(engine, result.voxels, target);
	ApplyWorldChange(engine, result.dirty, true);
}

void UndertimeModule::LogPerf(Engine& engine, float dt, float updateMs)
{
	if (!devPerf) return;
	perfFrames += 1;
	perfFrameMs += dt * 1000.0f;
	perfUpdateMs += updateMs;
	perfPhysicsMs += engine.GetPhysics().Stats().stepMs;
	perfTimer += dt;
	if (perfTimer < 2.0f) return;
	const RenderStats& r = engine.GetRenderer().Stats();
	CHERRY_LOG("dev perf: %d blocks (%d live, %d in %d piles, %d rescued) | frame %.2f ms | physics %.2f ms (%u bodies, %u active) | game update %.2f ms | GPU %.2f ms | %u draws | %d waiting, last pile rebuild %.2f ms (mesh %.2f, collision %.2f) for %d blocks",
		blocks.Count(), blocks.LiveCount(), blocks.SettledCount(), blocks.PileCount(), blocks.Rescued(), perfFrameMs / perfFrames, perfPhysicsMs / perfFrames, engine.GetPhysics().Stats().bodies, engine.GetPhysics().Stats().activeBodies,
		perfUpdateMs / perfFrames, r.gpuFrameMs, r.drawCalls, blocks.PendingCount(), blocks.LastPileBuildMs(), blocks.LastPileMeshMs(), blocks.LastPileCollisionMs(), blocks.LastPileBlocks());
	perfTimer = perfFrameMs = perfUpdateMs = perfPhysicsMs = 0.0f;
	perfFrames = 0;
}

float UndertimeModule::RayStartOutsidePlayer() const
{
	const XMFLOAT3 eye = player.EyePosition();
	const XMFLOAT3 dir = player.LookDirection();
	const float r = cfg::kBodyWidth * 0.5f + 0.03f;
	const float bottom = player.position.y + r, top = player.position.y + cfg::kBodyHeight - r;
	float along = 0.0f;
	for (; along < cfg::kReach; along += 0.02f)
	{
		const float px = eye.x + dir.x * along - player.position.x, pz = eye.z + dir.z * along - player.position.z;
		const float py = eye.y + dir.y * along;
		const float dy = py < bottom ? bottom - py : (py > top ? py - top : 0.0f);
		if (px * px + pz * pz + dy * dy > r * r) break;
	}
	return along;
}

void UndertimeModule::SpawnBlocksFromFace(Engine& engine, const std::vector<VoxelWorld::RemovedVoxel>& voxels, const Target& hit)
{
	if (voxels.empty()) return;
	int axis = 0;
	for (int a = 0; a < 3; ++a)
		if (hit.normal[a] != 0) axis = a;
	const int sign = hit.normal[axis] > 0 ? 1 : -1;
	const int faceVoxel = axis == 0 ? hit.x : (axis == 1 ? hit.y : hit.z);

	// Mirrored out of the hole into free air in front of the face (not into rock, and not
	// onto blocks already lying there).
	const auto occupied = blocks.OccupiedCells(engine);
	// Solid blocks also stay clear of the player (they'd be shoved out of the capsule).
	const bool avoidPlayer = blocks.GetOptions().solidBlocks;
	const float s0 = cfg::kVoxelSize;
	const float clear = cfg::kBodyWidth * 0.5f + s0 * 0.75f;
	const auto inPlayer = [&](int x, int y, int z)
	{
		const float dx = (x + 0.5f) * s0 - player.position.x, dz = (z + 0.5f) * s0 - player.position.z, cy = (y + 0.5f) * s0;
		return dx * dx + dz * dz < clear * clear && cy > player.position.y - s0 && cy < player.position.y + cfg::kBodyHeight + s0;
	};
	const auto isFree = [&](int x, int y, int z) { return !world.IsSolid(x, y, z) && !occupied(x, y, z) && !(avoidPlayer && inPlayer(x, y, z)); };
	const std::vector<XMINT3> cells = MirrorOutOfFace(voxels, axis, sign, faceVoxel, isFree);
	int inFront = 0;
	for (size_t i = 0; i < voxels.size(); ++i) inFront += (cells[i].x != voxels[i].x || cells[i].y != voxels[i].y || cells[i].z != voxels[i].z) ? 1 : 0;
	CHERRY_LOG("Bite: %d loose blocks (%d start in front of the face), %d lying around", static_cast<int>(voxels.size()), inFront, blocks.Count() + static_cast<int>(voxels.size()));

	std::mt19937 rng(swingCount * 7919u + 17u);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	const XMFLOAT3 out{ static_cast<float>(hit.normal[0]), static_cast<float>(hit.normal[1]), static_cast<float>(hit.normal[2]) };
	const float s = cfg::kVoxelSize;

	// A puff of rock dust out of the face, in the colour of what broke.
	{
		XMFLOAT3 middle{ 0, 0, 0 };
		for (const VoxelWorld::RemovedVoxel& v : voxels) { middle.x += v.x + 0.5f; middle.y += v.y + 0.5f; middle.z += v.z + 0.5f; }
		const float n = static_cast<float>(voxels.size());
		middle = { middle.x / n * s + out.x * s, middle.y / n * s + out.y * s, middle.z / n * s + out.z * s };
		const XMFLOAT4 colour = VoxelWorld::VoxelColor(voxels[0].block, voxels[0].x, voxels[0].y, voxels[0].z);
		RockDust(engine, middle, std::min(cfg::kDustPerBite + 2 * static_cast<int>(voxels.size()), cfg::kDustPerBiteMax), colour,
			{ out.x * cfg::kDustSpeed, out.y * cfg::kDustSpeed, out.z * cfg::kDustSpeed }, 0.07f);
	}
	for (size_t i = 0; i < voxels.size(); ++i)
	{
		const VoxelWorld::RemovedVoxel& v = voxels[i];
		const XMFLOAT3 center{ (cells[i].x + 0.5f) * s, (cells[i].y + 0.5f) * s, (cells[i].z + 0.5f) * s };
		// Out of the face, plus a sideways scatter; gravity tumbles them down the wall.
		XMFLOAT3 side{ unit(rng), unit(rng) * 0.3f, unit(rng) };
		const float along = side.x * out.x + side.y * out.y + side.z * out.z;
		side = { side.x - out.x * along, side.y - out.y * along, side.z - out.z * along };
		const XMFLOAT3 velocity{ out.x * cfg::kBlockSpeed + side.x * cfg::kBlockScatter,
			out.y * cfg::kBlockSpeed + side.y * cfg::kBlockScatter,
			out.z * cfg::kBlockSpeed + side.z * cfg::kBlockScatter };
		const XMFLOAT3 spin{ unit(rng) * cfg::kBlockSpin, unit(rng) * cfg::kBlockSpin, unit(rng) * cfg::kBlockSpin };
		blocks.Spawn(engine, v.block, v.x, v.y, v.z, center, { 0, 0, 0, 1 }, velocity, spin);
	}
}

void UndertimeModule::SpawnBlocksFromBoulder(Engine& engine, const ChipResult& chip, const XMFLOAT3& out)
{
	std::mt19937 rng(swingCount * 104729u + 3u);
	std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
	const float s = cfg::kVoxelSize;
	const XMMATRIX grid = XMMatrixRotationQuaternion(XMLoadFloat4(&chip.rotation)) * XMMatrixTranslation(chip.position.x, chip.position.y, chip.position.z);
	for (const VoxelWorld::RemovedVoxel& v : chip.voxels)
	{
		XMFLOAT3 center;
		XMStoreFloat3(&center, XMVector3TransformCoord(XMVectorSet((v.x + 0.5f) * s, (v.y + 0.5f) * s, (v.z + 0.5f) * s, 1.0f), grid));
		const XMFLOAT3 velocity{ chip.velocity.x + out.x * cfg::kBlockSpeed + unit(rng) * cfg::kBlockScatter,
			chip.velocity.y + out.y * cfg::kBlockSpeed + unit(rng) * cfg::kBlockScatter * 0.3f,
			chip.velocity.z + out.z * cfg::kBlockSpeed + unit(rng) * cfg::kBlockScatter };
		const XMFLOAT3 spin{ unit(rng) * cfg::kBlockSpin, unit(rng) * cfg::kBlockSpin, unit(rng) * cfg::kBlockSpin };
		blocks.Spawn(engine, v.block, chip.worldOrigin.x + v.x, chip.worldOrigin.y + v.y, chip.worldOrigin.z + v.z, center, chip.rotation, velocity, spin);
	}
}

void UndertimeModule::DropTestBlocks(Engine& engine, int count)
{
	// Fills free cells layer by layer in a square 2.5 m ahead of the player: each column
	// from its floor (a physics ray down from head height, so the terrain counts too) up to
	// the first rock, skipping blocks already there and the player. Each gets a random push
	// and spin, so the heap tumbles and has to come to rest like rubble from a cave-in.
	const float s = cfg::kVoxelSize;
	const XMFLOAT3 look = player.LookDirection();
	const float flat = std::max(std::sqrt(look.x * look.x + look.z * look.z), 1e-3f);
	const float ax = player.position.x + look.x / flat * 2.5f, az = player.position.z + look.z / flat * 2.5f;
	const int cx = static_cast<int>(std::floor(ax / s)), cz = static_cast<int>(std::floor(az / s));
	const float head = player.position.y + 1.6f;
	struct Column { int x, z, y; bool open; };
	std::vector<Column> columns;
	for (int dx = -10; dx <= 10; ++dx)
		for (int dz = -10; dz <= 10; ++dz)
		{
			const int x = cx + dx, z = cz + dz;
			const float px = (x + 0.5f) * s, pz = (z + 0.5f) * s;
			if (world.IsSolid(x, static_cast<int>(std::floor(head / s)), z)) continue;
			const float ox = px - player.position.x, oz = pz - player.position.z;
			if (ox * ox + oz * oz < 0.7f * 0.7f) continue;
			const auto floor = engine.GetPhysics().Raycast({ px, head, pz }, { 0, -1, 0 }, 8.0f);
			if (!floor) continue;
			columns.push_back({ x, z, static_cast<int>(std::floor((floor->point.y + 0.02f) / s)), true });
		}
	const auto occupied = blocks.OccupiedCells(engine);
	int placed = 0;
	for (int layer = 0; layer < 48 && placed < count; ++layer)
		for (Column& c : columns)
		{
			if (placed >= count) break;
			if (!c.open) continue;
			const int y = c.y + layer;
			if (world.IsSolid(c.x, y, c.z)) { c.open = false; continue; }   // reached the ceiling
			if (occupied(c.x, y, c.z)) continue;
			uint32_t h = static_cast<uint32_t>(placed) * 2654435761u + 0x9E3779B9u;
			auto random = [&h]() { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return (h & 0xFFFF) / 32767.5f - 1.0f; };   // -1..1
			const XMFLOAT3 push{ random() * 1.5f, random() * 1.0f + 0.5f, random() * 1.5f };
			const XMFLOAT3 spin{ random() * 6.0f, random() * 6.0f, random() * 6.0f };
			blocks.Spawn(engine, Block::Stone, std::clamp(c.x, 0, cfg::kVoxX - 1), std::clamp(y, 0, cfg::kVoxY - 1), std::clamp(c.z, 0, cfg::kVoxZ - 1),
				{ (c.x + 0.5f) * s, (y + 0.5f) * s, (c.z + 0.5f) * s }, { 0, 0, 0, 1 }, push, spin);
			++placed;
		}
	CHERRY_LOG("Dropped %d of %d test blocks around %.1f %.1f %.1f (%d lying around)", placed, count, ax, player.position.y, az, blocks.Count());
}

int UndertimeModule::BackpackCount() const
{
	int total = 0;
	for (int n : backpack) total += n;
	return total;
}

void UndertimeModule::UpdateShovel(Engine& engine, float dt)
{
	Input& input = engine.GetInput();
	if (waitForRelease && !input.IsMouseDown(MouseButton::Left)) waitForRelease = false;
	shovelDip = std::max(shovelDip - dt, 0.0f);

	// Aim: the first loose block or boulder along the view, or the rock surface (blocks lying
	// on the floor where you look).
	const XMFLOAT3 eye = player.EyePosition();
	const XMFLOAT3 dir = player.LookDirection();
	const float reach = cfg::kShovelReach;
	const VoxelWorld::RayHit hit = world.Raycast(eye, dir, reach);
	bool aimed = hit.hit;
	float aimDistance = hit.hit ? hit.distance : reach;
	float along = RayStartOutsidePlayer();
	for (int attempt = 0; attempt < 4 && along < aimDistance; ++attempt)
	{
		const XMFLOAT3 from{ eye.x + dir.x * along, eye.y + dir.y * along, eye.z + dir.z * along };
		const auto loose = engine.GetPhysics().Raycast(from, dir, aimDistance - along, true);
		if (!loose) break;
		if (blocks.Owns(loose->body) || fragments.Owns(loose->body))
		{
			aimed = true;
			aimDistance = along + loose->distance;
			break;
		}
		along += loose->distance + 0.05f;
	}
	// Settled piles, whichever physics layer they're on (solid piles are on the static one).
	if (const auto pile = blocks.RaycastPiles(eye, dir, aimDistance))
	{
		aimed = true;
		aimDistance = *pile;
	}
	const XMFLOAT3 point{ eye.x + dir.x * aimDistance, eye.y + dir.y * aimDistance, eye.z + dir.z * aimDistance };
	const int room = cfg::kBackpackCapacity - BackpackCount();
	scoopPreview = aimed ? blocks.InReach(engine, point, cfg::kShovelRadius, std::max(std::min(cfg::kShovelScoop, room), 1)) : std::vector<LooseBlocks::Ref>{};
	blocks.Outline(engine, scoopPreview, room > 0 ? DebugDraw::Color(1.0f, 0.95f, 0.8f) : DebugDraw::Color(0.9f, 0.3f, 0.25f));

	const bool digging = !waitForRelease && input.IsMouseDown(MouseButton::Left);
	if (!digging)
	{
		shovelTimer = 0.0f;
		wasShovelling = false;
		return;
	}
	if (!wasShovelling)
	{
		shovelTimer = std::max(shovelTimer, cfg::kFirstSwingStart);   // a fresh click scoops quickly
		wasShovelling = true;
	}
	shovelTimer += dt / cfg::kShovelPeriod;
	if (shovelTimer < 1.0f) return;
	shovelTimer -= 1.0f;
	if (scoopPreview.empty()) return;
	if (room <= 0)
	{
		if (fullWarning <= 0.0f) messages.Show("Backpack full.", 1.5f);
		fullWarning = 2.0f;
		return;
	}

	LooseBlocks::Counts got{};
	if (blocks.Collect(engine, scoopPreview, got) == 0) return;
	shovelDip = 0.2f;
	if (pickupToast <= 0.0f) recentPickup.fill(0);
	for (size_t b = 0; b < got.size(); ++b)
	{
		backpack[b] += got[b];
		recentPickup[b] += got[b];
	}
	pickupToast = cfg::kPickupToastTime;
	CHERRY_LOG("Scooped %d blocks: backpack %d / %d", static_cast<int>(scoopPreview.size()), BackpackCount(), cfg::kBackpackCapacity);
	scoopPreview.clear();
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

	// Daylight flows into whatever the change opened.
	const Skylight::CellBox lit = skylight.Update(world, box);
	if (!lit.Empty())
	{
		const std::vector<uint8_t> levels = skylight.GpuLevels(lit);
		engine.GetRenderer().UpdateLightVolume(skylightVolume,
			{ static_cast<uint32_t>(lit.min[0]), static_cast<uint32_t>(lit.min[1]), static_cast<uint32_t>(lit.min[2]) },
			{ static_cast<uint32_t>(lit.max[0] - lit.min[0] + 1), static_cast<uint32_t>(lit.max[1] - lit.min[1] + 1), static_cast<uint32_t>(lit.max[2] - lit.min[2] + 1) },
			levels.data());
	}

	// Static collision: rebuilt once per frame for every chunk touched (see OnUpdate).
	collision.MarkDirty(box);

	// Piles resting on or against the changed rock turn back into live blocks.
	blocks.Wake(engine, box);

	// Meshes: rebuild every chunk the change (and its neighbours' faces) touches.
	if (worldRenderer == WorldRenderer::Meshes)
	{
		const int first = VoxelWorld::ChunkOfVoxelZ(std::max(box.min[2] - 1, 0));
		const int last = VoxelWorld::ChunkOfVoxelZ(std::min(box.max[2] + 1, cfg::kVoxZ - 1));
		for (int chunk = first; chunk <= last; ++chunk) RebuildChunk(engine, chunk);
	}

	if (!checkSupport) return;

	CountCrystals(box);

	// Anything left hanging free breaks off and falls.
	DetachResult detached = DetachUnsupported(world, box, cfg::kSupportSearchBudget);
	lastSupportVisited = detached.visited;
	for (const VoxelWorld::Box& removed : detached.removed)
	{
		ApplyWorldChange(engine, removed, false);
		// A cloud of dust where the rock came away.
		const float s = cfg::kVoxelSize;
		const XMFLOAT3 middle{ (removed.min[0] + removed.max[0] + 1) * 0.5f * s, (removed.min[1] + removed.max[1] + 1) * 0.5f * s, (removed.min[2] + removed.max[2] + 1) * 0.5f * s };
		const float volume = (removed.max[0] - removed.min[0] + 1) * (removed.max[1] - removed.min[1] + 1) * (removed.max[2] - removed.min[2] + 1) * s * s * s;
		const int count = std::min(static_cast<int>(volume * cfg::kCaveInDustPerMetre) + 12, cfg::kCaveInDustMax);
		RockDust(engine, middle, count, VoxelWorld::VoxelColor(Block::Stone, removed.min[0], removed.min[1], removed.min[2]), { 0, -0.5f, 0 }, 0.16f);
		// The ground shakes: more for more rock, less further away.
		const XMFLOAT3 eye = player.EyePosition();
		const float away = std::sqrt((middle.x - eye.x) * (middle.x - eye.x) + (middle.y - eye.y) * (middle.y - eye.y) + (middle.z - eye.z) * (middle.z - eye.z));
		const float felt = std::clamp(1.0f - away / cfg::kRumbleReach, 0.0f, 1.0f);
		rumble = std::min(1.0f, rumble + volume * cfg::kRumblePerMetre * felt);
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
		if (!VoxelWorld::InGrid(x, y, z) || cfg::Unbreakable(world.Get(x, y, z))) continue;
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
	// Cave-in: a low, heavy shake, and a jolt that fades with it.
	if (rumble > 0.0f)
	{
		const float amount = rumble * rumble;
		const float shake = cfg::kRumbleShake * amount;
		transform.position.x += shake * (0.6f * std::sin(time * 23.0f) + 0.4f * std::sin(time * 41.0f + 0.7f));
		transform.position.y += shake * (0.6f * std::sin(time * 29.0f + 1.1f) + 0.4f * std::sin(time * 37.0f));
		const float roll = cfg::kRumbleKick * amount * std::sin(time * 17.0f + 0.4f);
		const float pitch = cfg::kRumbleKick * amount * 0.6f * std::sin(time * 13.0f + 2.0f);
		rotation = XMQuaternionMultiply(XMQuaternionRotationRollPitchYaw(pitch, 0.0f, roll), rotation);
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

	// Shovel: held low and forward; each scoop dips it down and back up.
	const float dip = std::sin(std::min(shovelDip / 0.2f, 1.0f) * XM_PI);
	const float windUp = wasShovelling ? 0.25f * shovelTimer * shovelTimer : 0.0f;
	const XMVECTOR shovelRotation = XMQuaternionRotationRollPitchYaw(1.05f + windUp - 0.45f * dip, 0.1f, -0.15f);
	const XMFLOAT3 shovelOffset{ cfg::kPickOffset[0], cfg::kPickOffset[1] - 0.02f + bob - 0.05f * dip, cfg::kPickOffset[2] + 0.04f * dip };
	place(shovelEntity, shovelOffset, shovelRotation, cfg::kPickScale);
	scene.Get<MeshRenderer>(pickEntity).visible = tool == Tool::Pick;
	scene.Get<MeshRenderer>(shovelEntity).visible = tool == Tool::Shovel;

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

	const float lanternRange = cfg::kLanternRange[lanternLevel];

	// Daytime sky for now; the outside clock drives it in milestone 2.
	settings.sky.enabled = true;
	settings.sky.gradientPower = cfg::kSkyGradientPower;
	settings.sky.sunSize = cfg::kSunDiskSize;
	settings.shadows.enabled = true;
	settings.shadows.nearDistance = cfg::kShadowNear;
	settings.shadows.farDistance = cfg::kShadowFar;
	settings.bloom.enabled = true;
	settings.bloom.intensity = cfg::kBloomIntensity;
	settings.bloom.threshold = cfg::kBloomThreshold;
	// The light of the outside hour; when days flash by (deep in the cave), their average
	// with a gentle pulse instead of a strobe.
	DayLight day = DayLightAt(clock.HourOfDay());
	if (strobe > 0.0f)
	{
		day = Blend(day, DayLightAverage(), strobe);
		const float pulse = 1.0f + cfg::kStrobePulse * strobe * std::sin(time * 2.0f * 3.14159265f * cfg::kStrobePulseHz);
		day.lightColor = color::Scale(day.lightColor, pulse);
		day.ambient *= pulse;
	}
	// The weather: overcast greys the sky and weakens the sun; racing days blur it to its average.
	Weather weather = WeatherAt(clock.Days());
	if (devWeather[0] >= 0.0f) weather = { devWeather[0], devWeather[1], devWeather[2] };
	if (strobe > 0.0f)
	{
		const Weather average = WeatherAverage();
		weather.overcast += (average.overcast - weather.overcast) * strobe;
		weather.rain *= 1.0f - strobe;
		weather.wet += (average.wet - weather.wet) * strobe;
	}
	{
		auto grey = [&](const XMFLOAT3& c)
		{
			const float luma = (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * 0.85f;
			return color::Lerp(c, { luma, luma, luma * 1.04f }, cfg::kOvercastGrey * weather.overcast);
		};
		day.zenith = grey(day.zenith);
		day.horizon = grey(day.horizon);
		day.lightColor = color::Scale(day.lightColor, 1.0f - (1.0f - cfg::kOvercastSun) * weather.overcast);
		day.diskBrightness *= 1.0f - weather.overcast;
		day.ambient *= 1.0f + 0.25f * weather.overcast;   // a grey sky lights evenly
	}
	settings.clearColor = { day.horizon.x, day.horizon.y, day.horizon.z, 1.0f };
	settings.sky.zenith = day.zenith;
	settings.sky.horizon = day.horizon;
	settings.sky.ground = day.ground;
	settings.sky.sunIntensity = day.diskBrightness;
	settings.sky.starIntensity = day.stars;
	settings.sunDirection = devSun.x != 0.0f || devSun.y != 0.0f || devSun.z != 0.0f ? devSun : day.lightDirection;
	{
		// The shadows follow the sun in small steps, not a little every frame: each move turns
		// the shadow maps' texel grid, which makes shadow edges shimmer while time passes. The
		// sun (and moon) themselves move smoothly.
		const XMFLOAT3 d = settings.sunDirection;
		const float step = XMConvertToRadians(cfg::kShadowStepDegrees);
		const float elevation = std::round(std::asin(std::clamp(d.y, -1.0f, 1.0f)) / step) * step;
		const float azimuth = std::round(std::atan2(d.x, d.z) / step) * step;
		settings.shadows.direction = { std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth) };
		settings.shadows.fadeSeconds = shadowFade ? cfg::kShadowFadeSeconds : 0.0f;
	}
	settings.sunColor = day.lightColor;
	settings.fillLightScale = day.fill;
	// Ambient: neutral by day, bluer at night.
	const XMFLOAT3 tint = color::Lerp({ 1.0f, 1.02f, 1.08f }, { 0.75f, 0.85f, 1.25f }, day.night);
	settings.ambient = color::Scale(tint, day.ambient);

	// Lamps and lit windows (Shaders/TownVoxel.hlsl reads the globals).
	settings.shaderGlobals[0] = { day.night, day.sunHeight, weather.wet, weather.rain };   // z, w: wet ground, rain (Shaders/Ground.hlsl)
	const XMFLOAT3 windowLight = color::Linear(cfg::kWindowLightColor);
	settings.shaderGlobals[1] = { windowLight.x, windowLight.y, windowLight.z, 0.0f };
	// Wind for the trees and grass (Shaders/Foliage.hlsl): the way the clouds drift, rising
	// and falling over a minute or so, with gusts rolling across.
	{
		const float length = std::max(std::hypot(cfg::kCloudWind[0], cfg::kCloudWind[1]), 1e-3f);
		const float swell = 0.6f * std::sin(time * 0.11f) + 0.4f * std::sin(time * 0.047f + 2.0f);
		const float strength = std::max(0.0f, cfg::kWindStrength * (1.0f + cfg::kWindVariation * swell) * (1.0f + cfg::kOvercastWind * weather.overcast));
		settings.shaderGlobals[2] = { cfg::kCloudWind[0] / length, cfg::kCloudWind[1] / length, strength, time * cfg::kWindGustSpeed };
	}
	settings.shaderGlobals[3] = VoxelWorld::PaletteLayout();   // the cave rock's palette (Shaders/CaveRock.hlsl)
	props.SetLightLevel(engine.GetScene(), cfg::kLampDayLevel + (1.0f - cfg::kLampDayLevel) * day.night);

	// Indoors: the rooms of the buildings that stand in this era (nearest first, up to the
	// renderer's limit).
	settings.rooms = props.Rooms();
	if (settings.rooms.size() > kMaxRooms)
	{
		const XMFLOAT3 eye = player.EyePosition();
		std::sort(settings.rooms.begin(), settings.rooms.end(), [&](const RoomSettings& a, const RoomSettings& b)
		{
			auto d2 = [&](const RoomSettings& r) { const float dx = r.center.x - eye.x, dz = r.center.z - eye.z; return dx * dx + dz * dz; };
			return d2(a) < d2(b);
		});
		settings.rooms.resize(kMaxRooms);
	}

	// Sky-coloured haze outside.
	settings.fogEnabled = true;
	settings.fogColor = day.horizon;
	settings.fogStart = cfg::kOutsideFogStart;
	settings.fogEnd = cfg::kOutsideFogEnd;
	// Valley haze: height fog, with a mist that lies low around dawn (and a little at dusk).
	{
		const float hour = static_cast<float>(clock.HourOfDay());
		auto bump = [](float x, float centre, float half) { const float t = std::clamp(1.0f - std::fabs(x - centre) / half, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
		const float mist = std::max(bump(hour, cfg::kMistDawnHour, cfg::kMistDawnHours), cfg::kMistDusk / cfg::kMistDawn * bump(hour, cfg::kMistDuskHour, cfg::kMistDawnHours)) * (1.0f - strobe);
		HeightFogSettings& haze = settings.heightFog;
		haze.enabled = true;
		haze.density = cfg::kHazeDensity * (1.0f + cfg::kMistDawn * mist) * (1.0f + (cfg::kOvercastHaze - 1.0f) * weather.overcast);
		haze.baseHeight = cfg::kGroundY;
		haze.falloff = cfg::kHazeFalloff + (cfg::kMistFalloff - cfg::kHazeFalloff) * mist;
		settings.fogColor = color::Lerp(day.horizon, day.zenith, cfg::kHazeBlue);
		haze.sunColor = { day.horizon.x + day.lightColor.x * cfg::kHazeSunWarmth, day.horizon.y + day.lightColor.y * cfg::kHazeSunWarmth, day.horizon.z + day.lightColor.z * cfg::kHazeSunWarmth };
		settings.fogStart = cfg::kHazeStart;
	}

	// The cave is the renderer's interior (per pixel, so the valley seen through the mouth
	// stays lit): the cave's own dim ambient, plus the daylight that reaches in from the mouth
	// and the shafts (Skylight), which lets the sun in near them too. No fog: beyond the
	// lantern it's simply dark, and the daylight at the mouth stays visible from deep in.
	InteriorSettings& interior = settings.interior;
	interior.enabled = true;
	interior.boxMin = { 0.0f, 0.0f, 0.0f };
	interior.boxMax = { static_cast<float>(cfg::kGridX), static_cast<float>(cfg::kGridY), static_cast<float>(cfg::kGridZ) };
	interior.fadeStart = cfg::kInteriorFadeStart;
	interior.fadeLength = cfg::kSunFadeDistance;
	interior.ambient = { cfg::kCaveAmbient, cfg::kCaveAmbient * 1.02f, cfg::kCaveAmbient * 1.08f };
	interior.fogStart = interior.fogEnd = 0.0f;
	interior.lightVolume = skylightVolume;
	interior.lightStrength = cfg::kSkylightStrength;
	interior.sunFromLight = cfg::kSkylightSunReach;

	settings.clouds.enabled = true;
	settings.clouds.coverage = cfg::kCloudCoverage + (cfg::kOvercastCoverage - cfg::kCloudCoverage) * weather.overcast;
	settings.clouds.scale = cfg::kCloudScale;
	settings.clouds.softness = cfg::kCloudSoftness;
	settings.clouds.darkness = cfg::kCloudDarkness;
	settings.clouds.height = cfg::kCloudHeight;
	settings.clouds.wind = { 0.0f, 0.0f };   // the game drifts them with outside time (UpdateClock)
	settings.clouds.drift = { static_cast<float>(cloudDrift[0]), static_cast<float>(cloudDrift[1]) };
	settings.clouds.blur = cloudBlur;      // too fast to see: an even veil
	settings.ssao.enabled = true;
	settings.ssao.radius = cfg::kSsaoRadius;
	settings.ssao.strength = cfg::kSsaoStrength;
	settings.ssao.bias = cfg::kSsaoBias;
	settings.edgeHighlights.enabled = true;
	settings.edgeHighlights.strength = cfg::kEdgeStrength;
	settings.edgeHighlights.distance = cfg::kEdgeDistance;
	settings.edgeHighlights.sharpness = cfg::kEdgeSharpness;
	LightShaftSettings& shafts = settings.lightShafts;
	shafts.enabled = true;
	shafts.density = cfg::kShaftDensity;
	shafts.interiorDensity = cfg::kShaftCaveDensity;
	shafts.anisotropy = cfg::kShaftAnisotropy;
	shafts.intensity = cfg::kShaftIntensity * (1.0f - strobe);   // no flickering beams while days race by
	shafts.maxDistance = cfg::kShaftDistance;
	shafts.steps = cfg::kShaftSteps;

	// The colour grade: the era's outside, the cave's inside, easing between them.
	{
		auto toSettings = [](const cfg::Grade& g)
		{
			ColorGradeSettings s;
			s.enabled = true;
			s.tint = { g.tint[0], g.tint[1], g.tint[2] };
			s.saturation = g.saturation;
			s.contrast = g.contrast;
			s.lift = { g.lift[0], g.lift[1], g.lift[2] };
			s.gain = { g.gain[0], g.gain[1], g.gain[2] };
			s.vignette = g.vignette;
			return s;
		};
		auto lerp3 = [](const XMFLOAT3& a, const XMFLOAT3& b, float t) { return XMFLOAT3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; };
		auto blend = [&](const ColorGradeSettings& a, const ColorGradeSettings& b, float t)
		{
			ColorGradeSettings s = a;
			s.tint = lerp3(a.tint, b.tint, t);
			s.saturation += (b.saturation - a.saturation) * t;
			s.contrast += (b.contrast - a.contrast) * t;
			s.lift = lerp3(a.lift, b.lift, t);
			s.gain = lerp3(a.gain, b.gain, t);
			s.vignette += (b.vignette - a.vignette) * t;
			return s;
		};
		const float depthIn = IsInside() ? std::clamp((player.position.z - cfg::kInteriorFadeStart) / cfg::kGradeCaveDepth, 0.0f, 1.0f) : 0.0f;
		const ColorGradeSettings target = blend(toSettings(cfg::kEraGrades[std::clamp(era, 1, 6) - 1]), toSettings(cfg::kCaveGrade), depthIn);
		const float dt = gradeTime < 0.0f ? 1e9f : std::max(time - gradeTime, 0.0f);
		grade = blend(grade.enabled ? grade : target, target, 1.0f - std::exp(-dt / cfg::kGradeSeconds));
		settings.colorGrade = grade;

		// Eye adaptation: the eyes open in the dark (less outside at night).
		EyeAdaptationSettings& eye = settings.eyeAdaptation;
		eye.enabled = true;
		eye.key = cfg::kEyeKey;
		eye.minExposure = 1.0f;
		const float nightOutside = day.night * (1.0f - depthIn);
		eye.maxExposure = cfg::kEyeMaxExposure + (cfg::kEyeMaxExposureNight - cfg::kEyeMaxExposure) * nightOutside;
		eye.speedBrighter = cfg::kEyeSpeedBrighter;
		eye.speedDarker = cfg::kEyeSpeedDarker;
		eye.reset = gradeTime < 0.0f;   // the first frame: no adapting from nothing
		gradeTime = time;
	}

	// Lantern with a slight flicker.
	const float flicker = 1.0f + cfg::kLanternFlicker * (0.6f * std::sin(time * 11.3f) + 0.4f * std::sin(time * 23.7f + 1.3f));
	PointLight lantern;
	lantern.position = lanternWorld;
	lantern.color = color::Linear(cfg::kLanternColor);
	lantern.intensity = cfg::kLanternIntensity * flicker;
	lantern.range = lanternRange;
	settings.pointLights.assign(1, lantern);
	// Lamps in the first zone, flickering; light around the Time crystal near you. The renderer
	// keeps the ones that matter most where you are.
	{
		const XMFLOAT3 lampColour = color::Linear(cfg::kMineLampColor);
		for (const MineLamp& lamp : mineLamps)
		{
			PointLight light;
			light.position = lamp.light;
			light.color = lampColour;
			light.intensity = cfg::kMineLampIntensity * (1.0f + cfg::kMineLampFlicker * (0.6f * std::sin(time * 9.7f + lamp.phase) + 0.4f * std::sin(time * 21.3f + lamp.phase * 2.0f)));
			light.range = cfg::kMineLampRange;
			settings.pointLights.push_back(light);
		}
		const XMFLOAT3 eye = player.EyePosition();
		const XMFLOAT3 crystalColour = color::Linear(cfg::kCrystalLightColor);
		for (const auto& [key, count] : crystalCells)
		{
			if (count < cfg::kCrystalClusterMin) continue;
			// A metre above the cell's middle: light from the floor's own height wouldn't reach it.
			const XMFLOAT3 at{ (static_cast<float>(key & 0x3FF) + 0.5f) * 2.0f, (static_cast<float>((key >> 10) & 0x3FF) + 0.5f) * 2.0f + 1.0f, (static_cast<float>(key >> 20) + 0.5f) * 2.0f };
			const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
			if (dx * dx + dy * dy + dz * dz > cfg::kCrystalLightReach * cfg::kCrystalLightReach) continue;
			PointLight light;
			light.position = at;
			light.color = crystalColour;
			light.intensity = std::min(count * cfg::kCrystalLightPerVoxel, cfg::kCrystalLightMax) * (0.9f + 0.1f * std::sin(time * 1.7f + at.x));
			light.range = cfg::kCrystalLightRange;
			settings.pointLights.push_back(light);
		}
	}
	// Rain around you while you're outside (the cave's roof keeps it off inside).
	{
		Scene& scene = engine.GetScene();
		if (!scene.IsAlive(rainEmitter))
		{
			rainEmitter = scene.CreateEntity();   // no EntityInfo: never saved
			scene.Add(rainEmitter, Transform{});
			ParticleEmitter drops;
			drops.maxParticles = cfg::kRainMax;
			drops.lifetime = 1.3f;
			drops.lifetimeJitter = 0.1f;
			drops.spawnBox = { 10.0f, 1.0f, 10.0f };
			drops.offset = { 0.0f, 9.0f, 0.0f };
			drops.velocity = { 0.0f, -cfg::kRainSpeed, 0.0f };
			drops.velocityJitter = { 0.3f, 1.0f, 0.3f };
			drops.sizeStart = drops.sizeEnd = 0.012f;   // streaks: thin, and long from their speed
			drops.sizeJitter = 0.3f;
			drops.stretch = 0.045f;
			drops.colorStart = { 0.8f, 0.85f, 0.92f, 0.45f };
			drops.colorEnd = { 0.8f, 0.85f, 0.92f, 0.3f };
			scene.Add(rainEmitter, drops);
		}
		const XMFLOAT3 eye = player.EyePosition();
		scene.Get<Transform>(rainEmitter).position = { eye.x + player.velocity.x * 0.6f, eye.y, eye.z + player.velocity.z * 0.6f };
		ParticleEmitter& drops = scene.Get<ParticleEmitter>(rainEmitter);
		const bool outside = !IsInside() || player.position.z < cfg::kInteriorFadeStart;
		drops.rate = static_cast<float>(cfg::kRainRate) * weather.rain;
		drops.enabled = outside && weather.rain > 0.02f;
	}

	// Dust motes drifting around you in the cave (they only show in the lantern's light).
	if (engine.GetScene().IsAlive(lanternMotes))
	{
		Scene& scene = engine.GetScene();
		scene.Get<Transform>(lanternMotes).position = player.EyePosition();
		scene.Get<ParticleEmitter>(lanternMotes).enabled = IsInside() && player.position.z > cfg::kInteriorFadeStart + 2.0f;
	}

	// Deep in the mountain the valley can't be seen: skip drawing it.
	scenery.SetVisible(engine.GetScene(), !(IsInside() && player.position.z > cfg::kHideValleyDepth));
}

// =============================================================================
// Dust
// =============================================================================

void UndertimeModule::RockDust(Engine& engine, const XMFLOAT3& at, int count, const XMFLOAT4& colour, const XMFLOAT3& out, float size)
{
	ParticleEmitter dust;
	dust.lifetime = 1.6f;
	dust.lifetimeJitter = 0.4f;
	dust.spawnBox = { size * 2.0f, size * 2.0f, size * 2.0f };
	dust.velocity = { 0, 0.15f, 0 };
	dust.velocityJitter = { 0.6f, 0.4f, 0.6f };
	dust.gravity = 1.2f;
	dust.drag = 2.5f;
	dust.turbulence = 0.6f;
	dust.sizeStart = size;
	dust.sizeEnd = size * 2.5f;
	dust.sizeJitter = 0.4f;
	// Paler than the rock: dust in the air.
	dust.colorStart = { colour.x * 1.3f + 0.05f, colour.y * 1.3f + 0.05f, colour.z * 1.3f + 0.05f, 0.75f };
	dust.colorEnd = { colour.x * 1.4f + 0.08f, colour.y * 1.4f + 0.08f, colour.z * 1.4f + 0.08f, 0.0f };
	dust.square = true;
	engine.GetParticles().Burst(dust, at, count, out);
}

void UndertimeModule::Sparks(Engine& engine, const Target& at, float hardness)
{
	// Hot sparks off the pick where it struck, bouncing out of the face and falling.
	ParticleEmitter sparks;
	sparks.lifetime = 0.45f;
	sparks.lifetimeJitter = 0.5f;
	sparks.spawnBox = { 0.04f, 0.04f, 0.04f };
	sparks.velocity = { 0, 0.8f, 0 };
	sparks.velocityJitter = { 2.2f, 1.6f, 2.2f };
	sparks.gravity = 9.0f;
	sparks.drag = 1.5f;
	sparks.sizeStart = 0.035f;
	sparks.sizeEnd = 0.01f;
	sparks.sizeJitter = 0.4f;
	sparks.colorStart = { 1.0f, 0.75f, 0.35f, 1.0f };
	sparks.colorEnd = { 1.0f, 0.35f, 0.08f, 0.0f };
	sparks.emissive = 8.0f;
	sparks.additive = true;
	sparks.square = true;
	sparks.stretch = 0.025f;   // short bright streaks
	const float out = 2.5f;
	const XMFLOAT3 point{ at.point.x + at.normal[0] * 0.03f, at.point.y + at.normal[1] * 0.03f, at.point.z + at.normal[2] * 0.03f };
	const int count = std::min(static_cast<int>(cfg::kSparksPerStrike * hardness / cfg::kSparkHardness), cfg::kSparksMax);
	engine.GetParticles().Burst(sparks, point, count, { at.normal[0] * out, at.normal[1] * out, at.normal[2] * out });
}

void UndertimeModule::CreateCaveLife(Engine& engine)
{
	Scene& scene = engine.GetScene();
	for (const MineLamp& lamp : mineLamps) if (scene.IsAlive(lamp.entity)) scene.DestroyEntity(lamp.entity);
	mineLamps.clear();

	// A lamp hanging from the ceiling of each cavern of the first zone (miners were here).
	const MeshHandle lampMesh = engine.GetAssets().GetMesh("Models/Placeholders/MineLamp.vox");
	if (!layout.zones.empty() && lampMesh.IsValid())
	{
		uint32_t salt = 1;
		for (const CaveRoom& room : layout.zones[0].rooms)
		{
			const XMFLOAT3 from{ room.center.x, room.center.y + 0.6f, room.center.z };
			const VoxelWorld::RayHit up = world.Raycast(from, { 0, 1, 0 }, 12.0f);
			if (!up.hit || up.distance + 0.6f < cfg::kMineLampMinHeadroom) continue;
			const float ceiling = up.point.y;
			const Entity entity = scene.CreateEntity();   // no EntityInfo: never saved
			Transform transform;
			transform.position = { room.center.x, ceiling - 1.5f, room.center.z };   // the model is 1.5 m tall, chain at the top
			scene.Add(entity, transform);
			scene.Add(entity, MeshRenderer{ lampMesh, engine.GetAssets().GetMaterial("engine/VoxelModel") });
			mineLamps.push_back({ entity, { room.center.x, ceiling - 1.1f, room.center.z }, static_cast<float>(salt++) * 2.3f });
		}
	}

	// Time crystal, counted per 2 m cell (lights around the clusters).
	crystalCells.clear();
	VoxelWorld::Box all;
	all.min[0] = all.min[1] = all.min[2] = 0;
	all.max[0] = cfg::kVoxX - 1;
	all.max[1] = cfg::kVoxY - 1;
	all.max[2] = cfg::kVoxZ - 1;
	CountCrystals(all);
	int clusters = 0;
	for (const auto& [key, count] : crystalCells) clusters += count >= cfg::kCrystalClusterMin ? 1 : 0;
	CHERRY_LOG("UNDERTIME: %d mine lamps%s, %d Time crystal clusters lit", static_cast<int>(mineLamps.size()),
		mineLamps.empty() ? "" : (" (first at " + std::to_string(static_cast<int>(mineLamps[0].light.x)) + " " + std::to_string(static_cast<int>(mineLamps[0].light.y)) + " " + std::to_string(static_cast<int>(mineLamps[0].light.z)) + ")").c_str(), clusters);

	// Motes drifting around the player in the cave, lit by the lantern.
	if (!scene.IsAlive(lanternMotes))
	{
		lanternMotes = scene.CreateEntity();
		scene.Add(lanternMotes, Transform{});
		ParticleEmitter motes;
		motes.rate = static_cast<float>(cfg::kLanternMoteRate);
		motes.maxParticles = cfg::kLanternMoteMax;
		motes.lifetime = 7.0f;
		motes.lifetimeJitter = 0.4f;
		motes.spawnBox = { 2.2f, 1.4f, 2.2f };
		motes.velocity = { 0.0f, -0.01f, 0.0f };
		motes.velocityJitter = { 0.05f, 0.03f, 0.05f };
		motes.turbulence = 0.06f;
		motes.drag = 0.6f;
		motes.sizeStart = motes.sizeEnd = 0.012f;
		motes.sizeJitter = 0.4f;
		motes.colorStart = { 0.95f, 0.9f, 0.8f, 0.45f };
		motes.colorEnd = { 0.95f, 0.9f, 0.8f, 0.0f };
		motes.square = true;
		motes.enabled = false;
		scene.Add(lanternMotes, motes);
	}
}

void UndertimeModule::CountCrystals(const VoxelWorld::Box& box)
{
	// Recount every 2 m cell the box touches.
	constexpr int kCell = 2 * cfg::kVoxelsPerMeter;
	for (int cz = box.min[2] / kCell; cz <= box.max[2] / kCell; ++cz)
		for (int cy = box.min[1] / kCell; cy <= box.max[1] / kCell; ++cy)
			for (int cx = box.min[0] / kCell; cx <= box.max[0] / kCell; ++cx)
			{
				int count = 0;
				for (int z = cz * kCell; z < (cz + 1) * kCell && z < cfg::kVoxZ; ++z)
					for (int y = cy * kCell; y < (cy + 1) * kCell && y < cfg::kVoxY; ++y)
						for (int x = cx * kCell; x < (cx + 1) * kCell && x < cfg::kVoxX; ++x)
							count += world.Get(x, y, z) == Block::TimeCrystal ? 1 : 0;
				const uint32_t key = static_cast<uint32_t>(cx) | (static_cast<uint32_t>(cy) << 10) | (static_cast<uint32_t>(cz) << 20);
				if (count > 0) crystalCells[key] = count;
				else crystalCells.erase(key);
			}
}

void UndertimeModule::CreateShaftDust(Engine& engine)
{
	// Motes hanging in each natural shaft, over its foot: they only shine where the beam of
	// sun comes through (the particles are lit by the sun's shadows).
	Scene& scene = engine.GetScene();
	for (Entity e : shaftDust) scene.DestroyEntity(e);
	shaftDust.clear();
	for (const CaveShaft& shaft : layout.shafts)
	{
		const Entity entity = scene.CreateEntity();   // no EntityInfo: never saved
		Transform transform;
		transform.position = { shaft.bottom.x, shaft.bottom.y + 2.5f, shaft.bottom.z - 1.0f };
		scene.Add(entity, transform);
		ParticleEmitter motes;
		motes.rate = static_cast<float>(cfg::kShaftDustRate);
		motes.maxParticles = cfg::kShaftDustMax;
		motes.lifetime = 10.0f;
		motes.lifetimeJitter = 0.4f;
		motes.spawnBox = { cfg::kShaftRadius + 0.8f, 3.0f, cfg::kShaftRadius + 1.5f };
		motes.velocity = { 0.02f, -0.02f, 0.0f };
		motes.velocityJitter = { 0.05f, 0.04f, 0.05f };
		motes.turbulence = 0.08f;
		motes.drag = 0.5f;
		motes.sizeStart = motes.sizeEnd = 0.035f;
		motes.sizeJitter = 0.4f;
		motes.colorStart = { 0.9f, 0.85f, 0.75f, 0.8f };   // fading out over their life
		motes.colorEnd = { 0.9f, 0.85f, 0.75f, 0.0f };
		motes.square = true;
		scene.Add(entity, motes);
		shaftDust.push_back(entity);
	}
}

// =============================================================================
// The outside clock
// =============================================================================

std::vector<float> UndertimeModule::ZoneStarts() const
{
	std::vector<float> starts;
	for (const CaveZone& zone : layout.zones) starts.push_back(zone.z0);
	return starts;
}

void UndertimeModule::SetEra(Engine& engine, int newEra)
{
	newEra = std::clamp(newEra, era::kFirst, era::kLast);
	if (newEra == era) return;
	era = newEra;
	props.ApplyEra(engine, engine.GetScene(), era, &engine.GetPhysics());
}

void UndertimeModule::UpdateClock(Engine& engine, float dt)
{
	const bool inside = IsInside();
	timeRate = OutsideClock::Rate(inside, player.position.z, ZoneStarts()) * std::pow(10.0f, static_cast<float>(debugTimeScale));
	clock.Advance(dt, timeRate);
	// Clouds drift with outside time: on the surface at the wind's speed, faster as time
	// speeds up underground.
	const double speedUp = timeRate / cfg::kSurfaceRate;
	cloudDrift[0] += cfg::kCloudWind[0] * dt * speedUp;
	cloudDrift[1] += cfg::kCloudWind[1] * dt * speedUp;
	const float cloudSpeed = std::hypot(cfg::kCloudWind[0], cfg::kCloudWind[1]) * static_cast<float>(speedUp);
	const float crossing = cfg::kCloudScale / std::max(cloudSpeed, 0.01f);   // seconds for a cloud to pass
	const float blurTarget = std::clamp((cfg::kCloudBlurSlow - crossing) / (cfg::kCloudBlurSlow - cfg::kCloudBlurFast), 0.0f, 1.0f);
	cloudBlur += (blurTarget - cloudBlur) * std::min(1.0f, dt / 0.5f);
	strobe += (StrobeBlend(timeRate) - strobe) * std::min(1.0f, dt / 0.5f);

	// The town changes only while you can't see it: deep in, or inside and facing away from
	// the mouth.
	const float facingIn = std::cos(player.yaw) * std::cos(player.pitch);
	const bool valleyUnseen = inside && (player.position.z > cfg::kHideValleyDepth || (player.position.z > 8.0f && facingIn > 0.3f));
	if (clock.Era() != era && valleyUnseen) SetEra(engine, clock.Era());

	// Coming out: how long were you gone?
	if (inside && !wasInside)
	{
		daysAtEntry = clock.Days();
		eraAtEntry = era;
	}
	if (!inside && wasInside)
	{
		const double away = clock.Days() - daysAtEntry;
		if (away > 1.0 / 24.0)
		{
			std::string text = "While you were inside, " + OutsideClock::SpanText(away) + " passed.";
			if (era != eraAtEntry) text += std::string("\nStillwell: ") + era::Name(era) + ".";
			messages.Show(text, cfg::kMessageSeconds + 1.5f);
		}
	}
	wasInside = inside;
}

void UndertimeModule::DrawWatch(const hud::Frame& frame)
{
	std::vector<std::string> lines;
	lines.push_back(clock.DateText());
	char line[96];
	const int zone = OutsideClock::Zone(IsInside(), player.position.z, ZoneStarts());
	const float minutesPerMinute = timeRate;   // outside minutes per real minute (= seconds per second)
	std::string pace;
	if (minutesPerMinute >= 60.0f * 24.0f * 300.0f) pace = "1 min = 1 year";
	else if (minutesPerMinute >= 60.0f * 24.0f * 25.0f) pace = "1 min = 1 month";
	else if (minutesPerMinute >= 60.0f * 24.0f * 6.0f) pace = "1 min = 1 week";
	else if (minutesPerMinute >= 60.0f * 20.0f) pace = "1 min = 1 day";
	else { std::snprintf(line, sizeof(line), "1 min = %.1f hours", minutesPerMinute / 60.0f); pace = line; }
	std::snprintf(line, sizeof(line), "%s  %s", zone >= 0 ? ("Zone " + std::to_string(zone + 1)).c_str() : (IsInside() ? "Entrance" : "Outside"), pace.c_str());
	lines.push_back(line);
	lines.push_back("Since you arrived: " + OutsideClock::SpanText(clock.Days() - cfg::kArrivalHour / 24.0));

	// Warn when the next era is near at this pace.
	bool warning = false;
	const int due = clock.Era();
	if (due < era::kLast)
	{
		const double daysLeft = OutsideClock::EraStartYears(due + 1) * 365.0 - clock.Days();
		const double secondsLeft = daysLeft * 86400.0 / timeRate;
		if (secondsLeft < cfg::kEraWarningSeconds)
		{
			warning = true;
			lines.push_back(std::string("Soon: ") + era::Name(due + 1));
		}
	}
	const DayLight day = DayLightAt(clock.HourOfDay());
	hud::Watch(frame, clock.HourOfDay(), clock.PersonalSeconds(), day.night, strobe, warning, time, lines);
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
				"1  pick   2  shovel   (or mouse wheel)        Esc  pause",
				"Hold left mouse: the pick mines, the shovel scoops loose blocks into your backpack",
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

	if (tool == Tool::Pick)
	{
		const char* label = !target.valid ? "" : (target.fragment != kInvalidBody ? "Boulder" : cfg::Info(target.block).name);
		const ImU32 labelColor = (target.valid && target.block != Block::Stone && !cfg::Unbreakable(target.block))
			? ToImColor(cfg::Info(target.block).color) : IM_COL32(230, 222, 210, 255);
		// The ring shows the swing: it fills as the pick winds up and lands when it closes.
		const bool swinging = swingAmount > 0.0f && target.valid && !cfg::Unbreakable(target.block);
		hud::Crosshair(frame, swinging ? std::max(swingTimer, 0.001f) : 0.0f, label, labelColor);
	}
	else
	{
		char label[48] = "";
		if (!scoopPreview.empty()) std::snprintf(label, sizeof(label), "%d loose block%s", static_cast<int>(scoopPreview.size()), scoopPreview.size() == 1 ? "" : "s");
		hud::Crosshair(frame, wasShovelling && !scoopPreview.empty() ? std::max(shovelTimer, 0.001f) : 0.0f, label, IM_COL32(230, 222, 210, 255));
	}

	char depthLine[64];
	if (IsInside())
		std::snprintf(depthLine, sizeof(depthLine), "Depth  %.0f m", Depth() * 10.0f);
	else
		std::snprintf(depthLine, sizeof(depthLine), "Outside");
	char renderLine[64];
	std::snprintf(renderLine, sizeof(renderLine), "%s  (V)", worldRenderer == WorldRenderer::RayMarched ? "Ray-marched voxels" : "Voxel meshes");
	const char* toolLine = tool == Tool::Pick ? "[1 Pick]   2 Shovel" : " 1 Pick   [2 Shovel]";
	const std::string eraLine = std::string("Era  ") + era::Name(era) + "  (N)";
	hud::Panel(frame, 0, { "UNDERTIME  " + buildStamp, depthLine, renderLine, toolLine, eraLine });
	DrawWatch(frame);

	// Backpack, with what you just scooped up on top.
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
	{
		char line[48];
		std::snprintf(line, sizeof(line), "Backpack  %d / %d", BackpackCount(), cfg::kBackpackCapacity);
		bag.push_back(line);
	}
	for (int b = 0; b < static_cast<int>(Block::Count); ++b)
	{
		if (backpack[b] == 0) continue;
		char line[48];
		std::snprintf(line, sizeof(line), "%-14s %d", cfg::Info(static_cast<Block>(b)).name, backpack[b]);
		bag.push_back(line);
	}
	hud::Panel(frame, 1, bag);

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
		ImGui::Text("Loose blocks  %d  (live %d, %d waiting to settle)", blocks.Count(), blocks.LiveCount(), blocks.PendingCount());
		ImGui::Text("              settled %d in %d piles", blocks.SettledCount(), blocks.PileCount());
		ImGui::Text("              crumbled %d, put back at the mine mouth %d", blocks.Crumbled(), blocks.Rescued());
		ImGui::Text("Boulders      %d  (broke off %d)", fragments.Count(), detachedCount);
		ImGui::Text("Pile rebuild  %.2f ms (mesh %.2f, collision %.2f)", blocks.LastPileBuildMs(), blocks.LastPileMeshMs(), blocks.LastPileCollisionMs());
		ImGui::Text("              %d pile(s), %d blocks", blocks.LastPileBuilds(), blocks.LastPileBlocks());
		if (ImGui::TreeNodeEx("Loose block tests", ImGuiTreeNodeFlags_DefaultOpen))
		{
			LooseBlocks::Options options = blocks.GetOptions();
			bool changed = false;
			changed |= ImGui::Checkbox("Cap (old): past 3000 the oldest crumble away", &options.cap);
			changed |= ImGui::Checkbox("Piles: blocks at rest settle into piles", &options.piles);
			changed |= ImGui::Checkbox("Solid blocks: falling blocks block the player", &options.solidBlocks);
			changed |= ImGui::Checkbox("Solid piles: piles block the player", &options.solidPiles);
			if (changed) blocks.SetOptions(engine, options);
			if (ImGui::Button("Drop 2000 test blocks")) DropTestBlocks(engine, 2000);
			ImGui::SameLine();
			if (ImGui::Button("Drop 10000")) DropTestBlocks(engine, 10000);
			ImGui::TreePop();
		}
		ImGui::Text("Physics       %u bodies, %u active, %.2f ms", engine.GetPhysics().Stats().bodies, engine.GetPhysics().Stats().activeBodies, engine.GetPhysics().Stats().stepMs);
		ImGui::Text("Collision     %.2f ms for %d chunk(s)   support check %d voxels", collision.LastFlushMs(), collision.LastFlushChunks(), lastSupportVisited);
		ImGui::Text("Seed %u   generated in %.0f ms   collision built in %.0f ms", seed, generationMs, collision.BuildMs());
		for (size_t i = 0; i < layout.zones.size(); ++i)
		{
			const CaveZone& zone = layout.zones[i];
			int pockets = 0;
			for (const CavePocket& p : layout.pockets) pockets += p.zone == static_cast<int>(i) ? 1 : 0;
			ImGui::Text("Zone %zu  %3.0f-%3.0f m  rooms %zu  tunnels %zu  pockets %d", i + 1, zone.z0, zone.z1, zone.rooms.size(), zone.tunnels.size(), pockets);
		}
		ImGui::TextDisabled("F6 drop a test rock   F7 cave-in test   F8 new world   F9 next zone   F10 look down   F4 physics bounds");
		ImGui::SliderInt("Pick level", &pickLevel, 0, 3);
		ImGui::SliderInt("Lantern level", &lanternLevel, 0, 3);
	}
}
