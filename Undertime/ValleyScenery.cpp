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
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace DirectX;

namespace
{
	using SteadyClock = std::chrono::steady_clock;
	float MsSince(SteadyClock::time_point start) { return std::chrono::duration<float, std::milli>(SteadyClock::now() - start).count(); }

	// Levels of detail: 0.25 m with ground cover, 0.25 m, 0.5 m, 1 m.
	constexpr int kLevels = 4;
	constexpr float kLodBlock[kLevels] = { 0.25f, 0.25f, 0.5f, 1.0f };

	constexpr float kSkirt = 4.0f;   // m the sides along a region's edge reach down (hides cracks between sizes)
	enum CoverKind { CoverTuft, CoverTuftTall, CoverYellow, CoverWhite, CoverPurple, CoverBush, CoverPebbles, CoverKinds };
	// Collision models: the three small outcrops, then the three crags.
	constexpr int kCollisionModels = 6;

	float CoverHash(int x, int z, uint32_t salt)
	{
		uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u + salt * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
	}

	// The model at 1 / `factor` of its resolution, for far away: each factor^3 cell becomes
	// one voxel if at least `fill` of it is solid, in its most common colour.
	VoxelModel Downsample(const VoxelModel& model, int factor, float fill)
	{
		VoxelModel out;
		out.palette = model.palette;
		out.materials = model.materials;
		out.Resize({ (model.size.x + factor - 1) / factor, (model.size.y + factor - 1) / factor, (model.size.z + factor - 1) / factor });
		const int need = std::max(1, static_cast<int>(std::ceil(fill * factor * factor * factor)));
		std::vector<uint8_t> colours;
		for (int z = 0; z < static_cast<int>(out.size.z); ++z)
			for (int y = 0; y < static_cast<int>(out.size.y); ++y)
				for (int x = 0; x < static_cast<int>(out.size.x); ++x)
				{
					colours.clear();
					for (int dz = 0; dz < factor; ++dz)
						for (int dy = 0; dy < factor; ++dy)
							for (int dx = 0; dx < factor; ++dx)
								if (const uint8_t v = model.At(x * factor + dx, y * factor + dy, z * factor + dz)) colours.push_back(v);
					if (static_cast<int>(colours.size()) < need) continue;
					uint8_t best = colours[0];
					int bestCount = 0;
					for (uint8_t c : colours)
					{
						const int count = static_cast<int>(std::count(colours.begin(), colours.end(), c));
						if (count > bestCount) { best = c; bestCount = count; }
					}
					out.Set(x, y, z, best);
				}
		return out;
	}

