#include "LooseBlocks.h"
#include "Meshes.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

using namespace DirectX;

std::vector<XMINT3> MirrorOutOfFace(const std::vector<VoxelWorld::RemovedVoxel>& voxels, int axis, int sign, int faceVoxel,
	const std::function<bool(int, int, int)>& isFree)
{
	// The face plane sits on the hit voxel's boundary, so mirroring keeps cells on the grid:
	// a voxel `d` cells behind the face lands `d` cells in front of it.
	const int plane = sign > 0 ? faceVoxel + 1 : faceVoxel;
	auto key = [](int x, int y, int z) { return (static_cast<uint64_t>(x + 4096) << 42) | (static_cast<uint64_t>(y + 4096) << 21) | static_cast<uint64_t>(z + 4096); };
	std::unordered_set<uint64_t> taken;
	std::vector<XMINT3> out;
	out.reserve(voxels.size());
	for (const VoxelWorld::RemovedVoxel& v : voxels)
	{
		int c[3] = { v.x, v.y, v.z };
		c[axis] = 2 * plane - 1 - c[axis];
		if (isFree(c[0], c[1], c[2]) && taken.insert(key(c[0], c[1], c[2])).second)
		{
			out.push_back({ c[0], c[1], c[2] });
		}
		else
		{
			taken.insert(key(v.x, v.y, v.z));
			out.push_back({ v.x, v.y, v.z });   // no room in front: falls out of its own cell
		}
	}
	return out;
}

// =============================================================================

void LooseBlocks::Initialize(MaterialHandle material)
{
	mMaterial = material;
}

MeshHandle LooseBlocks::MeshFor(Engine& engine, cfg::Block block, int x, int y, int z)
{
	// Colours quantised (in a perceptual-ish space) so blocks share a handful of meshes.
	const XMFLOAT4 linear = VoxelWorld::VoxelColor(block, x, y, z);
	auto q = [](float c) { return static_cast<uint32_t>(std::clamp(std::sqrt(std::max(c, 0.0f)), 0.0f, 1.0f) * 31.0f + 0.5f); };
	const uint32_t key = (q(linear.x) << 10) | (q(linear.y) << 5) | q(linear.z);
	auto it = mMeshes.find(key);
	if (it != mMeshes.end()) return it->second;

	auto back = [](uint32_t v) { const float c = v / 31.0f; return c * c; };
	const XMFLOAT4 colour{ back((key >> 10) & 31), back((key >> 5) & 31), back(key & 31), 1.0f };
	MeshData mesh;
	const float half = cfg::kVoxelSize * 0.5f * (1.0f - cfg::kBlockGap);
	meshes::AddBox(mesh, { 0, 0, 0 }, { half, half, half }, colour);
	const MeshHandle handle = engine.GetRenderer().CreateMesh(mesh);
	mMeshes.emplace(key, handle);
	return handle;
}

void LooseBlocks::Spawn(Engine& engine, cfg::Block block, int colorX, int colorY, int colorZ, const XMFLOAT3& center,
	const XMFLOAT4& rotation, const XMFLOAT3& velocity, const XMFLOAT3& spin)
{
	if (block == cfg::Block::Air) return;
	const float half = cfg::kVoxelSize * 0.5f * (1.0f - cfg::kBlockGap);
	const std::vector<PhysicsBox> box{ PhysicsBox{ { 0, 0, 0 }, { half, half, half } } };
	const BodyId body = engine.GetPhysics().CreateDynamicBoxes(box, cfg::kRockDensity, center, rotation, velocity, spin, true);
	if (body == kInvalidBody) return;

	Scene& scene = engine.GetScene();
	Block b;
	b.block = block;
	b.body = body;
	b.entity = scene.CreateEntity();
	Transform transform;
	transform.position = center;
	transform.rotation = rotation;
	scene.Add(b.entity, transform);
	scene.Add(b.entity, MeshRenderer{ MeshFor(engine, block, colorX, colorY, colorZ), mMaterial });
	scene.Add(b.entity, RigidBody{ body });
	mIndex[body] = mBlocks.size();
	mBlocks.push_back(b);

	// Over the cap: the oldest blocks still lying around crumble away (they never go back
	// into the rock).
	if (Count() > cfg::kMaxLooseBlocks)
	{
		for (Block& old : mBlocks)
		{
			if (old.body != kInvalidBody) { StartLeaving(engine, old, false); ++mCrumbled; break; }
		}
	}
}

std::function<bool(int, int, int)> LooseBlocks::OccupiedCells(Engine& engine) const
{
	auto cells = std::make_shared<std::unordered_set<uint64_t>>();
	const PhysicsWorld& physics = engine.GetPhysics();
	const float s = cfg::kVoxelSize;
	auto key = [](int x, int y, int z) { return (static_cast<uint64_t>(x + 4096) << 42) | (static_cast<uint64_t>(y + 4096) << 21) | static_cast<uint64_t>(z + 4096); };
	for (const Block& b : mBlocks)
	{
		if (b.body == kInvalidBody) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, false);
		cells->insert(key(static_cast<int>(std::floor(p.x / s)), static_cast<int>(std::floor(p.y / s)), static_cast<int>(std::floor(p.z / s))));
	}
	return [cells, key](int x, int y, int z) { return cells->count(key(x, y, z)) != 0; };
}

