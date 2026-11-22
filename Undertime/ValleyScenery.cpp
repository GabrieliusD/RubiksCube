#include "ValleyScenery.h"
#include "TerrainMesh.h"
#include "Config.h"
#include "ColorUtil.h"
#include <Core/Assets.h>
#include <Core/Log.h>
#include <Voxel/VoxelModel.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace DirectX;

namespace
{
	float CoverHash(int x, int z, uint32_t salt)
	{
		uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u + salt * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
	}
}

void ValleyScenery::Build(Engine& engine, Scene& scene, const Valley& valley, PhysicsWorld* physics)
{
	Destroy(engine, scene, physics);
	const auto start = std::chrono::steady_clock::now();
	Renderer& renderer = engine.GetRenderer();
	AssetManager& assets = engine.GetAssets();

	const MaterialHandle blockMaterial = assets.GetMaterial("engine/VoxelModel");
	// The terrain gets wet after rain (Shaders/Ground.hlsl).
	const MaterialHandle wetGround = assets.GetMaterial("Materials/Ground.mat");
	const MaterialHandle groundMaterial = wetGround.IsValid() ? wetGround : blockMaterial;
	std::array<XMFLOAT3, static_cast<int>(Ground::Count)> colours{};
	colours[static_cast<int>(Ground::Grass)] = color::Linear(cfg::kTerrainGrass);
	colours[static_cast<int>(Ground::Grass2)] = color::Linear(cfg::kTerrainGrass2);
	colours[static_cast<int>(Ground::Dirt)] = color::Linear(cfg::kTerrainDirt);
	colours[static_cast<int>(Ground::Sand)] = color::Linear(cfg::kTerrainSand);
	colours[static_cast<int>(Ground::Gravel)] = color::Linear(cfg::kTerrainGravel);
	colours[static_cast<int>(Ground::Rock)] = color::Linear(cfg::kTerrainRock);
	colours[static_cast<int>(Ground::Rock2)] = color::Linear(cfg::kTerrainRock2);
	colours[static_cast<int>(Ground::Snow)] = color::Linear(cfg::kTerrainSnow);
	colours[static_cast<int>(Ground::Path)] = color::Linear(cfg::kTerrainPath);
	colours[static_cast<int>(Ground::Moss)] = color::Linear(cfg::kTerrainMoss);

	// Water that flows (Content/Shaders/Water.hlsl), steered by a flow map of the river and
	// the lake: which way and how fast the water runs, how deep it is, and the foam below
	// the falls. Spray rises at the falls nearest the town.
	{
		float x0, z0, x1, z1;
		std::vector<uint8_t> texels = BuildFlowMap(valley, x0, z0, x1, z1);
		mFlowMap = renderer.CreateTexture(static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(z1 - z0), texels.data(), false);
		MaterialDesc water;
		const XMFLOAT3 waterColour = color::Linear(cfg::kWaterColor);
		water.baseColor = { waterColour.x, waterColour.y, waterColour.z, cfg::kWaterOpacity };
		water.roughness = 0.05f;
		water.shader = assets.GetSurfaceShader("Shaders/Water.hlsl");
		const XMFLOAT3 deep = color::Linear(cfg::kWaterDeep), shallow = color::Linear(cfg::kWaterShallow);
		water.params = { x0, z0, x1 - x0, z1 - z0, deep.x, deep.y, deep.z, shallow.x, shallow.y, shallow.z, cfg::kRiverMaxSpeed };
		water.textures = { mFlowMap };
		mWaterMaterial = renderer.CreateMaterial(water);
		mSprays = FallSprays(valley);
	}

	auto addMesh = [&](const MeshData& data, MaterialHandle material)
	{
		if (data.indices.empty()) return;
		const MeshHandle mesh = renderer.CreateMesh(data);
		mOwnedMeshes.push_back(mesh);
		mItems.push_back({ mesh, material, Transform{} });
	};
	// Ground cover models (tall grass, flowers, bushes, pebbles), baked once with their corner
	// at the origin; each vertex's alpha carries how far it sways (Shaders/Foliage.hlsl:
	// 0.99 at the foot, 0.01 at the tip).
	const MaterialHandle foliageMaterial = assets.GetMaterial("Materials/Foliage.mat");
	const MaterialHandle plantMaterial = foliageMaterial.IsValid() ? foliageMaterial : blockMaterial;
	enum CoverKind { CoverTuft, CoverTuftTall, CoverYellow, CoverWhite, CoverPurple, CoverBush, CoverPebbles, CoverKinds };
	const char* const kCoverModels[CoverKinds] = {
		"Models/Placeholders/GrassTuft.vox", "Models/Placeholders/GrassTuftTall.vox", "Models/Placeholders/FlowersYellow.vox",
		"Models/Placeholders/FlowersWhite.vox", "Models/Placeholders/FlowersPurple.vox", "Models/Placeholders/Bush.vox", "Models/Placeholders/Pebbles.vox" };
	MeshData coverMeshes[CoverKinds];
	float coverWidth[CoverKinds]{};
	for (int i = 0; i < CoverKinds; ++i)
	{
		VoxelModel model;
		if (!assets.ReadVoxelModel(kCoverModels[i], model)) continue;
		VoxelBakeOptions options;
		options.pivot = VoxelPivot::Corner;
		coverMeshes[i] = BakeVoxelMesh(model, options);
		const float height = std::max(model.size.y * cfg::kVoxelSize, 0.25f);
		const bool sways = i != CoverPebbles;
		for (Vertex& v : coverMeshes[i].vertices) v.color.w = sways ? 0.99f - 0.98f * std::clamp(v.position.y / height, 0.0f, 1.0f) : 0.99f;
		coverWidth[i] = model.size.x * cfg::kVoxelSize;
	}
	const std::vector<GroundPad>& pads = valley.Pads();
	const Perlin3 flowerNoise(977);
	// Scatters cover over a detailed chunk, standing on the patch's columns.
	auto scatterCover = [&](const TerrainPatch& patch, MeshData& out)
	{
		const float spacing = cfg::kCoverSpacing;
		const float size = patch.block * patch.n;
		const int cells = static_cast<int>(size / spacing);
		auto columnAt = [&](float x, float z, int& i, int& j) { i = static_cast<int>(std::floor((x - patch.x0) / patch.block)); j = static_cast<int>(std::floor((z - patch.z0) / patch.block)); };
		for (int cz = 0; cz < cells; ++cz)
			for (int cx = 0; cx < cells; ++cx)
			{
				const int gx = static_cast<int>(std::lround(patch.x0 / spacing)) + cx, gz = static_cast<int>(std::lround(patch.z0 / spacing)) + cz;
				const float x = patch.x0 + (cx + 0.15f + 0.7f * CoverHash(gx, gz, 41)) * spacing;
				const float z = patch.z0 + (cz + 0.15f + 0.7f * CoverHash(gx, gz, 42)) * spacing;
				int i, j;
				columnAt(x, z, i, j);
				if (i < 0 || j < 0 || i >= patch.n || j >= patch.n) continue;
				const Ground ground = patch.ground[patch.Index(i, j)];
				if (ground != Ground::Grass && ground != Ground::Grass2 && ground != Ground::Moss) continue;
				if (patch.water[patch.Index(i, j)] > patch.Top(i, j) - 0.5f) continue;
				bool onPad = false;
				for (const GroundPad& pad : pads)
					if (std::fabs(x - pad.x) < pad.halfX + cfg::kCoverClear && std::fabs(z - pad.z) < pad.halfZ + cfg::kCoverClear) { onPad = true; break; }
				if (onPad) continue;
				// What grows here.
				const float roll = CoverHash(gx, gz, 43);
				const float wooded = valley.ForestPatch(x, z);
				const float flowers = 0.5f + 0.5f * flowerNoise.Fbm(x / 18.0f, 0.5f, z / 18.0f, 2);
				int kind = -1;
				if (roll < cfg::kCoverPebbles) kind = CoverPebbles;
				else if (ground == Ground::Moss || wooded > cfg::kMossPatch - 0.15f)
				{
					if (roll < cfg::kCoverPebbles + cfg::kCoverBush) kind = CoverBush;
					else if (roll < cfg::kCoverPebbles + cfg::kCoverBush + 0.5f * cfg::kCoverGrass) kind = CoverTuft;
				}
				else if (flowers > cfg::kCoverFlowerPatch && roll < cfg::kCoverPebbles + cfg::kCoverFlowers)
					kind = flowers > cfg::kCoverFlowerPatch + 0.08f ? CoverPurple : (CoverHash(gx, gz, 44) < 0.55f ? CoverYellow : CoverWhite);
				else if (roll < cfg::kCoverPebbles + cfg::kCoverGrass)
					kind = CoverHash(gx, gz, 45) < 0.4f ? CoverTuftTall : CoverTuft;
				if (kind < 0 || coverMeshes[kind].vertices.empty()) continue;
				// On flat ground only (not hanging over a step), on the block grid.
				const float w = coverWidth[kind];
				const float x0 = std::floor((x - 0.5f * w) / patch.block) * patch.block, z0 = std::floor((z - 0.5f * w) / patch.block) * patch.block;
				int i0, j0, i1, j1;
				columnAt(x0 + 0.01f, z0 + 0.01f, i0, j0);
				columnAt(x0 + w - 0.01f, z0 + w - 0.01f, i1, j1);
				if (i0 < -1 || j0 < -1 || i1 > patch.n || j1 > patch.n) continue;
				const float top = patch.Top(i, j);
				bool flat = true;
				for (int b = 0; b < 4 && flat; ++b) flat = std::fabs(patch.Top(b & 1 ? i1 : i0, b & 2 ? j1 : j0) - top) < 0.01f;
				if (!flat) continue;
				// A quarter turn about the footprint centre keeps the blocks on the grid.
				const int turn = static_cast<int>(CoverHash(gx, gz, 46) * 4.0f) & 3;
				const float c = 0.5f * w;
				const uint32_t base = static_cast<uint32_t>(out.vertices.size());
				for (Vertex v : coverMeshes[kind].vertices)
				{
					float px = v.position.x - c, pz = v.position.z - c, nx = v.normal.x, nz = v.normal.z;
					for (int t = 0; t < turn; ++t) { std::swap(px, pz); pz = -pz; std::swap(nx, nz); nz = -nz; }
					v.position = { x0 + c + px, top + v.position.y, z0 + c + pz };
					v.normal = { nx, v.normal.y, nz };
					out.vertices.push_back(v);
				}
				for (uint32_t index : coverMeshes[kind].indices) out.indices.push_back(base + index);
			}
	};

	auto addCollision = [&](const MeshData& data)
	{
		if (physics && !data.indices.empty()) mBodies.push_back(physics->CreateStaticMesh(MeshTriangles(data)));
	};

	// Terrain chunks: 0.25 m blocks where Valley::Detailed, 1 m blocks elsewhere, with skirts
	// where the two meet. Sampling and meshing run on worker threads (the valley is
	// read-only); meshes and collision are made here afterwards.
	struct ChunkJob
	{
		float x0, z0, block;
		std::array<float, 4> skirts;
		MeshData ground, water, shape, cover;
	};
	const float chunkSize = cfg::kTerrainChunk, skirt = 3.0f;
	std::vector<ChunkJob> jobs;
	for (float z0 = cfg::kValleyWorld[1]; z0 < cfg::kValleyWorld[3]; z0 += chunkSize)
	{
		for (float x0 = cfg::kValleyWorld[0]; x0 < cfg::kValleyWorld[2]; x0 += chunkSize)
		{
			const float h = chunkSize * 0.5f;
			const bool detailed = valley.Detailed(x0 + h, z0 + h);
			auto differs = [&](float x, float z) { return valley.Detailed(x, z) != detailed ? skirt : 0.0f; };
			ChunkJob job;
			job.x0 = x0;
			job.z0 = z0;
			job.block = detailed ? cfg::kVoxelSize : 1.0f;
			job.skirts = { differs(x0 - h, z0 + h), differs(x0 + chunkSize + h, z0 + h), differs(x0 + h, z0 - h), differs(x0 + h, z0 + chunkSize + h) };
			jobs.push_back(std::move(job));
		}
	}
	std::atomic<size_t> nextJob{ 0 };
	const bool collision = physics != nullptr;
	auto work = [&]()
	{
		for (size_t i = nextJob++; i < jobs.size(); i = nextJob++)
		{
			ChunkJob& job = jobs[i];
			const TerrainPatch patch = SampleTerrain(valley, job.x0, job.z0, job.block, static_cast<int>(std::lround(chunkSize / job.block)));
			job.ground = BuildTerrainMesh(patch, colours, job.skirts);
			job.water = BuildWaterMesh(patch, { 1, 1, 1 });
			if (job.block < 0.5f) scatterCover(patch, job.cover);
			if (!collision) continue;
			// Collision: the same shape, except on mountains away from the detailed valley
			// (nobody walks there), where 4 m blocks are plenty.
			const bool remote = job.block > 0.5f && job.skirts == std::array<float, 4>{ 0, 0, 0, 0 };
			job.shape = remote ? BuildTerrainMesh(SampleTerrain(valley, job.x0, job.z0, 4.0f, static_cast<int>(chunkSize / 4.0f)), colours, job.skirts, true)
				: BuildTerrainMesh(patch, colours, job.skirts, true);
		}
	};
	std::vector<std::thread> workers;
	const unsigned threadCount = std::max(1u, std::thread::hardware_concurrency());
	for (unsigned t = 1; t < threadCount; ++t) workers.emplace_back(work);
	work();
	for (std::thread& worker : workers) worker.join();

	// Chunks merged into regions of kTerrainRegion x kTerrainRegion: far fewer meshes and bodies.
	size_t triangles = 0, coverTriangles = 0;
	const int region = cfg::kTerrainRegion;
	const int columns = static_cast<int>(std::lround((cfg::kValleyWorld[2] - cfg::kValleyWorld[0]) / chunkSize));
	const int rows = static_cast<int>(std::lround((cfg::kValleyWorld[3] - cfg::kValleyWorld[1]) / chunkSize));
	for (int rz = 0; rz < rows; rz += region)
	{
		for (int rx = 0; rx < columns; rx += region)
		{
			MeshData ground, waterMesh, shape, cover;
			for (int cz = rz; cz < std::min(rz + region, rows); ++cz)
			{
				for (int cx = rx; cx < std::min(rx + region, columns); ++cx)
				{
					const ChunkJob& job = jobs[static_cast<size_t>(cz) * columns + cx];
					AppendMesh(ground, job.ground);
					AppendMesh(waterMesh, job.water);
					AppendMesh(shape, job.shape);
					AppendMesh(cover, job.cover);
				}
			}
			addMesh(ground, groundMaterial);
			addMesh(cover, plantMaterial);
			coverTriangles += cover.indices.size() / 3;
			addMesh(waterMesh, mWaterMaterial);
			addCollision(shape);
			triangles += ground.indices.size() / 3;
		}
	}

	// The forest: placeholder voxel models (Content/Models/Placeholders, editable in
	// MagicaVoxel), standing on whole blocks and turned in quarter turns so they line up
	// with the ground. Far trees use a coarse model baked with 1 m voxels.
	const char* const kTreeModels[] = {
		"Models/Placeholders/ConiferSmall.vox", "Models/Placeholders/ConiferMedium.vox",
		"Models/Placeholders/ConiferLarge.vox", "Models/Placeholders/Broadleaf.vox" };
	MeshHandle treeMeshes[4];
	for (int i = 0; i < 4; ++i) treeMeshes[i] = assets.GetMesh(kTreeModels[i]);
	MeshHandle farTree;
	VoxelModel farModel;
	if (assets.ReadVoxelModel("Models/Placeholders/ConiferFar.vox", farModel))
	{
		VoxelBakeOptions options;
		options.voxelSize = 1.0f;
		farTree = renderer.CreateMesh(BakeVoxelMesh(farModel, options));
		mOwnedMeshes.push_back(farTree);
	}
	int trees = 0;
	for (const TreeSpot& tree : valley.Trees())
	{
		const float block = tree.distant ? 1.0f : cfg::kVoxelSize;
		const MeshHandle mesh = tree.distant ? farTree : treeMeshes[tree.kind];
		if (!mesh.IsValid()) continue;
		const float x = std::floor(tree.x / block) * block, z = std::floor(tree.z / block) * block;
		const float y = std::round(valley.Height(x + 0.5f * block, z + 0.5f * block) / block) * block;
		Transform transform;
		transform.position = { x, y, z };
		transform.SetRotationEuler(0.0f, std::floor(tree.yaw / XM_PIDIV2) * XM_PIDIV2, 0.0f);
		mItems.push_back({ mesh, plantMaterial, transform });
		if (physics && !tree.distant) mBodies.push_back(physics->CreateStaticBox({ x, y + 1.6f, z }, { 0.25f, 1.6f, 0.25f }));   // the trunk
		++trees;
	}

	// Rock outcrops on steep ground: sunk into the slope (standing on its lowest point under
	// them), near ones block you (boxes baked from the model), far ones are baked with 1 m
	// blocks, four times the size, as crags on the distant mountains.
	const char* const kOutcropModels[] = { "Models/Placeholders/OutcropSmall.vox", "Models/Placeholders/OutcropMedium.vox", "Models/Placeholders/OutcropLarge.vox" };
	MeshHandle outcropNear[3], outcropFar[3];
	std::vector<PhysicsBox> outcropBoxes[3];
	XMFLOAT3 outcropSize[3]{};
	for (int i = 0; i < 3; ++i)
	{
		outcropNear[i] = assets.GetMesh(kOutcropModels[i]);
		VoxelModel model;
		if (!assets.ReadVoxelModel(kOutcropModels[i], model)) continue;
		outcropBoxes[i] = BakeVoxelBoxes(model);
		outcropSize[i] = { model.size.x * cfg::kVoxelSize, model.size.y * cfg::kVoxelSize, model.size.z * cfg::kVoxelSize };
		VoxelBakeOptions options;
		options.voxelSize = 1.0f;
		outcropFar[i] = renderer.CreateMesh(BakeVoxelMesh(model, options));
		mOwnedMeshes.push_back(outcropFar[i]);
	}
	int outcrops = 0;
	for (const OutcropSpot& spot : valley.Outcrops())
	{
		const MeshHandle mesh = spot.distant ? outcropFar[spot.kind] : outcropNear[spot.kind];
		if (!mesh.IsValid()) continue;
		const float scale = spot.distant ? 4.0f : 1.0f;
		const float block = spot.distant ? 1.0f : cfg::kVoxelSize;
		const float half = 0.5f * scale * std::max(outcropSize[spot.kind].x, outcropSize[spot.kind].z);
		float ground = 1e9f;
		for (int a = 0; a < 4; ++a)
			ground = std::min(ground, valley.Height(spot.x + half * 0.7f * ((a & 1) ? 1.0f : -1.0f), spot.z + half * 0.7f * ((a & 2) ? 1.0f : -1.0f)));
		Transform transform;
		transform.position = { std::floor(spot.x / block) * block, std::floor(ground / block) * block - block, std::floor(spot.z / block) * block };
		transform.SetRotationEuler(0.0f, std::floor(spot.yaw / XM_PIDIV2) * XM_PIDIV2, 0.0f);
		mItems.push_back({ mesh, blockMaterial, transform });
		if (physics && !spot.distant)
		{
			const XMMATRIX world = transform.Matrix();
			for (const PhysicsBox& box : outcropBoxes[spot.kind])
			{
				BoundingBox placed;
				BoundingBox(box.center, box.halfExtents).Transform(placed, world);
				mBodies.push_back(physics->CreateStaticBox(placed.Center, placed.Extents));
			}
		}
		++outcrops;
	}

	// Grates over the natural shafts' openings (the ground has holes there, Valley.cpp), at
	// the lowest ground around the hole, so you walk over them and look down through the
	// gaps. They block you; light falls through.
	const MeshHandle grate = assets.GetMesh("Models/Placeholders/ShaftGrate.vox");
	for (const cfg::Shaft& s : cfg::kShafts)
	{
		if (!grate.IsValid()) break;
		const float rim = cfg::kShaftRadius + cfg::kShaftHoleMargin + 0.6f;
		float y = 1e9f;
		for (int a = 0; a < 8; ++a)
			y = std::min(y, valley.Height(s.x + rim * std::cos(a * XM_PIDIV4), s.z + rim * std::sin(a * XM_PIDIV4)));
		y = std::floor(y / cfg::kVoxelSize) * cfg::kVoxelSize;
		Transform transform;
		transform.position = { s.x, y, s.z };
		mItems.push_back({ grate, blockMaterial, transform });
		const float half = cfg::kShaftRadius + cfg::kShaftHoleMargin;
		if (physics) mBodies.push_back(physics->CreateStaticBox({ s.x, y + 0.25f, s.z }, { half, 0.25f, half }));
	}

	Spawn(scene);
	mBuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	CHERRY_LOG("UNDERTIME valley: %zu terrain triangles, %zu ground cover triangles, %d trees, %d outcrops, %s collision, built in %.0f ms on %u threads",
		triangles, coverTriangles, trees, outcrops, physics ? "with" : "no", mBuildMs, threadCount);
}