	// A box's 12 triangles (outward-facing), placed by `world`, for a collision mesh.
	void AddBoxTriangles(std::vector<XMFLOAT3>& out, const XMFLOAT3& centre, const XMFLOAT3& half, const XMMATRIX& world)
	{
		const float c[3] = { centre.x, centre.y, centre.z }, h[3] = { half.x, half.y, half.z };
		// For each face: the axis it faces along, its sign, and two edges whose cross product points out.
		struct Face { int axis; float sign; int e1, e2; };
		static const Face kFaces[6] = { { 0, 1, 1, 2 }, { 0, -1, 2, 1 }, { 1, 1, 2, 0 }, { 1, -1, 0, 2 }, { 2, 1, 0, 1 }, { 2, -1, 1, 0 } };
		for (const Face& f : kFaces)
		{
			float o[3] = { c[0] - h[0], c[1] - h[1], c[2] - h[2] };
			o[f.axis] = c[f.axis] + f.sign * h[f.axis];
			float a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
			a[f.e1] = 2.0f * h[f.e1];
			b[f.e2] = 2.0f * h[f.e2];
			XMFLOAT3 q[4] = { { o[0], o[1], o[2] }, { o[0] + a[0], o[1] + a[1], o[2] + a[2] },
				{ o[0] + a[0] + b[0], o[1] + a[1] + b[1], o[2] + a[2] + b[2] }, { o[0] + b[0], o[1] + b[1], o[2] + b[2] } };
			for (XMFLOAT3& p : q) XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&p), world));
			out.insert(out.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
		}
	}

	// What the worker threads read (never changed after Build).
	struct Shared
	{
		const Valley* valley = nullptr;
		std::array<XMFLOAT3, static_cast<int>(Ground::Count)> colours{};
		MeshData cover[CoverKinds];
		float coverWidth[CoverKinds]{};
		Perlin3 flowerNoise{ 977 };
		int columns = 0, rows = 0;   // terrain chunks across the world
		// Collision besides the ground, per chunk: tree trunks, and models (outcrops, crags).
		struct Placed { int model; XMFLOAT4X4 world; };
		std::vector<std::vector<XMFLOAT3>> trunks;
		std::vector<std::vector<Placed>> placed;
		std::vector<PhysicsBox> modelBoxes[kCollisionModels];

		float ChunkX(int cx) const { return cfg::kValleyWorld[0] + cx * cfg::kTerrainChunk; }
		float ChunkZ(int cz) const { return cfg::kValleyWorld[1] + cz * cfg::kTerrainChunk; }
		int ChunkOf(float x, float z) const
		{
			const int cx = std::clamp(static_cast<int>(std::floor((x - cfg::kValleyWorld[0]) / cfg::kTerrainChunk)), 0, columns - 1);
			const int cz = std::clamp(static_cast<int>(std::floor((z - cfg::kValleyWorld[1]) / cfg::kTerrainChunk)), 0, rows - 1);
			return cz * columns + cx;
		}
	};

	// Ground cover (tall grass, flowers, bushes, pebbles) over a 0.25 m patch, standing on its columns.
	void ScatterCover(const Shared& sh, const TerrainPatch& patch, MeshData& out)
	{
		const Valley& valley = *sh.valley;
		const std::vector<GroundPad>& pads = valley.Pads();
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
				const float flowers = 0.5f + 0.5f * sh.flowerNoise.Fbm(x / 18.0f, 0.5f, z / 18.0f, 2);
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
				if (kind < 0 || sh.cover[kind].vertices.empty()) continue;
				// On flat ground only (not hanging over a step), on the block grid.
				const float w = sh.coverWidth[kind];
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
				for (Vertex v : sh.cover[kind].vertices)
				{
					float px = v.position.x - c, pz = v.position.z - c, nx = v.normal.x, nz = v.normal.z;
					for (int t = 0; t < turn; ++t) { std::swap(px, pz); pz = -pz; std::swap(nx, nz); nz = -nz; }
					v.position = { x0 + c + px, top + v.position.y, z0 + c + pz };
					v.normal = { nx, v.normal.y, nz };
					out.vertices.push_back(v);
				}
				for (uint32_t index : sh.cover[kind].indices) out.indices.push_back(base + index);
			}
	}

	struct RegionResult
	{
		int region = 0, level = 0;
		MeshData ground, water, cover;
		float ms = 0.0f;
	};

	// One region's terrain at a level of detail: its chunks (the river's always at 0.25 m),
	// with skirts along the region's edges and between chunks of different sizes.
	RegionResult BuildRegion(const Shared& sh, int region, int rx, int rz, int level)
	{
		const auto start = SteadyClock::now();
		RegionResult result;
		result.region = region;
		result.level = level;
		const int r = cfg::kTerrainRegion;
		const float c = cfg::kTerrainChunk;
		auto chunkLevel = [&](int cx, int cz) { return sh.valley->RiverChunk(sh.ChunkX(cx) + 0.5f * c, sh.ChunkZ(cz) + 0.5f * c) ? 0 : level; };
		for (int cz = rz * r; cz < std::min((rz + 1) * r, sh.rows); ++cz)
			for (int cx = rx * r; cx < std::min((rx + 1) * r, sh.columns); ++cx)
			{
				const int own = chunkLevel(cx, cz);
				const float block = kLodBlock[own];
				auto sameSize = [&](int a, int b) { return kLodBlock[a] == kLodBlock[b]; };
				std::array<float, 4> skirts{};
				const int ncx[4] = { cx - 1, cx + 1, cx, cx }, ncz[4] = { cz, cz, cz - 1, cz + 1 };
				for (int d = 0; d < 4; ++d)
				{
					if (ncx[d] < 0 || ncz[d] < 0 || ncx[d] >= sh.columns || ncz[d] >= sh.rows) continue;
					const bool otherRegion = ncx[d] / r != rx || ncz[d] / r != rz;
					skirts[d] = otherRegion || !sameSize(chunkLevel(ncx[d], ncz[d]), own) ? kSkirt : 0.0f;
				}
				const TerrainPatch patch = SampleTerrain(*sh.valley, sh.ChunkX(cx), sh.ChunkZ(cz), block, static_cast<int>(std::lround(c / block)));
				AppendMesh(result.ground, BuildTerrainMesh(patch, sh.colours, skirts));
				AppendMesh(result.water, BuildWaterMesh(patch, { 1, 1, 1 }));
				if (level == 0 && own == 0) ScatterCover(sh, patch, result.cover);
			}
		result.ms = MsSince(start);
		return result;
	}

	struct CollisionResult
	{
		int chunk = 0;
		std::vector<XMFLOAT3> triangles;
		float ms = 0.0f;
	};

	// One chunk's collision: its ground in 0.25 m blocks, tree trunks and outcrops.
	CollisionResult BuildCollision(const Shared& sh, int chunk)
	{
		const auto start = SteadyClock::now();
		CollisionResult result;
		result.chunk = chunk;
		const int cx = chunk % sh.columns, cz = chunk / sh.columns;
		const float block = cfg::kVoxelSize;
		const TerrainPatch patch = SampleTerrain(*sh.valley, sh.ChunkX(cx), sh.ChunkZ(cz), block, static_cast<int>(std::lround(cfg::kTerrainChunk / block)));
		result.triangles = MeshTriangles(BuildTerrainMesh(patch, sh.colours, { 0, 0, 0, 0 }, true));
		for (const XMFLOAT3& trunk : sh.trunks[chunk])
			AddBoxTriangles(result.triangles, { trunk.x, trunk.y + 1.6f, trunk.z }, { 0.25f, 1.6f, 0.25f }, XMMatrixIdentity());
		for (const Shared::Placed& placed : sh.placed[chunk])
		{
			const XMMATRIX world = XMLoadFloat4x4(&placed.world);
			for (const PhysicsBox& box : sh.modelBoxes[placed.model]) AddBoxTriangles(result.triangles, box.center, box.halfExtents, world);
		}
		result.ms = MsSince(start);
		return result;
	}

	int LevelFor(float distance, const float (&limits)[3])
	{
		for (int level = 0; level < kLevels - 1; ++level)
			if (distance < limits[level]) return level;
		return kLevels - 1;
	}

	// A crossfade's progress: `to` drawn on t of the pixels, `from` on the rest.
	struct Fade { float t = -1.0f; };   // < 0: none

	// Horizontal distance from a point to a rectangle (0 inside), plus the height above the valley floor.
	float DistanceTo(const XMFLOAT3& p, float x0, float z0, float x1, float z1)
	{
		const float dx = std::max({ x0 - p.x, 0.0f, p.x - x1 }), dz = std::max({ z0 - p.z, 0.0f, p.z - z1 });
		const float dy = std::max(0.0f, p.y - cfg::kGroundY - 30.0f);
		return std::sqrt(dx * dx + dz * dz + dy * dy);
	}
}

