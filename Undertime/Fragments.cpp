#include "Fragments.h"
#include "ColorUtil.h"
#include <algorithm>
#include <cmath>

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

// =============================================================================

void FragmentSystem::Initialize(VoxelVolumeHandle worldVolume, MaterialHandle material)
{
	mWorldVolume = worldVolume;
	mMaterial = material;
}

bool FragmentSystem::Spawn(Engine& engine, FragmentVoxels&& fragment, const XMFLOAT3& position,
	const XMFLOAT3& linearVelocity, const XMFLOAT3& angularVelocity, const XMFLOAT4& rotation)
{
	if (fragment.SolidCount() == 0) return false;
	PhysicsWorld& physics = engine.GetPhysics();
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();

	const std::vector<PhysicsBox> boxes = DecomposeIntoBoxes(fragment, cfg::kMaxFragmentBoxes);
	const BodyId body = physics.CreateDynamicBoxes(boxes, cfg::kRockDensity, position, rotation, linearVelocity, angularVelocity);
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
	transform.rotation = rotation;
	scene.Add(f.entity, transform);
	VoxelVolumeRenderer volume;
	volume.volume = f.volume;
	volume.voxelSize = cfg::kVoxelSize;
	volume.colorJitter = cfg::kStoneJitter;
	volume.aoStrength = cfg::kVoxelAoStrength;
	volume.jitterOffset = fragment.worldOrigin;
	volume.material = mMaterial;   // jitterOffset also keeps Local Position patterns in place
	scene.Add(f.entity, volume);
	scene.Add(f.entity, RigidBody{ body });

	f.voxels = std::move(fragment);
	mFragments.push_back(std::move(f));
	return true;
}

void FragmentSystem::Update(Engine& engine, float dt)
{
	PhysicsWorld& physics = engine.GetPhysics();
	for (size_t i = 0; i < mFragments.size(); )
	{
		Fragment& f = mFragments[i];
		f.age += dt;
		XMFLOAT3 position;
		XMFLOAT4 rotation;
		physics.GetTransform(f.body, position, rotation, false);
		if (position.y < cfg::kBlockLostY)
		{
			Release(engine, f);
			mFragments.erase(mFragments.begin() + static_cast<std::ptrdiff_t>(i));
			continue;
		}
		++i;
	}
}

ChipResult FragmentSystem::ShatterOldestOverCap(Engine& engine)
{
	ChipResult result;
	if (static_cast<int>(mFragments.size()) <= cfg::kMaxBoulders) return result;
	Fragment& f = mFragments.front();   // spawned first = oldest
	PhysicsWorld& physics = engine.GetPhysics();
	physics.GetTransform(f.body, result.position, result.rotation, false);
	result.velocity = physics.GetLinearVelocity(f.body);
	result.worldOrigin = f.voxels.worldOrigin;
	for (int z = 0; z < f.voxels.size.z; ++z)
		for (int y = 0; y < f.voxels.size.y; ++y)
			for (int x = 0; x < f.voxels.size.x; ++x)
				if (f.voxels.At(x, y, z) != Block::Air) result.voxels.push_back({ x, y, z, f.voxels.At(x, y, z) });
	result.hit = !result.voxels.empty();
	Release(engine, f);
	mFragments.erase(mFragments.begin());
	return result;
}