void ValleyScenery::Spawn(Scene& scene)
{
	mEntities.clear();
	mEntities.reserve(mItems.size());
	mRoot = scene.CreateEntity();   // no EntityInfo: never saved
	scene.Add(mRoot, Transform{});
	for (const Item& item : mItems)
	{
		const Entity entity = scene.CreateEntity();
		scene.Add(entity, item.transform);
		scene.Add(entity, Parent{ mRoot });
		MeshRenderer renderer{ item.mesh, item.material };
		renderer.visible = mVisible;
		scene.Add(entity, renderer);
		mEntities.push_back(entity);
	}
	// Spray at the falls: fine white mist rising off the foam.
	for (const XMFLOAT3& at : mSprays)
	{
		const Entity entity = scene.CreateEntity();
		Transform transform;
		transform.position = at;
		scene.Add(entity, transform);
		scene.Add(entity, Parent{ mRoot });
		ParticleEmitter spray;
		spray.enabled = mVisible;
		spray.rate = 14.0f;
		spray.maxParticles = 60;
		spray.lifetime = 1.6f;
		spray.lifetimeJitter = 0.4f;
		spray.spawnBox = { cfg::kRiverHalfWidth * 0.8f, 0.1f, cfg::kRiverHalfWidth * 0.8f };
		spray.velocity = { 0.0f, 0.6f, 0.0f };
		spray.velocityJitter = { 0.4f, 0.3f, 0.4f };
		spray.gravity = 0.3f;
		spray.drag = 1.2f;
		spray.turbulence = 0.4f;
		spray.sizeStart = 0.12f;
		spray.sizeEnd = 0.45f;
		spray.colorStart = { 0.92f, 0.95f, 0.97f, 0.35f };
		spray.colorEnd = { 0.92f, 0.95f, 0.97f, 0.0f };
		scene.Add(entity, spray);
		mEntities.push_back(entity);
	}
}