struct ValleyScenery::Lod
{
	Shared shared;
	MaterialHandle ground, water, plants;

	struct Region
	{
		int rx = 0, rz = 0;
		float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
		int level = -1;       // drawn at
		int requested = -1;   // being rebuilt at (-1: none)
		bool stale = false;   // to be rebuilt at its level (the mesh format changed)
		size_t item = 0;      // its items: ground, water, cover, then the same fading out
		MeshHandle meshes[3], previous[3];   // previous: the level fading out
		size_t triangles[3]{};
		size_t bytes = 0, previousBytes = 0;
		float fade = -1.0f;   // crossfade progress 0..1 (< 0: none)
	};
	std::vector<Region> regions;
	std::vector<RegionResult> ready;   // built, waiting to be swapped in

	// Trees and crags: a near and a far mesh, swapped by distance.
	struct Model
	{
		size_t nearItem = 0, farItem = SIZE_MAX;   // SIZE_MAX: no far version
		float x = 0, z = 0;
		bool isNear = true;
		float fade = -1.0f;   // crossfade progress 0..1 (< 0: none)
	};
	std::vector<Model> models;

	struct CollisionChunk { BodyId body = kInvalidBody; };
	std::unordered_map<int, CollisionChunk> collision;   // chunks wanted (a body once built)
	std::vector<CollisionResult> collisionReady;

	// Worker threads.
	std::vector<std::thread> workers;
	std::mutex mutex;
	std::condition_variable wake, idle;
	std::deque<std::function<void()>> jobs;
	int running = 0;
	bool stop = false;
	std::vector<RegionResult> regionsDone;           // guarded by mutex
	std::vector<CollisionResult> collisionDone;      // guarded by mutex
	int regionsInFlight = 0;

	Stats stats;

	void Start()
	{
		const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
		for (unsigned t = 0; t < count; ++t)
			workers.emplace_back([this]()
			{
				std::unique_lock lock(mutex);
				for (;;)
				{
					wake.wait(lock, [&]() { return stop || !jobs.empty(); });
					if (stop) return;
					std::function<void()> job = std::move(jobs.front());
					jobs.pop_front();
					++running;
					lock.unlock();
					job();
					lock.lock();
					--running;
					idle.notify_all();
				}
			});
	}
	void Stop()
	{
		{
			std::lock_guard lock(mutex);
			stop = true;
			jobs.clear();
		}
		wake.notify_all();
		for (std::thread& worker : workers) worker.join();
		workers.clear();
	}
	void Submit(std::function<void()> job)
	{
		{
			std::lock_guard lock(mutex);
			jobs.push_back(std::move(job));
		}
		wake.notify_one();
	}
	void WaitIdle()
	{
		std::unique_lock lock(mutex);
		idle.wait(lock, [&]() { return jobs.empty() && running == 0; });
	}
	void SubmitRegion(int index, int level)
	{
		Region& region = regions[index];
		region.requested = level;
		++regionsInFlight;
		const int rx = region.rx, rz = region.rz;
		Submit([this, index, rx, rz, level]()
		{
			RegionResult result = BuildRegion(shared, index, rx, rz, level);
			std::lock_guard lock(mutex);
			regionsDone.push_back(std::move(result));
		});
	}
	void SubmitCollision(int chunk)
	{
		Submit([this, chunk]()
		{
			CollisionResult result = BuildCollision(shared, chunk);
			std::lock_guard lock(mutex);
			collisionDone.push_back(std::move(result));
		});
	}
	// Moves finished work into `ready` / `collisionReady`.
	void Collect()
	{
		std::lock_guard lock(mutex);
		for (RegionResult& result : regionsDone)
		{
			--regionsInFlight;
			stats.regionMs = result.ms;
			if (regions[result.region].requested == result.level) ready.push_back(std::move(result));
		}
		regionsDone.clear();
		for (CollisionResult& result : collisionDone)
		{
			stats.collisionMs = result.ms;
			collisionReady.push_back(std::move(result));
		}
		collisionDone.clear();
	}
	// The collision chunks within `radius` of `eye`.
	template <class F> void ChunksAround(const XMFLOAT3& eye, float radius, F&& each) const
	{
		const float c = cfg::kTerrainChunk;
		const int cx0 = std::max(0, static_cast<int>(std::floor((eye.x - radius - cfg::kValleyWorld[0]) / c)));
		const int cx1 = std::min(shared.columns - 1, static_cast<int>(std::floor((eye.x + radius - cfg::kValleyWorld[0]) / c)));
		const int cz0 = std::max(0, static_cast<int>(std::floor((eye.z - radius - cfg::kValleyWorld[1]) / c)));
		const int cz1 = std::min(shared.rows - 1, static_cast<int>(std::floor((eye.z + radius - cfg::kValleyWorld[1]) / c)));
		for (int cz = cz0; cz <= cz1; ++cz)
			for (int cx = cx0; cx <= cx1; ++cx)
			{
				const float x0 = shared.ChunkX(cx), z0 = shared.ChunkZ(cz);
				const float dx = std::max({ x0 - eye.x, 0.0f, eye.x - x0 - c }), dz = std::max({ z0 - eye.z, 0.0f, eye.z - z0 - c });
				if (dx * dx + dz * dz <= radius * radius) each(cz * shared.columns + cx);
			}
	}
};

