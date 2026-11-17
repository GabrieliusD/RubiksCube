#include "Fragments.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

using namespace DirectX;
using cfg::Block;

int FragmentVoxels::SolidCount() const
{
	return static_cast<int>(std::count_if(voxels.begin(), voxels.end(), [](Block b) { return b != Block::Air; }));
}

std::vector<PhysicsBox> DecomposeIntoBoxes(const FragmentVoxels& fragment, int maxBoxes)
{
	const float s = cfg::kVoxelSize;
	const int maxDim = std::max({ fragment.size.x, fragment.size.y, fragment.size.z });
	std::vector<PhysicsBox> boxes;

	for (int scale = 1; ; scale *= 2)
	{
		// Occupancy at this scale: a cell is solid if any voxel inside it is.
		const int sx = (fragment.size.x + scale - 1) / scale;
		const int sy = (fragment.size.y + scale - 1) / scale;
		const int sz = (fragment.size.z + scale - 1) / scale;
		std::vector<uint8_t> occupied(static_cast<size_t>(sx) * sy * sz, 0);
		auto idx = [&](int x, int y, int z) { return x + sx * (y + sy * z); };
		for (int z = 0; z < fragment.size.z; ++z)
			for (int y = 0; y < fragment.size.y; ++y)
				for (int x = 0; x < fragment.size.x; ++x)
					if (fragment.At(x, y, z) != Block::Air) occupied[idx(x / scale, y / scale, z / scale)] = 1;

		// Greedy 3D merge: grow along x, then y, then z.
		boxes.clear();
		std::vector<uint8_t> used(occupied.size(), 0);
		auto free = [&](int x, int y, int z) { return occupied[idx(x, y, z)] && !used[idx(x, y, z)]; };
		for (int z = 0; z < sz; ++z)
		for (int y = 0; y < sy; ++y)
		for (int x = 0; x < sx; ++x)
		{
			if (!free(x, y, z)) continue;
			int x1 = x + 1;
			while (x1 < sx && free(x1, y, z)) ++x1;
			int y1 = y + 1;
			for (bool grow = true; grow && y1 < sy; )
			{
				for (int xx = x; xx < x1 && grow; ++xx) grow = free(xx, y1, z);
				if (grow) ++y1;
			}
			int z1 = z + 1;
			for (bool grow = true; grow && z1 < sz; )
			{
				for (int yy = y; yy < y1 && grow; ++yy)
					for (int xx = x; xx < x1 && grow; ++xx) grow = free(xx, yy, z1);
				if (grow) ++z1;
			}
			for (int zz = z; zz < z1; ++zz)
				for (int yy = y; yy < y1; ++yy)
					for (int xx = x; xx < x1; ++xx) used[idx(xx, yy, zz)] = 1;

			// Back to voxels (clamped to the fragment), then metres.
			const float vx0 = static_cast<float>(x * scale), vx1 = static_cast<float>(std::min(x1 * scale, fragment.size.x));
			const float vy0 = static_cast<float>(y * scale), vy1 = static_cast<float>(std::min(y1 * scale, fragment.size.y));
			const float vz0 = static_cast<float>(z * scale), vz1 = static_cast<float>(std::min(z1 * scale, fragment.size.z));
			PhysicsBox box;
			box.center = { (vx0 + vx1) * 0.5f * s, (vy0 + vy1) * 0.5f * s, (vz0 + vz1) * 0.5f * s };
			box.halfExtents = { (vx1 - vx0) * 0.5f * s, (vy1 - vy0) * 0.5f * s, (vz1 - vz0) * 0.5f * s };
			boxes.push_back(box);
		}

		if (static_cast<int>(boxes.size()) <= maxBoxes || scale >= maxDim)
		{
			return boxes;
		}
	}
}

std::vector<BakedVoxel> SnapToGrid(const FragmentVoxels& fragment, const XMFLOAT3& position, const XMFLOAT4& rotation)
{
	const float s = cfg::kVoxelSize;
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&rotation)) * XMMatrixTranslation(position.x, position.y, position.z);
	std::vector<BakedVoxel> out;
	std::unordered_set<uint64_t> taken;
	for (int z = 0; z < fragment.size.z; ++z)
	for (int y = 0; y < fragment.size.y; ++y)
	for (int x = 0; x < fragment.size.x; ++x)
	{
		const Block block = fragment.At(x, y, z);
		if (block == Block::Air) continue;
		XMFLOAT3 p;
		XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet((x + 0.5f) * s, (y + 0.5f) * s, (z + 0.5f) * s, 1.0f), world));
		const int cx = static_cast<int>(std::floor(p.x / s));
		const int cy = static_cast<int>(std::floor(p.y / s));
		const int cz = static_cast<int>(std::floor(p.z / s));
		const uint64_t key = (static_cast<uint64_t>(cx + 4096) << 42) | (static_cast<uint64_t>(cy + 4096) << 21) | static_cast<uint64_t>(cz + 4096);
		if (!taken.insert(key).second) continue;   // two voxels landed in the same cell
		out.push_back({ cx, cy, cz, block });
	}
	return out;
}

// =============================================================================

void FragmentSystem::Initialize(VoxelVolumeHandle worldVolume)
{
	mWorldVolume = worldVolume;
}