std::vector<uint8_t> ValleyScenery::BuildFlowMap(const Valley& valley, float& x0, float& z0, float& x1, float& z1)
{
	// Over the river and the lake (one texel per metre), with a margin for the banks.
	const float margin = cfg::kRiverHalfWidth + cfg::kRiverBank + 4.0f;
	x0 = cfg::kLakeCenter[0] - cfg::kLakeRadius[0] * 1.6f;
	x1 = cfg::kLakeCenter[0] + cfg::kLakeRadius[0] * 1.6f;
	z0 = cfg::kLakeCenter[1] - cfg::kLakeRadius[1] * 1.6f;
	z1 = cfg::kLakeCenter[1] + cfg::kLakeRadius[1] * 1.6f;
	for (const auto& point : cfg::kRiverPoints)
	{
		x0 = std::min(x0, point[0] - margin); x1 = std::max(x1, point[0] + margin);
		z0 = std::min(z0, point[1] - margin); z1 = std::max(z1, point[1] + margin);
	}
	x0 = std::floor(x0); z0 = std::floor(z0); x1 = std::ceil(x1); z1 = std::ceil(z1);
	const int width = static_cast<int>(x1 - x0), height = static_cast<int>(z1 - z0);
	std::vector<uint8_t> texels(static_cast<size_t>(width) * height * 4, 0);
	auto encode = [](float v) { return static_cast<uint8_t>(std::clamp(std::lround((0.5f + 0.5f * v / cfg::kRiverMaxSpeed) * 255.0f), 0l, 255l)); };
	auto row = [&](int j)
	{
		for (int i = 0; i < width; ++i)
		{
			uint8_t* out = &texels[(static_cast<size_t>(j) * width + i) * 4];
			out[0] = out[1] = 128;   // still
			const float x = x0 + i + 0.5f, z = z0 + j + 0.5f;
			float along = 0.0f;
			const float distance = valley.RiverDistance(x, z, &along);
			const float lx = (x - cfg::kLakeCenter[0]) / cfg::kLakeRadius[0], lz = (z - cfg::kLakeCenter[1]) / cfg::kLakeRadius[1];
			const bool nearLake = lx * lx + lz * lz < 2.0f;
			if (distance > margin && !nearLake) continue;
			float ground, water;
			valley.Sample(x, z, ground, water);
			if (water == Valley::kNoWater) continue;
			// Downstream: the way the distance along the river grows.
			float a, b, c, d;
			valley.RiverDistance(x + 0.5f, z, &a);
			valley.RiverDistance(x - 0.5f, z, &b);
			valley.RiverDistance(x, z + 0.5f, &c);
			valley.RiverDistance(x, z - 0.5f, &d);
			float dx = a - b, dz = c - d;
			const float length = std::sqrt(dx * dx + dz * dz);
			if (length > 1e-4f) { dx /= length; dz /= length; }
			float speed = cfg::kLakeSpeed;
			if (distance < cfg::kRiverHalfWidth + 1.0f)
			{
				const float slope = (valley.RiverLevelAt(along - 4.0f) - valley.RiverLevelAt(along + 4.0f)) / 8.0f;
				const float middle = std::clamp(1.0f - distance / (cfg::kRiverHalfWidth + 1.0f), 0.0f, 1.0f);
				speed = std::min(cfg::kRiverSpeed + slope * cfg::kRiverSlopeSpeed, cfg::kRiverMaxSpeed) * (0.4f + 0.6f * std::sqrt(middle));
			}
			out[0] = encode(dx * speed);
			out[1] = encode(dz * speed);
			out[2] = static_cast<uint8_t>(std::clamp(std::lround((water - ground) / 2.0f * 255.0f), 0l, 255l));
			// Foam below a fall, fading downstream.
			float foam = 0.0f;
			if (distance < cfg::kRiverHalfWidth + 1.0f)
				for (float back = -1.0f; back <= cfg::kFallFoamLength * 2.0f; back += 1.0f)
					if (valley.RiverLevelAt(along - back - 1.0f) - valley.RiverLevelAt(along - back) > cfg::kFallDrop)
						foam = std::max(foam, std::exp(-std::max(back, 0.0f) / cfg::kFallFoamLength));
			out[3] = static_cast<uint8_t>(std::lround(foam * 255.0f));
		}
	};
	std::atomic<int> nextRow{ 0 };
	auto work = [&]() { for (int j = nextRow++; j < height; j = nextRow++) row(j); };
	std::vector<std::thread> workers;
	for (unsigned t = 1; t < std::max(1u, std::thread::hardware_concurrency()); ++t) workers.emplace_back(work);
	work();
	for (std::thread& worker : workers) worker.join();

	// Smooth the flow (twice over 5x5 texels of water): where the river bends, the nearest
	// stretch of its centre line switches and the direction would jump, which shows as
	// zigzag streaks in the ripples.
	std::vector<float> flow(static_cast<size_t>(width) * height * 2), smoothed(flow.size());
	std::vector<uint8_t> wet(static_cast<size_t>(width) * height);
	for (size_t k = 0; k < wet.size(); ++k)
	{
		wet[k] = texels[k * 4 + 0] != 128 || texels[k * 4 + 1] != 128 || texels[k * 4 + 2] != 0;
		flow[k * 2 + 0] = (texels[k * 4 + 0] / 255.0f - 0.5f) * 2.0f;
		flow[k * 2 + 1] = (texels[k * 4 + 1] / 255.0f - 0.5f) * 2.0f;
	}
	for (int pass = 0; pass < 2; ++pass)
	{
		for (int j = 0; j < height; ++j)
			for (int i = 0; i < width; ++i)
			{
				const size_t k = static_cast<size_t>(j) * width + i;
				float sx = 0.0f, sz = 0.0f, n = 0.0f;
				if (wet[k])
					for (int dj = -2; dj <= 2; ++dj)
						for (int di = -2; di <= 2; ++di)
						{
							const int ii = i + di, jj = j + dj;
							if (ii < 0 || jj < 0 || ii >= width || jj >= height) continue;
							const size_t q = static_cast<size_t>(jj) * width + ii;
							if (!wet[q]) continue;
							sx += flow[q * 2]; sz += flow[q * 2 + 1]; n += 1.0f;
						}
				smoothed[k * 2] = n > 0.0f ? sx / n : flow[k * 2];
				smoothed[k * 2 + 1] = n > 0.0f ? sz / n : flow[k * 2 + 1];
			}
		flow.swap(smoothed);
	}
	for (size_t k = 0; k < wet.size(); ++k)
	{
		if (!wet[k]) continue;
		texels[k * 4 + 0] = encode(flow[k * 2] * cfg::kRiverMaxSpeed);
		texels[k * 4 + 1] = encode(flow[k * 2 + 1] * cfg::kRiverMaxSpeed);
	}
	return texels;
}