ValleyScenery::ValleyScenery() = default;

ValleyScenery::~ValleyScenery()
{
	if (mLod) mLod->Stop();
}

void ValleyScenery::Build(Engine& engine, Scene& scene, const Valley& valley, PhysicsWorld* physics, const XMFLOAT3& focus)
{
	Destroy(engine, scene, physics);
	const auto start = SteadyClock::now();
	Renderer& renderer = engine.GetRenderer();
	AssetManager& assets = engine.GetAssets();
	mPhysics = physics;
	mLod = std::make_unique<Lod>();
	Lod& lod = *mLod;
	Shared& sh = lod.shared;
	sh.valley = &valley;
	// Development: UNDERTIME_DEV_LOD="a b c" overrides cfg::kLodDistance (e.g. "110 9999 9999":
	// everything at 0.25 m, ground cover only near).
	char* devLod = nullptr;
	size_t devLodLength = 0;
	if (_dupenv_s(&devLod, &devLodLength, "UNDERTIME_DEV_LOD") == 0 && devLod)
	{
		sscanf_s(devLod, "%f %f %f", &mSettings.lodDistance[0], &mSettings.lodDistance[1], &mSettings.lodDistance[2]);
		std::free(devLod);
		CHERRY_LOG("UNDERTIME valley: LOD distances %.0f %.0f %.0f (UNDERTIME_DEV_LOD)", mSettings.lodDistance[0], mSettings.lodDistance[1], mSettings.lodDistance[2]);
	}
	// UNDERTIME_DEV_VALLEY="fade compact [fadeSeconds]" (0 or 1 each) switches those.
	char* devValley = nullptr;
	size_t devValleyLength = 0;
	if (_dupenv_s(&devValley, &devValleyLength, "UNDERTIME_DEV_VALLEY") == 0 && devValley)
	{
		int fade = mSettings.fade, compact = mSettings.compact;
		sscanf_s(devValley, "%d %d %f", &fade, &compact, &mSettings.fadeSeconds);
		std::free(devValley);
		mSettings.fade = fade != 0;
		mSettings.compact = compact != 0;
		CHERRY_LOG("UNDERTIME valley: fade %s, compact %s (UNDERTIME_DEV_VALLEY)", mSettings.fade ? "on" : "off", mSettings.compact ? "on" : "off");
	}
	sh.columns = static_cast<int>(std::lround((cfg::kValleyWorld[2] - cfg::kValleyWorld[0]) / cfg::kTerrainChunk));
	sh.rows = static_cast<int>(std::lround((cfg::kValleyWorld[3] - cfg::kValleyWorld[1]) / cfg::kTerrainChunk));
	sh.trunks.resize(static_cast<size_t>(sh.columns) * sh.rows);
	sh.placed.resize(sh.trunks.size());

	const MaterialHandle blockMaterial = assets.GetMaterial("engine/VoxelModel");
	// The terrain gets wet after rain (Shaders/Ground.hlsl).
	const MaterialHandle wetGround = assets.GetMaterial("Materials/Ground.mat");
	lod.ground = wetGround.IsValid() ? wetGround : blockMaterial;
	sh.colours[static_cast<int>(Ground::Grass)] = color::Linear(cfg::kTerrainGrass);
	sh.colours[static_cast<int>(Ground::Grass2)] = color::Linear(cfg::kTerrainGrass2);
	sh.colours[static_cast<int>(Ground::Dirt)] = color::Linear(cfg::kTerrainDirt);
	sh.colours[static_cast<int>(Ground::Sand)] = color::Linear(cfg::kTerrainSand);
	sh.colours[static_cast<int>(Ground::Gravel)] = color::Linear(cfg::kTerrainGravel);
	sh.colours[static_cast<int>(Ground::Rock)] = color::Linear(cfg::kTerrainRock);
	sh.colours[static_cast<int>(Ground::Rock2)] = color::Linear(cfg::kTerrainRock2);
	sh.colours[static_cast<int>(Ground::Snow)] = color::Linear(cfg::kTerrainSnow);
	sh.colours[static_cast<int>(Ground::Path)] = color::Linear(cfg::kTerrainPath);
	sh.colours[static_cast<int>(Ground::Moss)] = color::Linear(cfg::kTerrainMoss);

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
		lod.water = mWaterMaterial;
		mSprays = FallSprays(valley);
	}

	// Ground cover models (tall grass, flowers, bushes, pebbles), baked once with their corner
	// at the origin; each vertex's alpha carries how far it sways (Shaders/Foliage.hlsl:
	// 0.99 at the foot, 0.01 at the tip).
	const MaterialHandle foliageMaterial = assets.GetMaterial("Materials/Foliage.mat");
	lod.plants = foliageMaterial.IsValid() ? foliageMaterial : blockMaterial;
	const char* const kCoverModels[CoverKinds] = {
		"Models/Placeholders/GrassTuft.vox", "Models/Placeholders/GrassTuftTall.vox", "Models/Placeholders/FlowersYellow.vox",
		"Models/Placeholders/FlowersWhite.vox", "Models/Placeholders/FlowersPurple.vox", "Models/Placeholders/Bush.vox", "Models/Placeholders/Pebbles.vox" };
	for (int i = 0; i < CoverKinds; ++i)
	{
		VoxelModel model;
		if (!assets.ReadVoxelModel(kCoverModels[i], model)) continue;
		VoxelBakeOptions options;
		options.pivot = VoxelPivot::Corner;
		sh.cover[i] = BakeVoxelMesh(model, options);
		const float height = std::max(model.size.y * cfg::kVoxelSize, 0.25f);
		const bool sways = i != CoverPebbles;
		for (Vertex& v : sh.cover[i].vertices) v.color.w = sways ? 0.99f - 0.98f * std::clamp(v.position.y / height, 0.0f, 1.0f) : 0.99f;
		sh.coverWidth[i] = model.size.x * cfg::kVoxelSize;
	}

	// Terrain regions: three items each (ground, water, cover), meshes swapped in as built.
	const int region = cfg::kTerrainRegion;
	for (int rz = 0; rz * region < sh.rows; ++rz)
		for (int rx = 0; rx * region < sh.columns; ++rx)
		{
			Lod::Region r;
			r.rx = rx;
			r.rz = rz;
			r.x0 = sh.ChunkX(rx * region);
			r.z0 = sh.ChunkZ(rz * region);
			r.x1 = sh.ChunkX(std::min((rx + 1) * region, sh.columns));
			r.z1 = sh.ChunkZ(std::min((rz + 1) * region, sh.rows));
			r.item = mItems.size();
			for (int set = 0; set < 2; ++set)
			{
				mItems.push_back({ MeshHandle{}, lod.ground, Transform{} });
				mItems.push_back({ MeshHandle{}, mWaterMaterial, Transform{} });
				mItems.push_back({ MeshHandle{}, lod.plants, Transform{} });
			}
			lod.regions.push_back(r);
		}

	// A model's far version: halved (0.5 m blocks), baked like the original.
	auto farMesh = [&](const char* path, float fill) -> MeshHandle
	{
		VoxelModel model;
		if (!assets.ReadVoxelModel(path, model)) return {};
		VoxelBakeOptions options;
		options.voxelSize = cfg::kVoxelSize * 2.0f;
		const MeshHandle mesh = renderer.CreateMesh(BakeVoxelMesh(Downsample(model, 2, fill), options));
		mOwnedMeshes.push_back(mesh);
		return mesh;
	};
	auto addModel = [&](MeshHandle nearMesh, MeshHandle farVersion, MaterialHandle material, const Transform& transform)
	{
		Lod::Model model;
		model.x = transform.position.x;
		model.z = transform.position.z;
		model.isNear = !farVersion.IsValid() || DistanceTo(focus, model.x, model.z, model.x, model.z) < cfg::kModelLodDistance;
		model.nearItem = mItems.size();
		mItems.push_back({ nearMesh, material, transform });
		mItems.back().shown = model.isNear;
		if (farVersion.IsValid())
		{
			model.farItem = mItems.size();
			mItems.push_back({ farVersion, material, transform });
			mItems.back().shown = !model.isNear;
			lod.models.push_back(model);
		}
	};

	// The forest: placeholder voxel models (Content/Models/Placeholders, editable in
	// MagicaVoxel), standing on whole blocks and turned in quarter turns so they line up
	// with the ground; a halved version far away. Every trunk blocks you.
	const char* const kTreeModels[] = {
		"Models/Placeholders/ConiferSmall.vox", "Models/Placeholders/ConiferMedium.vox",
		"Models/Placeholders/ConiferLarge.vox", "Models/Placeholders/Broadleaf.vox" };
	MeshHandle treeNear[4], treeFar[4];
	for (int i = 0; i < 4; ++i)
	{
		treeNear[i] = assets.GetMesh(kTreeModels[i]);
		treeFar[i] = farMesh(kTreeModels[i], 0.25f);   // a quarter full keeps the trunk
	}
	int trees = 0;
	for (const TreeSpot& tree : valley.Trees())
	{
		if (!treeNear[tree.kind].IsValid()) continue;
		const float block = cfg::kVoxelSize;
		const float x = std::floor(tree.x / block) * block, z = std::floor(tree.z / block) * block;
		const float y = std::round(valley.Height(x + 0.5f * block, z + 0.5f * block) / block) * block;
		Transform transform;
		transform.position = { x, y, z };
		transform.SetRotationEuler(0.0f, std::floor(tree.yaw / XM_PIDIV2) * XM_PIDIV2, 0.0f);
		addModel(treeNear[tree.kind], treeFar[tree.kind], lod.plants, transform);
		sh.trunks[sh.ChunkOf(x, z)].push_back({ x, y, z });
		++trees;
	}

	// Rock outcrops on steep ground, sunk into the slope (standing on its lowest point under
	// them): small ones on the valley floor, crags four times the size on the mountains
	// (Crag*.vox, with a halved version far away). Both block you.
	const char* const kOutcropModels[] = { "Models/Placeholders/OutcropSmall.vox", "Models/Placeholders/OutcropMedium.vox", "Models/Placeholders/OutcropLarge.vox" };
	const char* const kCragModels[] = { "Models/Placeholders/CragSmall.vox", "Models/Placeholders/CragMedium.vox", "Models/Placeholders/CragLarge.vox" };
	MeshHandle outcropNear[3], cragNear[3], cragFar[3];
	XMFLOAT3 modelSize[kCollisionModels]{};
	for (int i = 0; i < 3; ++i)
	{
		const char* paths[2] = { kOutcropModels[i], kCragModels[i] };
		for (int crag = 0; crag < 2; ++crag)
		{
			VoxelModel model;
			if (!assets.ReadVoxelModel(paths[crag], model)) continue;
			sh.modelBoxes[i + 3 * crag] = BakeVoxelBoxes(model);
			modelSize[i + 3 * crag] = { model.size.x * cfg::kVoxelSize, model.size.y * cfg::kVoxelSize, model.size.z * cfg::kVoxelSize };
		}
		outcropNear[i] = assets.GetMesh(kOutcropModels[i]);
		cragNear[i] = assets.GetMesh(kCragModels[i]);
		cragFar[i] = farMesh(kCragModels[i], 0.5f);
		if (!cragNear[i].IsValid()) CHERRY_WARN("UNDERTIME valley: %s is missing (run UndertimePlaceholders)", kCragModels[i]);
	}
	int outcrops = 0;
	for (const OutcropSpot& spot : valley.Outcrops())
	{
		const int model = spot.kind + (spot.distant ? 3 : 0);
		const MeshHandle mesh = spot.distant ? cragNear[spot.kind] : outcropNear[spot.kind];
		if (!mesh.IsValid()) continue;
		const float half = 0.5f * std::max(modelSize[model].x, modelSize[model].z);
		float ground = 1e9f;
		for (int a = 0; a < 4; ++a)
			ground = std::min(ground, valley.Height(spot.x + half * 0.7f * ((a & 1) ? 1.0f : -1.0f), spot.z + half * 0.7f * ((a & 2) ? 1.0f : -1.0f)));
		const float block = cfg::kVoxelSize, sink = spot.distant ? 1.0f : block;
		Transform transform;
		transform.position = { std::floor(spot.x / block) * block, std::floor(ground / block) * block - sink, std::floor(spot.z / block) * block };
		transform.SetRotationEuler(0.0f, std::floor(spot.yaw / XM_PIDIV2) * XM_PIDIV2, 0.0f);
		addModel(mesh, spot.distant ? cragFar[spot.kind] : MeshHandle{}, blockMaterial, transform);
		Shared::Placed placed{ model, {} };
		XMStoreFloat4x4(&placed.world, transform.Matrix());
		sh.placed[sh.ChunkOf(transform.position.x, transform.position.z)].push_back(placed);
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

	// The first level of detail and collision around the focus, all at once.
	lod.Start();
	for (size_t i = 0; i < lod.regions.size(); ++i)
	{
		const Lod::Region& r = lod.regions[i];
		lod.SubmitRegion(static_cast<int>(i), LevelFor(DistanceTo(focus, r.x0, r.z0, r.x1, r.z1), mSettings.lodDistance));
	}
	if (physics)
		lod.ChunksAround(focus, cfg::kCollisionRadius, [&](int chunk) { lod.collision[chunk] = {}; lod.SubmitCollision(chunk); });
	lod.WaitIdle();
	Update(engine, scene, focus);   // (while mBuildMs is 0, it swaps in everything at once)

	mBuildMs = MsSince(start);
	CHERRY_LOG("UNDERTIME valley: %zu regions (%d at 0.25 m with ground cover, %d at 0.25 m, %d at 0.5 m, %d at 1 m), %zu terrain and water triangles, %zu ground cover triangles, %d trees, %d outcrops, %s collision (%zu chunks), built in %.0f ms on %zu threads",
		lod.regions.size(), lod.stats.regions[0], lod.stats.regions[1], lod.stats.regions[2], lod.stats.regions[3], lod.stats.triangles, lod.stats.coverTriangles,
		trees, outcrops, physics ? "with" : "no", lod.collision.size(), mBuildMs, lod.workers.size());
}