bool FragmentSystem::Spawn(Engine& engine, FragmentVoxels&& fragment, const XMFLOAT3& position,
	const XMFLOAT3& linearVelocity, const XMFLOAT3& angularVelocity)
{
	if (fragment.SolidCount() == 0) return false;
	PhysicsWorld& physics = engine.GetPhysics();
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();

	const std::vector<PhysicsBox> boxes = DecomposeIntoBoxes(fragment, cfg::kMaxFragmentBoxes);
	const BodyId body = physics.CreateDynamicBoxes(boxes, cfg::kRockDensity, position, { 0, 0, 0, 1 }, linearVelocity, angularVelocity);
	if (body == kInvalidBody) return false;

	// Rendered as its own small voxel volume, coloured exactly as it was in the world.
	VoxelVolumeDesc desc;
	desc.size = { static_cast<uint32_t>(fragment.size.x), static_cast<uint32_t>(fragment.size.y), static_cast<uint32_t>(fragment.size.z) };
	desc.voxels.resize(fragment.voxels.size());
	for (int z = 0; z < fragment.size.z; ++z)
		for (int y = 0; y < fragment.size.y; ++y)
			for (int x = 0; x < fragment.size.x; ++x)
				desc.voxels[x + fragment.size.x * (y + fragment.size.y * z)] = VoxelWorld::PaletteIndex(fragment.At(x, y, z), fragment.worldOrigin.z + z);
	desc.sharePaletteFrom = mWorldVolume;

	Fragment f;
	f.volume = renderer.CreateVoxelVolume(desc);
	f.body = body;

	f.entity = scene.CreateEntity();
	Transform transform;
	transform.position = position;
	scene.Add(f.entity, transform);
	VoxelVolumeRenderer volume;
	volume.volume = f.volume;
	volume.voxelSize = cfg::kVoxelSize;
	volume.colorJitter = cfg::kStoneJitter;
	volume.aoStrength = cfg::kVoxelAoStrength;
	volume.jitterOffset = fragment.worldOrigin;
	scene.Add(f.entity, volume);
	scene.Add(f.entity, RigidBody{ body });

	f.voxels = std::move(fragment);
	mFragments.push_back(std::move(f));

	// Too many loose pieces: the oldest bakes on the next update.
	if (static_cast<int>(mFragments.size()) > cfg::kMaxActiveFragments)
	{
		mFragments.front().age = cfg::kFragmentMaxLifetime;
	}
	return true;
}

void FragmentSystem::Update(Engine& engine, VoxelWorld& world, float dt,
	const XMFLOAT3& keepClearMin, const XMFLOAT3& keepClearMax, std::vector<VoxelWorld::Box>& changed)
{
	PhysicsWorld& physics = engine.GetPhysics();
	const auto start = std::chrono::steady_clock::now();
	bool baked = false;

	for (size_t i = 0; i < mFragments.size(); )
	{
		Fragment& f = mFragments[i];
		f.age += dt;

		XMFLOAT3 position;
		XMFLOAT4 rotation;
		physics.GetTransform(f.body, position, rotation, false);
		const bool resting = f.age >= cfg::kFragmentMinLifetime && physics.IsSleeping(f.body);
		const bool lost = position.y < -20.0f;
		if (resting || f.age >= cfg::kFragmentMaxLifetime || lost)
		{
			if (!lost) Bake(engine, world, f, keepClearMin, keepClearMax, changed);
			Release(engine, f);
			mFragments.erase(mFragments.begin() + static_cast<std::ptrdiff_t>(i));
			baked = true;
			continue;
		}
		++i;
	}
	if (baked)
	{
		mLastBakeMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
}

void FragmentSystem::Bake(Engine& engine, VoxelWorld& world, Fragment& f,
	const XMFLOAT3& clearMin, const XMFLOAT3& clearMax, std::vector<VoxelWorld::Box>& changed)
{
	if (f.voxels.SolidCount() < cfg::kMinBakeVoxels)
	{
		++mVanishedCount;   // dust
		return;
	}

	XMFLOAT3 position;
	XMFLOAT4 rotation;
	engine.GetPhysics().GetTransform(f.body, position, rotation, false);

	const float s = cfg::kVoxelSize;
	VoxelWorld::Box box;
	for (const BakedVoxel& v : SnapToGrid(f.voxels, position, rotation))
	{
		if (!VoxelWorld::InGrid(v.x, v.y, v.z) || world.Get(v.x, v.y, v.z) != Block::Air) continue;
		// Never bake rock into the player.
		const bool overlapsPlayer = (v.x + 1) * s > clearMin.x && v.x * s < clearMax.x
			&& (v.y + 1) * s > clearMin.y && v.y * s < clearMax.y
			&& (v.z + 1) * s > clearMin.z && v.z * s < clearMax.z;
		if (overlapsPlayer) continue;

		world.Set(v.x, v.y, v.z, v.block);
		if (box.Empty())
		{
			box.min[0] = box.max[0] = v.x; box.min[1] = box.max[1] = v.y; box.min[2] = box.max[2] = v.z;
		}
		else
		{
			box.min[0] = std::min(box.min[0], v.x); box.max[0] = std::max(box.max[0], v.x);
			box.min[1] = std::min(box.min[1], v.y); box.max[1] = std::max(box.max[1], v.y);
			box.min[2] = std::min(box.min[2], v.z); box.max[2] = std::max(box.max[2], v.z);
		}
	}
	if (!box.Empty())
	{
		changed.push_back(box);
		++mBakedCount;
	}
}

void FragmentSystem::Release(Engine& engine, Fragment& f)
{
	engine.GetScene().DestroyEntity(f.entity);
	engine.GetPhysics().RemoveBody(f.body);
	engine.GetRenderer().DestroyVoxelVolume(f.volume);
}

bool FragmentSystem::Owns(BodyId body) const
{
	return std::any_of(mFragments.begin(), mFragments.end(), [body](const Fragment& f) { return f.body == body; });
}

void FragmentSystem::Clear(Engine& engine)
{
	for (Fragment& f : mFragments) Release(engine, f);
	mFragments.clear();
}