std::vector<XMFLOAT3> ValleyScenery::FallSprays(const Valley& valley)
{
	// Each fall along the river, the ones nearest the town first.
	std::vector<XMFLOAT3> falls;
	const float length = valley.RiverLength();
	for (float along = 1.0f; along < length - 1.0f; along += 1.0f)
	{
		const float drop = valley.RiverLevelAt(along - 1.0f) - valley.RiverLevelAt(along);
		if (drop <= cfg::kFallDrop) continue;
		float x, z;
		valley.RiverPointAt(along + 0.5f, x, z);
		if (!falls.empty() && std::fabs(falls.back().x - x) + std::fabs(falls.back().z - z) < 3.0f) continue;
		falls.push_back({ x, valley.RiverLevelAt(along) + 0.05f, z });
	}
	const XMFLOAT2 town{ cfg::kSpawnX, -120.0f };
	std::sort(falls.begin(), falls.end(), [&](const XMFLOAT3& a, const XMFLOAT3& b)
	{
		auto d2 = [&](const XMFLOAT3& p) { const float dx = p.x - town.x, dz = p.z - town.y; return dx * dx + dz * dz; };
		return d2(a) < d2(b);
	});
	if (falls.size() > static_cast<size_t>(cfg::kFallSprayMax)) falls.resize(cfg::kFallSprayMax);
	return falls;
}