void ValleyScenery::Update(Engine& engine, Scene& scene, const XMFLOAT3& eye)
{
	if (!mLod) return;
	Lod& lod = *mLod;
	Renderer& renderer = engine.GetRenderer();
	lod.Collect();

	const bool building = mBuildMs == 0.0f;
	const float fadeStep = mSettings.fadeSeconds > 0.0f ? engine.DeltaTime() / mSettings.fadeSeconds : 1.0f;
	// A region's crossfade: the new level on t of the pixels, the old one on the rest; at the
	// end the old meshes go.
	auto setRegionFade = [&](Lod::Region& r, float t)
	{
		for (int k = 0; k < 3; ++k)
		{
			Item& now = mItems[r.item + k];
			Item& old = mItems[r.item + 3 + k];
			now.dither = t >= 1.0f ? 1.0f : t;
			now.invert = false;
			old.dither = 1.0f - t;
			old.invert = true;
			old.shown = t < 1.0f;
			if (t >= 1.0f)
			{
				if (r.previous[k].IsValid()) renderer.DestroyMesh(r.previous[k]);
				r.previous[k] = {};
				old.mesh = {};
			}
			ApplyItem(scene, r.item + k);
			ApplyItem(scene, r.item + 3 + k);
		}
		if (t >= 1.0f)
		{
			r.fade = -1.0f;
			r.previousBytes = 0;
		}
	};
	for (Lod::Region& r : lod.regions)
	{
		if (r.fade < 0.0f) continue;
		r.fade = mSettings.fade ? std::min(1.0f, r.fade + fadeStep) : 1.0f;
		setRegionFade(r, r.fade);
	}

	// Swap in rebuilt regions (a few a frame: each makes up to three meshes).
	int swaps = 0;
	const MeshFormat format = mSettings.compact ? MeshFormat::Compact : MeshFormat::Full;
	while (!lod.ready.empty() && (building || swaps < 3))
	{
		RegionResult result = std::move(lod.ready.back());
		lod.ready.pop_back();
		Lod::Region& r = lod.regions[result.region];
		if (r.requested != result.level) continue;
		if (r.fade >= 0.0f) setRegionFade(r, 1.0f);   // a fade still running: finish it
		const bool fade = mSettings.fade && !building && r.level >= 0;
		const MeshData* parts[3] = { &result.ground, &result.water, &result.cover };
		r.previousBytes = 0;
		r.bytes = 0;
		for (int k = 0; k < 3; ++k)
		{
			const MeshHandle old = r.meshes[k];
			r.meshes[k] = parts[k]->indices.empty() ? MeshHandle{} : renderer.CreateMesh(*parts[k], format);
			r.triangles[k] = parts[k]->indices.size() / 3;
			r.bytes += renderer.GetMeshBytes(r.meshes[k]);
			mItems[r.item + k].mesh = r.meshes[k];
			if (fade)
			{
				r.previous[k] = old;
				r.previousBytes += renderer.GetMeshBytes(old);
				mItems[r.item + 3 + k].mesh = old;
			}
			else if (old.IsValid())
				renderer.DestroyMesh(old);
			ApplyItem(scene, r.item + k);
		}
		if (fade)
		{
			r.fade = 0.0f;
			setRegionFade(r, 0.0f);
		}
		r.level = result.level;
		r.requested = -1;
		++swaps;
	}
	if (mRebuildAll)
	{
		mRebuildAll = false;
		for (Lod::Region& r : lod.regions) r.stale = r.level >= 0;
	}

	// Which regions want another level, nearest first.
	struct Want { int region, level; float distance; };
	std::vector<Want> wants;
	for (size_t i = 0; i < lod.regions.size(); ++i)
	{
		const Lod::Region& r = lod.regions[i];
		const float distance = DistanceTo(eye, r.x0, r.z0, r.x1, r.z1);
		const int current = r.requested >= 0 ? r.requested : r.level;
		const float (&limits)[3] = mSettings.lodDistance;
		int level = LevelFor(distance, limits);
		// Turning coarser waits until the region is a little further off (no flapping).
		if (current >= 0 && level > current && LevelFor(distance - cfg::kLodHysteresis, limits) <= current) level = current;
		if (level != current || r.stale) wants.push_back({ static_cast<int>(i), level, distance });
	}
	std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.level != b.level ? a.level < b.level : a.distance < b.distance; });
	for (const Want& want : wants)
	{
		if (lod.regionsInFlight >= cfg::kLodBuildsInFlight) break;
		lod.regions[want.region].stale = false;
		lod.SubmitRegion(want.region, want.level);
	}

	// Trees and crags: near or far versions, crossfading.
	int modelsFading = 0;
	for (Lod::Model& model : lod.models)
	{
		const float dx = model.x - eye.x, dz = model.z - eye.z;
		const float distance = std::sqrt(dx * dx + dz * dz);
		const bool isNear = model.isNear ? distance < cfg::kModelLodDistance + 8.0f : distance < cfg::kModelLodDistance;
		if (isNear != model.isNear)
		{
			model.isNear = isNear;
			model.fade = mSettings.fade && !building ? 0.0f : 1.0f;
		}
		if (model.fade < 0.0f) continue;
		model.fade = model.fade >= 1.0f ? 1.0f : std::min(1.0f, model.fade + fadeStep);
		Item& to = mItems[model.isNear ? model.nearItem : model.farItem];
		Item& from = mItems[model.isNear ? model.farItem : model.nearItem];
		to.shown = true;
		to.dither = model.fade;
		to.invert = false;
		from.shown = model.fade < 1.0f;
		from.dither = 1.0f - model.fade;
		from.invert = true;
		ApplyItem(scene, model.nearItem);
		ApplyItem(scene, model.farItem);
		if (model.fade >= 1.0f) model.fade = -1.0f;
		else ++modelsFading;
	}

	// Collision around the eye: wanted chunks are built on the workers; one close by that's
	// still missing (a teleport, the start) is built right now.
	if (mPhysics)
	{
		lod.ChunksAround(eye, cfg::kCollisionRadius, [&](int chunk)
		{
			if (lod.collision.count(chunk)) return;
			lod.collision[chunk] = {};
			lod.SubmitCollision(chunk);
		});
		lod.ChunksAround(eye, cfg::kCollisionNow, [&](int chunk)
		{
			auto it = lod.collision.find(chunk);
			if (it != lod.collision.end() && it->second.body != kInvalidBody) return;
			for (const CollisionResult& waiting : lod.collisionReady)
				if (waiting.chunk == chunk) return;
			lod.collision[chunk] = {};
			lod.collisionReady.push_back(BuildCollision(lod.shared, chunk));
		});
		const auto bodiesStart = SteadyClock::now();
		while (!lod.collisionReady.empty() && (building || MsSince(bodiesStart) < cfg::kCollisionBudgetMs))
		{
			// The ones closest to the eye first.
			auto nearest = lod.collisionReady.begin();
			float best = 1e30f;
			for (auto it = lod.collisionReady.begin(); it != lod.collisionReady.end(); ++it)
			{
				const int cx = it->chunk % lod.shared.columns, cz = it->chunk / lod.shared.columns;
				const float dx = lod.shared.ChunkX(cx) + 0.5f * cfg::kTerrainChunk - eye.x, dz = lod.shared.ChunkZ(cz) + 0.5f * cfg::kTerrainChunk - eye.z;
				if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; nearest = it; }
			}
			CollisionResult result = std::move(*nearest);
			lod.collisionReady.erase(nearest);
			auto it = lod.collision.find(result.chunk);
			if (it == lod.collision.end() || it->second.body != kInvalidBody) continue;   // dropped meanwhile, or built twice
			const auto bodyStart = SteadyClock::now();
			it->second.body = mPhysics->CreateStaticMesh(result.triangles);
			lod.stats.bodyMs = MsSince(bodyStart);
		}
		// Far behind: dropped.
		for (auto it = lod.collision.begin(); it != lod.collision.end(); )
		{
			const int cx = it->first % lod.shared.columns, cz = it->first / lod.shared.columns;
			const float x0 = lod.shared.ChunkX(cx), z0 = lod.shared.ChunkZ(cz), c = cfg::kTerrainChunk;
			const float dx = std::max({ x0 - eye.x, 0.0f, eye.x - x0 - c }), dz = std::max({ z0 - eye.z, 0.0f, eye.z - z0 - c });
			if (dx * dx + dz * dz <= cfg::kCollisionKeep * cfg::kCollisionKeep) { ++it; continue; }
			if (it->second.body != kInvalidBody) mPhysics->RemoveBody(it->second.body);
			it = lod.collision.erase(it);
		}
	}

	// Stats.
	Stats& stats = lod.stats;
	std::fill(std::begin(stats.regions), std::end(stats.regions), 0);
	stats.triangles = stats.coverTriangles = stats.meshBytes = 0;
	stats.rebuilding = 0;
	stats.fading = modelsFading;
	for (const Lod::Region& r : lod.regions)
	{
		if (r.level >= 0) ++stats.regions[r.level];
		if (r.requested >= 0) ++stats.rebuilding;
		if (r.fade >= 0.0f) ++stats.fading;
		stats.triangles += r.triangles[0] + r.triangles[1];
		stats.coverTriangles += r.triangles[2];
		stats.meshBytes += r.bytes + r.previousBytes;
	}
	stats.collisionChunks = 0;
	for (const auto& [chunk, entry] : lod.collision) stats.collisionChunks += entry.body != kInvalidBody;
}