void LooseBlocks::StartLeaving(Engine& engine, Block& b, bool collected)
{
	if (b.body == kInvalidBody) return;
	XMFLOAT4 rotation;
	engine.GetPhysics().GetTransform(b.body, b.from, rotation, true);
	Scene& scene = engine.GetScene();
	scene.Remove<RigidBody>(b.entity);   // the game moves it from now on
	engine.GetPhysics().RemoveBody(b.body);
	mIndex.erase(b.body);
	b.body = kInvalidBody;
	b.leaving = 0.0f;
	b.collected = collected;
}

void LooseBlocks::Update(Engine& engine, float dt, const XMFLOAT3& collector)
{
	Scene& scene = engine.GetScene();
	const PhysicsWorld& physics = engine.GetPhysics();
	for (size_t i = 0; i < mBlocks.size(); )
	{
		Block& b = mBlocks[i];
		b.age += dt;
		bool remove = false;
		if (b.body != kInvalidBody)
		{
			XMFLOAT3 p;
			XMFLOAT4 r;
			physics.GetTransform(b.body, p, r, false);
			if (p.y < cfg::kBlockLostY) remove = true;   // fell out of the world
		}
		else
		{
			b.leaving += dt;
			Transform& t = scene.Get<Transform>(b.entity);
			if (b.collected)
			{
				// Flies into the backpack, shrinking as it goes.
				const float u = std::min(b.leaving / cfg::kScoopFlyTime, 1.0f);
				const float e = u * u;
				t.position = { b.from.x + (collector.x - b.from.x) * e, b.from.y + (collector.y - b.from.y) * e, b.from.z + (collector.z - b.from.z) * e };
				const float k = 1.0f - 0.7f * u;
				t.scale = { k, k, k };
				remove = u >= 1.0f;
			}
			else
			{
				const float u = std::min(b.leaving / cfg::kBlockCrumbleTime, 1.0f);
				const float k = 1.0f - u * u;
				t.scale = { k, k, k };
				remove = u >= 1.0f;
			}
		}
		if (remove)
		{
			Release(engine, b);
			// Swap-and-pop; fix the index of the block that moved.
			if (i + 1 != mBlocks.size())
			{
				mBlocks[i] = mBlocks.back();
				if (mBlocks[i].body != kInvalidBody) mIndex[mBlocks[i].body] = i;
			}
			mBlocks.pop_back();
			continue;
		}
		++i;
	}
}

std::vector<BodyId> LooseBlocks::InReach(Engine& engine, const XMFLOAT3& point, float radius, int max) const
{
	const PhysicsWorld& physics = engine.GetPhysics();
	std::vector<std::pair<float, BodyId>> nearby;
	for (const Block& b : mBlocks)
	{
		if (b.body == kInvalidBody) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, true);
		const float dx = p.x - point.x, dy = p.y - point.y, dz = p.z - point.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 <= radius * radius) nearby.push_back({ d2, b.body });
	}
	std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	if (static_cast<int>(nearby.size()) > max) nearby.resize(max);
	std::vector<BodyId> out;
	out.reserve(nearby.size());
	for (const auto& n : nearby) out.push_back(n.second);
	return out;
}

int LooseBlocks::Collect(Engine& engine, const std::vector<BodyId>& bodies, Counts& taken)
{
	int count = 0;
	for (BodyId body : bodies)
	{
		auto it = mIndex.find(body);
		if (it == mIndex.end()) continue;
		Block& b = mBlocks[it->second];
		++taken[static_cast<int>(b.block)];
		StartLeaving(engine, b, true);
		++count;
	}
	return count;
}

void LooseBlocks::Outline(Engine& engine, const std::vector<BodyId>& bodies, uint32_t color) const
{
	const PhysicsWorld& physics = engine.GetPhysics();
	const float half = cfg::kVoxelSize * 0.52f;
	for (BodyId body : bodies)
	{
		if (!Owns(body)) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(body, p, r, true);
		engine.GetRenderer().Debug().Box(BoundingOrientedBox(p, { half, half, half }, r), color);
	}
}

void LooseBlocks::Release(Engine& engine, Block& b)
{
	engine.GetScene().DestroyEntity(b.entity);
	if (b.body != kInvalidBody)
	{
		engine.GetPhysics().RemoveBody(b.body);
		mIndex.erase(b.body);
	}
}

void LooseBlocks::Clear(Engine& engine)
{
	for (Block& b : mBlocks) Release(engine, b);
	mBlocks.clear();
	mIndex.clear();
}

void LooseBlocks::Shutdown(Engine& engine)
{
	Clear(engine);
	for (auto& [key, mesh] : mMeshes) engine.GetRenderer().DestroyMesh(mesh);
	mMeshes.clear();
}
