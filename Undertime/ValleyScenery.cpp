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

	// Water: translucent, each block of the surface slowly brightening and dimming.
	SurfaceShaderOptions waterOptions;
	waterOptions.translucent = true;
	mWaterShader = renderer.CreateSurfaceShader("UNDERTIME water",
		"SurfaceOutput EvaluateSurface(SurfaceInput input)\n"
		"{\n"
		"\tSurfaceOutput s = DefaultSurfaceOutput(input);\n"
		"\tconst float2 cell = floor(input.positionW.xz * 4.0);\n"
		"\tconst float h = frac(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);\n"
		"\ts.baseColor *= 0.85 + 0.3 * (0.5 + 0.5 * sin(input.time * (0.8 + h) + h * 6.2831));\n"
		"\tconst float facing = saturate(dot(input.normalW, input.viewDirW));\n"
		"\ts.alpha = lerp(0.95, s.alpha, facing);\n"
		"\treturn s;\n"
		"}\n", waterOptions);
	MaterialDesc water;
	const XMFLOAT3 waterColour = color::Linear(cfg::kWaterColor);
	water.baseColor = { waterColour.x, waterColour.y, waterColour.z, cfg::kWaterOpacity };
	water.roughness = 0.08f;
	water.shader = mWaterShader;
	mWaterMaterial = renderer.CreateMaterial(water);

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
			addMesh(ground, blockMaterial);
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
	if (mWaterShader.IsValid()) renderer.DestroySurfaceShader(mWaterShader);
	mWaterMaterial = {};
	mWaterShader = {};
	mVisible = true;
}