ValleyScenery::Stats ValleyScenery::GetStats() const
{
	return mLod ? mLod->stats : Stats{};
}

void ValleyScenery::SetSettings(const Settings& settings)
{
	if (settings.compact != mSettings.compact) mRebuildAll = true;
	mSettings = settings;
}

void ValleyScenery::ApplyItem(Scene& scene, size_t index)
{
	if (index >= mItems.size() || index >= mEntities.size()) return;
	const Item& item = mItems[index];
	const Entity entity = mEntities[index];
	if (!scene.IsAlive(entity) || !scene.Has<MeshRenderer>(entity)) return;
	MeshRenderer& renderer = scene.Get<MeshRenderer>(entity);
	renderer.mesh = item.mesh;
	renderer.visible = mVisible && item.shown && item.mesh.IsValid();
	renderer.dither = item.dither;
	renderer.ditherInvert = item.invert;
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
		renderer.visible = mVisible && item.shown && item.mesh.IsValid();
		renderer.dither = item.dither;
		renderer.ditherInvert = item.invert;
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
	for (size_t i = 0; i < mEntities.size(); ++i)
	{
		const Entity entity = mEntities[i];
		if (!scene.IsAlive(entity)) continue;
		if (scene.Has<MeshRenderer>(entity)) scene.Get<MeshRenderer>(entity).visible = visible && (i >= mItems.size() || (mItems[i].shown && mItems[i].mesh.IsValid()));
		if (scene.Has<ParticleEmitter>(entity)) scene.Get<ParticleEmitter>(entity).enabled = visible;
	}
}

