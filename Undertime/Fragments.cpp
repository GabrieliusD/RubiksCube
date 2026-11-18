#include "Fragments.h"
#include "ColorUtil.h"
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
	const XMFLOAT3& linearVelocity, const XMFLOAT3& angularVelocity, FragmentKind kind, const XMFLOAT4& rotation)
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
	scene.Add(f.entity, volume);
	scene.Add(f.entity, RigidBody{ body });

	f.voxels = std::move(fragment);
	f.kind = kind;
	mFragments.push_back(std::move(f));

	// Over the caps: the oldest rubble crumbles early; the oldest boulder bakes.
	if (kind == FragmentKind::Rubble && CountKind(FragmentKind::Rubble) > cfg::kMaxRubble)
	{
		for (Fragment& old : mFragments)
		{
			if (old.kind == FragmentKind::Rubble && old.crumble < 0.0f) { old.crumble = 0.0f; break; }
		}
	}
	if (kind == FragmentKind::Structural && CountKind(FragmentKind::Structural) > cfg::kMaxBoulders)
	{
		for (Fragment& old : mFragments)
		{
			if (old.kind == FragmentKind::Structural && !old.forceBake) { old.forceBake = true; break; }
		}
	}
	return true;
}

int FragmentSystem::CountKind(FragmentKind kind) const
{
	return static_cast<int>(std::count_if(mFragments.begin(), mFragments.end(), [kind](const Fragment& f) { return f.kind == kind; }));
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
		const XMFLOAT3 v = physics.GetLinearVelocity(f.body);
		const bool sleeping = physics.IsSleeping(f.body);
		const bool slow = v.x * v.x + v.y * v.y + v.z * v.z < cfg::kRestSpeed * cfg::kRestSpeed;
		f.still = (sleeping || slow) ? f.still + dt : 0.0f;
		const bool settled = sleeping || f.still >= cfg::kRestTime;
		const bool lost = position.y < -20.0f;

		if (f.kind == FragmentKind::Nugget)
		{
			// Collected in CollectNuggets; only lost or forgotten ones end here.
			if (lost || f.age > cfg::kNuggetMaxLifetime)
			{
				Release(engine, f);
				mFragments.erase(mFragments.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			++i;
			continue;
		}

		if (f.kind == FragmentKind::Rubble)
		{
			// Rubble settles, lingers a moment, then shrinks to nothing.
			if (f.crumble < 0.0f && ((f.age >= cfg::kRubbleMinLifetime && settled) || f.age >= cfg::kRubbleMaxLifetime))
			{
				f.crumble = 0.0f;
			}
			if (f.crumble >= 0.0f) f.crumble += dt;
			const float linger = cfg::kRubbleLinger, shrink = cfg::kRubbleShrinkTime;
			if (f.crumble >= linger + shrink || lost)
			{
				Release(engine, f);
				++mVanishedCount;
				mFragments.erase(mFragments.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			if (f.crumble > linger)
			{
				const float u = (f.crumble - linger) / shrink;
				engine.GetScene().Get<VoxelVolumeRenderer>(f.entity).voxelSize = cfg::kVoxelSize * (1.0f - u * u);
			}
			++i;
			continue;
		}

		// Structural: boulders stay loose (break them with the pick); big slabs, and the
		// oldest boulders past the cap, bake back into the world once they rest.
		const bool resting = f.age >= cfg::kFragmentMinLifetime && settled;
		const bool big = f.voxels.SolidCount() > cfg::kBoulderMaxVoxels;
		if ((resting && (big || f.forceBake)) || (big && f.age >= cfg::kFragmentMaxLifetime) || lost)
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

int FragmentSystem::CollectNuggets(Engine& engine, const XMFLOAT3& collector, float dt,
	std::array<int, static_cast<int>(Block::Count)>& collected)
{
	PhysicsWorld& physics = engine.GetPhysics();
	const float s = cfg::kVoxelSize;
	int count = 0;
	for (size_t i = 0; i < mFragments.size(); )
	{
		Fragment& f = mFragments[i];
		if (f.kind != FragmentKind::Nugget) { ++i; continue; }

		XMFLOAT3 position;
		XMFLOAT4 rotation;
		physics.GetTransform(f.body, position, rotation, false);
		XMFLOAT3 center;
		const float half = f.voxels.size.x * s * 0.5f;
		XMStoreFloat3(&center, XMVector3Rotate(XMVectorSet(half, half, half, 0.0f), XMLoadFloat4(&rotation)) + XMLoadFloat3(&position));
		const XMFLOAT3 to{ collector.x - center.x, collector.y - center.y, collector.z - center.z };
		const float distance = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);

		if (f.pull < 0.0f && f.age >= cfg::kNuggetPickupDelay && distance < cfg::kNuggetPullRadius) f.pull = 0.0f;
		if (f.pull >= 0.0f) f.pull += dt;

		if (f.pull >= 0.0f && (distance < cfg::kNuggetCollectRadius || f.pull >= cfg::kNuggetPullTimeout))
		{
			for (Block b : f.voxels.voxels)
				if (b != Block::Air) ++collected[static_cast<int>(b)];
			Release(engine, f);
			mFragments.erase(mFragments.begin() + static_cast<std::ptrdiff_t>(i));
			++count;
			continue;
		}

		if (f.pull >= 0.0f && distance > 1e-3f)
		{
			// Steer its velocity towards the player: impulse = mass * velocity change.
			const float mass = f.voxels.SolidCount() * s * s * s * cfg::kRockDensity;
			const float speed = cfg::kNuggetPullSpeed * std::min(1.0f, f.pull / 0.15f);
			const XMFLOAT3 v = physics.GetLinearVelocity(f.body);
			const XMFLOAT3 want{ to.x / distance * speed, to.y / distance * speed + 0.6f, to.z / distance * speed };
			const float k = std::min(1.0f, cfg::kNuggetPullResponse * dt);
			physics.AddImpulse(f.body, { (want.x - v.x) * k * mass, (want.y - v.y) * k * mass, (want.z - v.z) * k * mass }, center);
		}
		++i;
	}
	return count;
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
		Spawn(engine, std::move(out), piecePosition, velocity, { 0, 0, 0 }, FragmentKind::Structural, rotation);
	}
	return result;
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
	return std::any_of(mFragments.begin(), mFragments.end(), [body](const Fragment& f) { return f.body == body && f.kind == FragmentKind::Structural; });
}

void FragmentSystem::Clear(Engine& engine)
{
	for (Fragment& f : mFragments) Release(engine, f);
	mFragments.clear();
}
