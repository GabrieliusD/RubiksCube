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
		MeshData ground, water, shape;
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
	size_t triangles = 0;
	const int region = cfg::kTerrainRegion;
	const int columns = static_cast<int>(std::lround((cfg::kValleyWorld[2] - cfg::kValleyWorld[0]) / chunkSize));
	const int rows = static_cast<int>(std::lround((cfg::kValleyWorld[3] - cfg::kValleyWorld[1]) / chunkSize));
	for (int rz = 0; rz < rows; rz += region)
	{
		for (int rx = 0; rx < columns; rx += region)
		{
			MeshData ground, waterMesh, shape;
			for (int cz = rz; cz < std::min(rz + region, rows); ++cz)
			{
				for (int cx = rx; cx < std::min(rx + region, columns); ++cx)
				{
					const ChunkJob& job = jobs[static_cast<size_t>(cz) * columns + cx];
					AppendMesh(ground, job.ground);
					AppendMesh(waterMesh, job.water);
					AppendMesh(shape, job.shape);
				}
			}
			addMesh(ground, blockMaterial);
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
		mItems.push_back({ mesh, blockMaterial, transform });
		if (physics && !tree.distant) mBodies.push_back(physics->CreateStaticBox({ x, y + 1.6f, z }, { 0.25f, 1.6f, 0.25f }));   // the trunk
		++trees;
	}

	Spawn(scene);
	mBuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	CHERRY_LOG("UNDERTIME valley: %zu terrain triangles, %d trees, %s collision, built in %.0f ms on %u threads",
		triangles, trees, physics ? "with" : "no", mBuildMs, threadCount);
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