ChipResult FragmentSystem::Chip(Engine& engine, BodyId body, const XMFLOAT3& point, float pickFactor, uint32_t salt)
{
	ChipResult result;
	auto it = std::find_if(mFragments.begin(), mFragments.end(), [body](const Fragment& f) { return f.body == body; });
	if (it == mFragments.end()) return result;

	PhysicsWorld& physics = engine.GetPhysics();
	const float s = cfg::kVoxelSize;
	XMFLOAT3 position;
	XMFLOAT4 rotation;
	physics.GetTransform(body, position, rotation, false);
	const XMFLOAT3 velocity = physics.GetLinearVelocity(body);

	// The hit point in the fragment's local space (metres).
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&rotation)) * XMMatrixTranslation(position.x, position.y, position.z);
	XMFLOAT3 local;
	XMStoreFloat3(&local, XMVector3TransformCoord(XMLoadFloat3(&point), XMMatrixInverse(nullptr, world)));

	const XMINT3 size = it->voxels.size;
	const XMINT3 baseOrigin = it->voxels.worldOrigin;
	std::vector<Block> voxels = it->voxels.voxels;
	auto index = [&](int x, int y, int z) { return x + size.x * (y + size.y * z); };

	// Same bite rule as the world, a little easier because the rock is already broken.
	int nearest = -1;
	float nearestDistance = 1e30f;
	for (int z = 0; z < size.z; ++z)
	for (int y = 0; y < size.y; ++y)
	for (int x = 0; x < size.x; ++x)
	{
		const Block block = voxels[index(x, y, z)];
		if (block == Block::Air) continue;
		const float dx = (x + 0.5f) * s - local.x, dy = (y + 0.5f) * s - local.y, dz = (z + 0.5f) * s - local.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (distance < nearestDistance) { nearestDistance = distance; nearest = index(x, y, z); }
		const float hardness = cfg::Info(block).hardness * (1.0f + (baseOrigin.z + z) * s / cfg::kHardnessDepthScale);
		const float strength = std::min(std::sqrt(pickFactor * cfg::kLooseRockBonus / std::max(hardness, 0.01f)), cfg::kBiteMaxScale);
		const float rough = 1.0f + cfg::kBiteRoughness * (color::Hash01(x, y, z, salt) * 2.0f - 1.0f);
		if (distance <= cfg::kBiteRadius * strength * rough)
		{
			result.voxels.push_back({ x, y, z, block });
			voxels[index(x, y, z)] = Block::Air;
		}
	}
	if (result.voxels.empty() && nearest >= 0)
	{
		const int x = nearest % size.x, y = (nearest / size.x) % size.y, z = nearest / (size.x * size.y);
		result.voxels.push_back({ x, y, z, voxels[nearest] });
		voxels[nearest] = Block::Air;
	}
	if (result.voxels.empty()) return result;

	result.hit = true;
	result.worldOrigin = baseOrigin;
	result.position = position;
	result.rotation = rotation;
	result.velocity = velocity;

	// Split what's left into 6-connected pieces.
	std::vector<int> label(voxels.size(), -1);
	std::vector<int> stack;
	std::vector<std::vector<int>> pieces;
	for (int first = 0; first < static_cast<int>(voxels.size()); ++first)
	{
		if (voxels[first] == Block::Air || label[first] >= 0) continue;
		const int id = static_cast<int>(pieces.size());
		pieces.emplace_back();
		stack.assign(1, first);
		label[first] = id;
		while (!stack.empty())
		{
			const int c = stack.back();
			stack.pop_back();
			pieces[id].push_back(c);
			const int x = c % size.x, y = (c / size.x) % size.y, z = c / (size.x * size.y);
			const int n[6][3] = { { x - 1, y, z }, { x + 1, y, z }, { x, y - 1, z }, { x, y + 1, z }, { x, y, z - 1 }, { x, y, z + 1 } };
			for (const auto& q : n)
			{
				if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= size.x || q[1] >= size.y || q[2] >= size.z) continue;
				const int j = index(q[0], q[1], q[2]);
				if (voxels[j] == Block::Air || label[j] >= 0) continue;
				label[j] = id;
				stack.push_back(j);
			}
		}
	}

	// Remove the old body, then respawn each piece where it was.
	Release(engine, *it);
	mFragments.erase(it);
	for (const std::vector<int>& piece : pieces)
	{
		if (static_cast<int>(piece.size()) < cfg::kMinBoulderVoxels)
		{
			// Too small to stay a boulder: goes out with the bite.
			for (int c : piece)
			{
				result.voxels.push_back({ c % size.x, (c / size.x) % size.y, c / (size.x * size.y), voxels[c] });
			}
			continue;
		}
		int lo[3] = { size.x, size.y, size.z }, hi[3] = { -1, -1, -1 };
		for (int c : piece)
		{
			const int p[3] = { c % size.x, (c / size.x) % size.y, c / (size.x * size.y) };
			for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
		}
		FragmentVoxels out;
		out.size = { hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1 };
		out.worldOrigin = { baseOrigin.x + lo[0], baseOrigin.y + lo[1], baseOrigin.z + lo[2] };
		out.voxels.assign(static_cast<size_t>(out.size.x) * out.size.y * out.size.z, Block::Air);
		for (int c : piece)
		{
			const int x = c % size.x - lo[0], y = (c / size.x) % size.y - lo[1], z = c / (size.x * size.y) - lo[2];
			out.voxels[x + out.size.x * (y + out.size.y * z)] = voxels[c];
		}
		XMFLOAT3 piecePosition;
		XMStoreFloat3(&piecePosition, XMVector3TransformCoord(XMVectorSet(lo[0] * s, lo[1] * s, lo[2] * s, 1.0f), world));
		Spawn(engine, std::move(out), piecePosition, velocity, { 0, 0, 0 }, rotation);
	}
	return result;
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