bool ValleyScenery::EnsureEntities(Scene& scene)
{
	if (mItems.empty()) return false;
	const Entity first = mEntities.empty() ? INVALID_ENTITY : mEntities.front();
	if (scene.IsAlive(mRoot) && scene.IsAlive(first) && scene.Has<MeshRenderer>(first) && scene.Get<MeshRenderer>(first).mesh == mItems.front().mesh) return false;
	Spawn(scene);
	return true;
}

void ValleyScenery::SetVisible(Scene& scene, bool visible)
{
	if (visible == mVisible) return;
	mVisible = visible;
	for (Entity entity : mEntities)
	{
		if (scene.IsAlive(entity) && scene.Has<MeshRenderer>(entity)) scene.Get<MeshRenderer>(entity).visible = visible;
		if (scene.IsAlive(entity) && scene.Has<ParticleEmitter>(entity)) scene.Get<ParticleEmitter>(entity).enabled = visible;
	}
}

void ValleyScenery::Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics)
{
	for (Entity entity : mEntities)
	{
		if (scene.IsAlive(entity)) scene.DestroyEntity(entity);
	}
	mEntities.clear();
	if (scene.IsAlive(mRoot)) scene.DestroyEntity(mRoot);
	mRoot = INVALID_ENTITY;
	mItems.clear();
	Renderer& renderer = engine.GetRenderer();
	for (MeshHandle mesh : mOwnedMeshes) renderer.DestroyMesh(mesh);
	mOwnedMeshes.clear();
	if (physics) for (BodyId body : mBodies) physics->RemoveBody(body);
	mBodies.clear();
	if (mWaterMaterial.IsValid()) renderer.DestroyMaterial(mWaterMaterial);
	if (mFlowMap.IsValid()) renderer.DestroyTexture(mFlowMap);
	mWaterMaterial = {};
	mFlowMap = {};
	mSprays.clear();
	mVisible = true;
}