void ValleyScenery::Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics)
{
	Renderer& renderer = engine.GetRenderer();
	if (mLod)
	{
		mLod->Stop();
		for (const Lod::Region& r : mLod->regions)
			for (MeshHandle mesh : r.meshes)
				if (mesh.IsValid()) renderer.DestroyMesh(mesh);
		for (const Lod::Region& r : mLod->regions)
			for (MeshHandle mesh : r.previous)
				if (mesh.IsValid()) renderer.DestroyMesh(mesh);
		if (physics)
			for (const auto& [chunk, entry] : mLod->collision)
				if (entry.body != kInvalidBody) physics->RemoveBody(entry.body);
		mLod.reset();
	}
	for (Entity entity : mEntities)
	{
		if (scene.IsAlive(entity)) scene.DestroyEntity(entity);
	}
	mEntities.clear();
	if (scene.IsAlive(mRoot)) scene.DestroyEntity(mRoot);
	mRoot = INVALID_ENTITY;
	mItems.clear();
	for (MeshHandle mesh : mOwnedMeshes) renderer.DestroyMesh(mesh);
	mOwnedMeshes.clear();
	if (physics) for (BodyId body : mBodies) physics->RemoveBody(body);
	mBodies.clear();
	mPhysics = nullptr;
	if (mWaterMaterial.IsValid()) renderer.DestroyMaterial(mWaterMaterial);
	if (mFlowMap.IsValid()) renderer.DestroyTexture(mFlowMap);
	mWaterMaterial = {};
	mFlowMap = {};
	mSprays.clear();
	mVisible = true;
	mBuildMs = 0.0f;
}
