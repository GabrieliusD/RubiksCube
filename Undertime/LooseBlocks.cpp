#include "LooseBlocks.h"
#include "Meshes.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_set>
#include <DirectXCollision.h>

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

namespace
{
	int64_t PackCell(int x, int y, int z)
	{
		constexpr int64_t kOffset = 1 << 20;
		return ((x + kOffset) << 42) | ((y + kOffset) << 21) | (z + kOffset);
	}

	int CellOf(float metres) { return static_cast<int>(std::floor(metres / cfg::kPileCell)); }

	uint64_t VoxelKey(const XMFLOAT3& p)
	{
		const float s = cfg::kVoxelSize;
		return static_cast<uint64_t>(PackCell(static_cast<int>(std::floor(p.x / s)), static_cast<int>(std::floor(p.y / s)), static_cast<int>(std::floor(p.z / s))));
	}

	float BlockHalf() { return cfg::kVoxelSize * 0.5f * (1.0f - cfg::kBlockGap); }

	XMFLOAT4 ColourOf(uint16_t key)
	{
		auto back = [](uint32_t v) { const float c = v / 31.0f; return c * c; };
		return { back((key >> 10) & 31), back((key >> 5) & 31), back(key & 31), 1.0f };
	}
}

int64_t PileCellKey(const XMFLOAT3& p)
{
	return PackCell(CellOf(p.x), CellOf(p.y), CellOf(p.z));
}

// =============================================================================

void LooseBlocks::Initialize(MaterialHandle material)
{
	mMaterial = material;
}

uint16_t LooseBlocks::ColourKey(cfg::Block block, int x, int y, int z)
{
	// Colours quantised (in a perceptual-ish space) so blocks share a handful of meshes.
	const XMFLOAT4 linear = VoxelWorld::VoxelColor(block, x, y, z);
	auto q = [](float c) { return static_cast<uint32_t>(std::clamp(std::sqrt(std::max(c, 0.0f)), 0.0f, 1.0f) * 31.0f + 0.5f); };
	return static_cast<uint16_t>((q(linear.x) << 10) | (q(linear.y) << 5) | q(linear.z));
}

MeshHandle LooseBlocks::MeshFor(Engine& engine, uint16_t colour)
{
	auto it = mMeshes.find(colour);
	if (it != mMeshes.end()) return it->second;
	MeshData mesh;
	const float half = BlockHalf();
	meshes::AddBox(mesh, { 0, 0, 0 }, { half, half, half }, ColourOf(colour));
	const MeshHandle handle = engine.GetRenderer().CreateMesh(mesh);
	mMeshes.emplace(colour, handle);
	return handle;
}

void LooseBlocks::Spawn(Engine& engine, cfg::Block block, int colorX, int colorY, int colorZ, const XMFLOAT3& center,
	const XMFLOAT4& rotation, const XMFLOAT3& velocity, const XMFLOAT3& spin)
{
	if (block == cfg::Block::Air) return;
	SpawnLive(engine, block, ColourKey(block, colorX, colorY, colorZ), center, rotation, velocity, spin);
}

LooseBlocks::Settled LooseBlocks::MakeSettled(const XMFLOAT3& p, const XMFLOAT4& r, cfg::Block block, uint16_t colour, bool town)
{
	Settled s{ p, r, block, colour, town };
	if (!town && InYard(p))
	{
		s.town = true;
		++mYardPaid[static_cast<int>(block)];
		++mYardCount;
	}
	return s;
}

LooseBlocks::Counts LooseBlocks::TakeYardPaid()
{
	const Counts paid = mYardPaid;
	mYardPaid.fill(0);
	return paid;
}

void LooseBlocks::FillYard(Engine& engine, cfg::Block block, int count, float groundY)
{
	// A stepped mound in the middle of the yard: square layers, each one block smaller all
	// round, as wide as it needs to be for `count` (a little jittered, so it reads as a heap).
	const float s = cfg::kVoxelSize;
	const int nx = std::max(2, static_cast<int>((mYard[2] - mYard[0]) / s) - 2);
	const int nz = std::max(2, static_cast<int>((mYard[3] - mYard[1]) / s) - 2);
	const int widest = std::min(nx, nz);
	auto holds = [](int side) { int n = 0; for (int k = side; k > 0; k -= 2) n += k * k; return n; };
	int side = 2;
	while (side < widest && holds(side) < count) side += 1;
	const float x0 = (mYard[0] + mYard[2]) * 0.5f - side * s * 0.5f, z0 = (mYard[1] + mYard[3]) * 0.5f - side * s * 0.5f;
	const uint16_t colour = ColourKey(block, cfg::kVoxX / 2, 4, 2);
	int placed = 0;
	for (int layer = 0; placed < count; ++layer)
	{
		const int shrink = std::min(layer, (side - 1) / 2);   // past the top, layers of the smallest square
		const int across = side - 2 * shrink;
		for (int z = 0; z < across && placed < count; ++z)
			for (int x = 0; x < across && placed < count; ++x)
			{
				uint32_t h = static_cast<uint32_t>(placed) * 2654435761u;
				h ^= h >> 15;
				const float jx = ((h & 0xff) / 255.0f - 0.5f) * 0.06f, jz = (((h >> 8) & 0xff) / 255.0f - 0.5f) * 0.06f;
				const XMFLOAT3 p{ x0 + (shrink + x + 0.5f) * s + jx, groundY + (layer + 0.5f) * s, z0 + (shrink + z + 0.5f) * s + jz };
				XMFLOAT4 r;
				XMStoreFloat4(&r, XMQuaternionRotationRollPitchYaw(0.0f, ((h >> 16) & 0xff) / 255.0f * 0.5f - 0.25f, 0.0f));
				Pile& pile = mPiles[PileFor(engine, PileCellKey(p))];
				pile.blocks.push_back(Settled{ p, r, block, colour, true });
				pile.dirty = true;
				++mSettledCount;
				++mYardCount;
				++placed;
			}
	}
}

void LooseBlocks::SpawnLive(Engine& engine, cfg::Block block, uint16_t colour, const XMFLOAT3& center,
	const XMFLOAT4& rotation, const XMFLOAT3& velocity, const XMFLOAT3& spin, bool town)
{
	const float half = BlockHalf();
	const std::vector<PhysicsBox> box{ PhysicsBox{ { 0, 0, 0 }, { half, half, half } } };
	const BodyId body = engine.GetPhysics().CreateDynamicBoxes(box, cfg::kRockDensity, center, rotation, velocity, spin, !mOptions.solidBlocks);
	if (body == kInvalidBody)
	{
		// Out of physics bodies: the block still isn't lost, it joins the pile where it is.
		Settled s = MakeSettled(center, rotation, block, colour, town);
		Pile& pile = mPiles[PileFor(engine, PileCellKey(center))];
		pile.blocks.push_back(s);
		pile.dirty = true;
		++mSettledCount;
		return;
	}

	Scene& scene = engine.GetScene();
	Block b;
	b.block = block;
	b.colour = colour;
	b.body = body;
	b.town = town;
	b.entity = scene.CreateEntity();
	Transform transform;
	transform.position = center;
	transform.rotation = rotation;
	scene.Add(b.entity, transform);
	scene.Add(b.entity, MeshRenderer{ MeshFor(engine, colour), mMaterial });
	scene.Add(b.entity, RigidBody{ body });
	mIndex[body] = mBlocks.size();
	mBlocks.push_back(b);
}

std::function<bool(int, int, int)> LooseBlocks::OccupiedCells(Engine& engine) const
{
	auto cells = std::make_shared<std::unordered_set<uint64_t>>();
	const PhysicsWorld& physics = engine.GetPhysics();
	for (const Block& b : mBlocks)
	{
		if (b.body == kInvalidBody) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, false);
		cells->insert(VoxelKey(p));
	}
	for (const Pile& pile : mPiles)
		for (const Settled& s : pile.blocks) cells->insert(VoxelKey(s.position));
	return [cells](int x, int y, int z) { return cells->count(static_cast<uint64_t>(PackCell(x, y, z))) != 0; };
}

void LooseBlocks::StartLeaving(Engine& engine, Block& b, bool collected)
{
	if (b.body == kInvalidBody) return;
	XMFLOAT4 rotation;
	engine.GetPhysics().GetTransform(b.body, b.from, rotation, true);
	Scene& scene = engine.GetScene();
	scene.Remove<RigidBody>(b.entity);   // the game moves it from now on
	Unqueue(b);
	engine.GetPhysics().RemoveBody(b.body);
	mIndex.erase(b.body);
	b.body = kInvalidBody;
	b.leaving = 0.0f;
	b.collected = collected;
}

void LooseBlocks::TakeSettled(Engine& engine, int pileIndex, int index, bool collected)
{
	Pile& pile = mPiles[pileIndex];
	const Settled s = pile.blocks[index];
	pile.blocks[index] = pile.blocks.back();
	pile.blocks.pop_back();
	pile.dirty = true;
	--mSettledCount;

	// A bodiless block that only animates away (into the backpack or crumbling).
	Scene& scene = engine.GetScene();
	Block b;
	b.block = s.block;
	b.colour = s.colour;
	b.town = s.town;
	b.entity = scene.CreateEntity();
	Transform transform;
	transform.position = s.position;
	transform.rotation = s.rotation;
	scene.Add(b.entity, transform);
	scene.Add(b.entity, MeshRenderer{ MeshFor(engine, s.colour), mMaterial });
	b.from = s.position;
	b.leaving = 0.0f;
	b.collected = collected;
	mBlocks.push_back(b);
}

int LooseBlocks::PileFor(Engine&, int64_t key)
{
	auto it = mPileIndex.find(key);
	if (it != mPileIndex.end()) return it->second;
	Pile pile;
	pile.key = key;
	mPiles.push_back(std::move(pile));
	const int index = static_cast<int>(mPiles.size()) - 1;
	mPileIndex[key] = index;
	return index;
}

void LooseBlocks::Settle(Engine& engine, const Block& b)
{
	XMFLOAT3 p;
	XMFLOAT4 r;
	engine.GetPhysics().GetTransform(b.body, p, r, false);
	Pile& pile = mPiles[PileFor(engine, PileCellKey(p))];
	pile.blocks.push_back(MakeSettled(p, r, b.block, b.colour, b.town));
	pile.dirty = true;
	++mSettledCount;
}

void LooseBlocks::RebuildPile(Engine& engine, int index, float& meshMs, float& collisionMs)
{
	using Clock = std::chrono::high_resolution_clock;
	const auto start = Clock::now();
	Pile& pile = mPiles[index];
	pile.dirty = false;

	// One cube's faces, coloured per block below.
	static MeshData cube;
	if (cube.vertices.empty())
	{
		const float half = BlockHalf();
		meshes::AddBox(cube, { 0, 0, 0 }, { half, half, half }, { 1, 1, 1, 1 });
	}

	MeshData mesh;
	mesh.vertices.reserve(cube.vertices.size() * pile.blocks.size());
	mesh.indices.reserve(cube.indices.size() * pile.blocks.size());
	std::vector<XMFLOAT3> triangles;
	triangles.reserve(cube.indices.size() * pile.blocks.size());
	for (const Settled& s : pile.blocks)
	{
		const XMMATRIX rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&s.rotation));
		const XMMATRIX world = rotation * XMMatrixTranslation(s.position.x, s.position.y, s.position.z);
		const XMFLOAT4 colour = ColourOf(s.colour);
		const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
		for (const Vertex& v : cube.vertices)
		{
			Vertex out = v;
			XMStoreFloat3(&out.position, XMVector3TransformCoord(XMLoadFloat3(&v.position), world));
			XMStoreFloat3(&out.normal, XMVector3TransformNormal(XMLoadFloat3(&v.normal), rotation));
			out.color = colour;
			mesh.vertices.push_back(out);
		}
		for (uint32_t i : cube.indices)
		{
			mesh.indices.push_back(base + i);
			triangles.push_back(mesh.vertices[base + i].position);
		}
	}

	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	const MeshHandle old = pile.mesh;
	pile.mesh = renderer.CreateMesh(mesh);
	if (pile.entity == INVALID_ENTITY)
	{
		pile.entity = scene.CreateEntity();
		scene.Add(pile.entity, Transform{});
		scene.Add(pile.entity, MeshRenderer{ pile.mesh, mMaterial });
	}
	else
	{
		scene.Get<MeshRenderer>(pile.entity).mesh = pile.mesh;
	}
	if (old.IsValid()) renderer.DestroyMesh(old);   // the GPU copy is released once in-flight frames are done
	const auto meshDone = Clock::now();
	meshMs += std::chrono::duration<float, std::milli>(meshDone - start).count();

	PhysicsWorld& physics = engine.GetPhysics();
	if (pile.body != kInvalidBody)
	{
		mPileBodies.erase(pile.body);
		physics.RemoveBody(pile.body);
	}
	pile.body = physics.CreateStaticMesh(triangles, !mOptions.solidPiles);
	if (pile.body != kInvalidBody) mPileBodies[pile.body] = index;
	collisionMs += std::chrono::duration<float, std::milli>(Clock::now() - meshDone).count();
}

void LooseBlocks::DestroyPile(Engine& engine, Pile& pile)
{
	if (pile.entity != INVALID_ENTITY) engine.GetScene().DestroyEntity(pile.entity);
	if (pile.mesh.IsValid()) engine.GetRenderer().DestroyMesh(pile.mesh);
	if (pile.body != kInvalidBody)
	{
		mPileBodies.erase(pile.body);
		engine.GetPhysics().RemoveBody(pile.body);
	}
	pile = Pile{};
}

int LooseBlocks::JoinWaiting(Engine& engine, Pile& pile)
{
	const PhysicsWorld& physics = engine.GetPhysics();
	int joined = 0;
	for (BodyId body : pile.pending)
	{
		// Skips blocks that moved, left or were taken since they started waiting.
		auto it = mIndex.find(body);
		if (it == mIndex.end()) continue;
		const size_t i = it->second;
		Block& b = mBlocks[i];
		if (!b.pending || b.pendingPile != pile.key) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, false);
		if (PileCellKey(p) != pile.key) { Unqueue(b); continue; }   // jittered into the next cell: waits there instead
		pile.blocks.push_back(MakeSettled(p, r, b.block, b.colour, b.town));
		++mSettledCount;
		RemoveAt(engine, i);
		++joined;
	}
	pile.pending.clear();
	pile.waiting = 0.0f;
	if (joined > 0) pile.dirty = true;
	return joined;
}

void LooseBlocks::FlushPiles(Engine& engine, bool joinWaiting)
{
	// Empty piles go (swap-and-pop, fixing the moved pile's indices).
	for (size_t i = 0; i < mPiles.size(); )
	{
		if (!mPiles[i].blocks.empty() || !mPiles[i].pending.empty()) { ++i; continue; }
		mPileIndex.erase(mPiles[i].key);
		DestroyPile(engine, mPiles[i]);
		if (i + 1 != mPiles.size())
		{
			mPiles[i] = std::move(mPiles.back());
			mPileIndex[mPiles[i].key] = static_cast<int>(i);
			if (mPiles[i].body != kInvalidBody) mPileBodies[mPiles[i].body] = static_cast<int>(i);
		}
		mPiles.pop_back();
	}

	using Clock = std::chrono::high_resolution_clock;
	const auto start = Clock::now();
	int builds = 0, blocks = 0;
	float meshMs = 0.0f, collisionMs = 0.0f;
	auto rebuild = [&](int i)
	{
		if (mPiles[i].blocks.empty()) { mPiles[i].dirty = false; return; }   // dropped at the next flush
		RebuildPile(engine, i, meshMs, collisionMs);
		++builds;
		blocks += static_cast<int>(mPiles[i].blocks.size());
	};

	// Blocks left these piles (taken, woken): rebuilt at once, or they'd show twice. Blocks
	// waiting to join ride along.
	for (size_t i = 0; i < mPiles.size(); ++i)
	{
		if (!mPiles[i].dirty) continue;
		if (joinWaiting && !mPiles[i].pending.empty()) JoinWaiting(engine, mPiles[i]);
		rebuild(static_cast<int>(i));
	}

	// Blocks waiting to join: each pile collects them for kPileBatchTime, so a heap coming to
	// rest rebuilds each pile once or twice instead of every frame. Longest waiting first,
	// within the frame's budget (they stay live meanwhile, so nothing shows missing).
	if (joinWaiting)
	{
		std::vector<std::pair<float, int>> ready;
		for (size_t i = 0; i < mPiles.size(); ++i)
			if (!mPiles[i].pending.empty() && mPiles[i].waiting >= cfg::kPileBatchTime) ready.push_back({ mPiles[i].waiting, static_cast<int>(i) });
		std::sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const auto& [waiting, i] : ready)
		{
			if (builds > 0 && std::chrono::duration<float, std::milli>(Clock::now() - start).count() >= cfg::kPileBuildBudgetMs) break;
			if (JoinWaiting(engine, mPiles[i]) > 0 || mPiles[i].dirty) rebuild(i);
		}
	}

	if (builds > 0)
	{
		mLastPileBuildMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
		mLastPileMeshMs = meshMs;
		mLastPileCollisionMs = collisionMs;
		mLastPileBuilds = builds;
		mLastPileBlocks = blocks;
	}
}

void LooseBlocks::CrumbleOverCap(Engine& engine)
{
	// The old rule, kept as a test flag: past the cap the oldest blocks crumble away. A few
	// per frame, so switching it on with a big pile doesn't spawn thousands of animations.
	for (int n = 0; n < 64 && Count() > cfg::kMaxLooseBlocks; ++n)
	{
		bool done = false;
		for (Block& old : mBlocks)
		{
			if (old.body != kInvalidBody) { StartLeaving(engine, old, false); done = true; break; }
		}
		if (!done)
		{
			for (size_t p = 0; p < mPiles.size() && !done; ++p)
			{
				if (mPiles[p].blocks.empty()) continue;
				TakeSettled(engine, static_cast<int>(p), 0, false);
				done = true;
			}
		}
		if (!done) break;
		++mCrumbled;
	}
}

void LooseBlocks::Update(Engine& engine, float dt, const XMFLOAT3& collector, const XMFLOAT3& feet)
{
	Scene& scene = engine.GetScene();
	const PhysicsWorld& physics = engine.GetPhysics();
	// Blocks never settle into a pile inside the player (a solid pile would trap them).
	const float clearRadius = cfg::kBodyWidth * 0.5f + 0.2f;
	auto insidePlayer = [&](const XMFLOAT3& p)
	{
		const float dx = p.x - feet.x, dz = p.z - feet.z;
		return dx * dx + dz * dz < clearRadius * clearRadius && p.y > feet.y - 0.2f && p.y < feet.y + cfg::kBodyHeight + 0.2f;
	};
	// A quaternion's dot product with another is the cosine of half the turn between them.
	const float stillTurnCos = std::cos(XMConvertToRadians(cfg::kBlockStillTurn) * 0.5f);

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
			// Fell out of the world (through a gap in the terrain): never lost, it drops back
			// onto the flat ground in front of the mine mouth.
			if (p.y < cfg::kBlockLostY)
			{
				const int n = mRescued++;
				const XMFLOAT3 at{ cfg::kSpawnX - 0.75f + (n % 6) * 0.3f, cfg::kGroundY + 1.5f + ((n / 36) % 10) * 0.3f, cfg::kSpawnZ + 1.0f + ((n / 6) % 6) * 0.3f };
				const Block rescued = b;
				RemoveAt(engine, i);
				SpawnLive(engine, rescued.block, rescued.colour, at, { 0, 0, 0, 1 }, { 0, 0, 0 }, { 0, 0, 0 }, rescued.town);
				continue;
			}
			// Staying put: measured from where it last moved to, so a block jittering in a
			// heap (whose body never sleeps) still counts as still.
			const float dx = p.x - b.anchor.x, dy = p.y - b.anchor.y, dz = p.z - b.anchor.z;
			const float turn = std::fabs(r.x * b.anchorRotation.x + r.y * b.anchorRotation.y + r.z * b.anchorRotation.z + r.w * b.anchorRotation.w);
			if (dx * dx + dy * dy + dz * dz > cfg::kBlockStillDrift * cfg::kBlockStillDrift || turn < stillTurnCos)
			{
				b.anchor = p;
				b.anchorRotation = r;
				b.still = 0.0f;
				Unqueue(b);
			}
			else
			{
				b.still += dt;
			}
			// Still long enough: waits for its pile's next rebuild, then joins it.
			if (mOptions.piles && !b.pending && b.still >= cfg::kPileSettleTime && !insidePlayer(p))
			{
				const int64_t key = PileCellKey(p);
				Pile& pile = mPiles[PileFor(engine, key)];
				if (pile.pending.empty()) pile.waiting = 0.0f;
				pile.pending.push_back(b.body);
				b.pending = true;
				b.pendingPile = key;
				++mPendingCount;
			}
			// Its body sleeps (anything landing on it wakes it again), so a heap at rest costs
			// little even before it joins the piles, or with piles switched off.
			if (b.still >= cfg::kBlockSleepAfter && !physics.IsSleeping(b.body)) engine.GetPhysics().Deactivate(b.body);
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
				const XMFLOAT3 to = b.toPoint ? b.to : collector;
				t.position = { b.from.x + (to.x - b.from.x) * e, b.from.y + (to.y - b.from.y) * e, b.from.z + (to.z - b.from.z) * e };
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
			RemoveAt(engine, i);
			continue;
		}
		++i;
	}
	for (Pile& pile : mPiles)
		if (!pile.pending.empty()) pile.waiting += dt;

	// Too many bodies at once (a huge cave-in or a test drop): the stillest blocks join their
	// piles early, so the physics stays within its limits. Nothing is lost.
	if (mOptions.piles && LiveCount() > cfg::kMaxLiveBlocks)
	{
		std::vector<std::pair<float, size_t>> stillest;
		for (size_t i = 0; i < mBlocks.size(); ++i)
		{
			const Block& b = mBlocks[i];
			if (b.body == kInvalidBody) continue;
			// Longest still first, then the slowest.
			const XMFLOAT3 v = physics.GetLinearVelocity(b.body);
			stillest.push_back({ b.still > 0.0f ? -b.still : v.x * v.x + v.y * v.y + v.z * v.z, i });
		}
		const size_t excess = static_cast<size_t>(LiveCount() - cfg::kMaxLiveBlocks);
		std::partial_sort(stillest.begin(), stillest.begin() + excess, stillest.end());
		std::vector<size_t> settle;
		for (size_t n = 0; n < excess; ++n) settle.push_back(stillest[n].second);
		std::sort(settle.begin(), settle.end(), std::greater<size_t>());   // highest first: swap-and-pop keeps the rest valid
		for (size_t i : settle)
		{
			Settle(engine, mBlocks[i]);
			RemoveAt(engine, i);
		}
	}

	if (mOptions.cap) CrumbleOverCap(engine);
	FlushPiles(engine, mOptions.piles);
}

void LooseBlocks::Wake(Engine& engine, const VoxelWorld::Box& box)
{
	if (box.Empty() || mSettledCount == 0) return;
	const float s = cfg::kVoxelSize;
	const float m = cfg::kPileWakeMargin;
	const XMFLOAT3 lo{ box.min[0] * s - m, box.min[1] * s - m, box.min[2] * s - m };
	const XMFLOAT3 hi{ (box.max[0] + 1) * s + m, (box.max[1] + 1) * s + m, (box.max[2] + 1) * s + m };
	// Blocks resting on the woken ones wake too, so nothing is left hanging: searched within
	// a few metres around the edit and further up (a tall heap on a mined floor).
	const XMFLOAT3 searchLo{ lo.x - 3.0f, lo.y - 1.0f, lo.z - 3.0f };
	const XMFLOAT3 searchHi{ hi.x + 3.0f, hi.y + 8.0f, hi.z + 3.0f };

	struct Candidate { int pile; int index; };
	std::unordered_map<uint64_t, std::vector<Candidate>> byVoxel;
	std::vector<Candidate> seeds;
	for (int cx = CellOf(searchLo.x); cx <= CellOf(searchHi.x); ++cx)
		for (int cy = CellOf(searchLo.y); cy <= CellOf(searchHi.y); ++cy)
			for (int cz = CellOf(searchLo.z); cz <= CellOf(searchHi.z); ++cz)
			{
				auto it = mPileIndex.find(PackCell(cx, cy, cz));
				if (it == mPileIndex.end()) continue;
				const Pile& pile = mPiles[it->second];
				for (int i = 0; i < static_cast<int>(pile.blocks.size()); ++i)
				{
					const XMFLOAT3& p = pile.blocks[i].position;
					byVoxel[VoxelKey(p)].push_back({ it->second, i });
					if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z) seeds.push_back({ it->second, i });
				}
			}
	if (seeds.empty()) return;

	auto id = [](const Candidate& c) { return (static_cast<uint64_t>(c.pile) << 32) | static_cast<uint32_t>(c.index); };
	std::unordered_set<uint64_t> woken;
	std::deque<Candidate> queue;
	for (const Candidate& c : seeds)
		if (woken.insert(id(c)).second) queue.push_back(c);
	while (!queue.empty())
	{
		const Candidate w = queue.front();
		queue.pop_front();
		const XMFLOAT3 wp = mPiles[w.pile].blocks[w.index].position;
		const int vx = static_cast<int>(std::floor(wp.x / s)), vy = static_cast<int>(std::floor(wp.y / s)), vz = static_cast<int>(std::floor(wp.z / s));
		for (int dx = -1; dx <= 1; ++dx)
			for (int dy = 0; dy <= 2; ++dy)
				for (int dz = -1; dz <= 1; ++dz)
				{
					auto it = byVoxel.find(static_cast<uint64_t>(PackCell(vx + dx, vy + dy, vz + dz)));
					if (it == byVoxel.end()) continue;
					for (const Candidate& c : it->second)
					{
						const XMFLOAT3& cp = mPiles[c.pile].blocks[c.index].position;
						const float hx = cp.x - wp.x, hz = cp.z - wp.z;
						if (cp.y <= wp.y + 0.05f || hx * hx + hz * hz > 0.35f * 0.35f) continue;
						if (woken.insert(id(c)).second) queue.push_back(c);
					}
				}
	}

	// Out of their piles (highest index first, so the swap-and-pop keeps the others valid),
	// then the piles are rebuilt before the new live bodies are simulated next to them.
	std::vector<Candidate> order;
	order.reserve(woken.size());
	for (uint64_t key : woken) order.push_back({ static_cast<int>(key >> 32), static_cast<int>(key & 0xFFFFFFFFu) });
	std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) { return a.pile != b.pile ? a.pile < b.pile : a.index > b.index; });
	std::vector<Settled> live;
	live.reserve(order.size());
	for (const Candidate& c : order)
	{
		Pile& pile = mPiles[c.pile];
		live.push_back(pile.blocks[c.index]);
		pile.blocks[c.index] = pile.blocks.back();
		pile.blocks.pop_back();
		pile.dirty = true;
		--mSettledCount;
	}
	FlushPiles(engine, false);
	for (const Settled& b : live) SpawnLive(engine, b.block, b.colour, b.position, b.rotation, { 0, 0, 0 }, { 0, 0, 0 }, b.town);
}

std::optional<float> LooseBlocks::RaycastPiles(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance) const
{
	if (mSettledCount == 0) return std::nullopt;
	const XMFLOAT3 end{ origin.x + direction.x * maxDistance, origin.y + direction.y * maxDistance, origin.z + direction.z * maxDistance };
	const float half = BlockHalf();
	const XMVECTOR o = XMLoadFloat3(&origin);
	const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&direction));
	std::optional<float> best;
	for (int cx = CellOf(std::min(origin.x, end.x)); cx <= CellOf(std::max(origin.x, end.x)); ++cx)
		for (int cy = CellOf(std::min(origin.y, end.y)); cy <= CellOf(std::max(origin.y, end.y)); ++cy)
			for (int cz = CellOf(std::min(origin.z, end.z)); cz <= CellOf(std::max(origin.z, end.z)); ++cz)
			{
				auto it = mPileIndex.find(PackCell(cx, cy, cz));
				if (it == mPileIndex.end()) continue;
				for (const Settled& s : mPiles[it->second].blocks)
				{
					float distance = 0.0f;
					if (BoundingOrientedBox(s.position, { half, half, half }, s.rotation).Intersects(o, d, distance) && distance <= maxDistance && (!best || distance < *best))
						best = distance;
				}
			}
	return best;
}

std::vector<LooseBlocks::Ref> LooseBlocks::InReach(Engine& engine, const XMFLOAT3& point, float radius, int max) const
{
	const PhysicsWorld& physics = engine.GetPhysics();
	std::vector<std::pair<float, Ref>> nearby;
	auto consider = [&](const XMFLOAT3& p, const Ref& ref)
	{
		const float dx = p.x - point.x, dy = p.y - point.y, dz = p.z - point.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 <= radius * radius) nearby.push_back({ d2, ref });
	};
	for (const Block& b : mBlocks)
	{
		if (b.body == kInvalidBody || b.town) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, true);
		consider(p, Ref{ b.body, -1, -1 });
	}
	for (int cx = CellOf(point.x - radius); cx <= CellOf(point.x + radius); ++cx)
		for (int cy = CellOf(point.y - radius); cy <= CellOf(point.y + radius); ++cy)
			for (int cz = CellOf(point.z - radius); cz <= CellOf(point.z + radius); ++cz)
			{
				auto it = mPileIndex.find(PackCell(cx, cy, cz));
				if (it == mPileIndex.end()) continue;
				const Pile& pile = mPiles[it->second];
				for (int i = 0; i < static_cast<int>(pile.blocks.size()); ++i)
					if (!pile.blocks[i].town) consider(pile.blocks[i].position, Ref{ kInvalidBody, it->second, i });
			}
	std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	if (static_cast<int>(nearby.size()) > max) nearby.resize(max);
	std::vector<Ref> out;
	out.reserve(nearby.size());
	for (const auto& n : nearby) out.push_back(n.second);
	return out;
}

int LooseBlocks::Collect(Engine& engine, const std::vector<Ref>& refs, Counts& taken, const XMFLOAT3* to)
{
	int count = 0;
	std::vector<Ref> settled;
	for (const Ref& ref : refs)
	{
		if (ref.body == kInvalidBody) { settled.push_back(ref); continue; }
		auto it = mIndex.find(ref.body);
		if (it == mIndex.end()) continue;
		Block& b = mBlocks[it->second];
		++taken[static_cast<int>(b.block)];
		StartLeaving(engine, b, true);
		++count;
	}
	// Highest index first within each pile, so taking one doesn't move the others.
	std::sort(settled.begin(), settled.end(), [](const Ref& a, const Ref& b) { return a.pile != b.pile ? a.pile < b.pile : a.index > b.index; });
	for (const Ref& ref : settled)
	{
		if (ref.pile < 0 || ref.pile >= static_cast<int>(mPiles.size()) || ref.index >= static_cast<int>(mPiles[ref.pile].blocks.size())) continue;
		++taken[static_cast<int>(mPiles[ref.pile].blocks[ref.index].block)];
		TakeSettled(engine, ref.pile, ref.index, true);
		++count;
	}
	if (to)
	{
		// Towards the barrow: every block that started leaving in this call (leaving is still 0).
		for (Block& b : mBlocks)
			if (b.body == kInvalidBody && b.collected && b.leaving == 0.0f) { b.toPoint = true; b.to = *to; }
	}
	return count;
}

void LooseBlocks::Outline(Engine& engine, const std::vector<Ref>& refs, uint32_t color) const
{
	const PhysicsWorld& physics = engine.GetPhysics();
	const float half = cfg::kVoxelSize * 0.52f;
	for (const Ref& ref : refs)
	{
		XMFLOAT3 p;
		XMFLOAT4 r;
		if (ref.body != kInvalidBody)
		{
			if (mIndex.count(ref.body) == 0) continue;
			physics.GetTransform(ref.body, p, r, true);
		}
		else
		{
			if (ref.pile < 0 || ref.pile >= static_cast<int>(mPiles.size()) || ref.index >= static_cast<int>(mPiles[ref.pile].blocks.size())) continue;
			p = mPiles[ref.pile].blocks[ref.index].position;
			r = mPiles[ref.pile].blocks[ref.index].rotation;
		}
		engine.GetRenderer().Debug().Box(BoundingOrientedBox(p, { half, half, half }, r), color);
	}
}

void LooseBlocks::SetOptions(Engine& engine, const Options& options)
{
	PhysicsWorld& physics = engine.GetPhysics();
	if (options.solidBlocks != mOptions.solidBlocks)
		for (const auto& [body, index] : mIndex) physics.SetIgnoreCharacters(body, !options.solidBlocks);
	if (options.solidPiles != mOptions.solidPiles)
		for (const auto& [body, index] : mPileBodies) physics.SetIgnoreCharacters(body, !options.solidPiles);
	// Turning piles off only stops new blocks settling: the piles already there stay (waking
	// them all at once could run out of physics bodies).
	mOptions = options;
}

void LooseBlocks::Unqueue(Block& b)
{
	if (!b.pending) return;
	b.pending = false;
	--mPendingCount;
}

void LooseBlocks::RemoveAt(Engine& engine, size_t i)
{
	Release(engine, mBlocks[i]);
	if (i + 1 != mBlocks.size())
	{
		mBlocks[i] = std::move(mBlocks.back());
		if (mBlocks[i].body != kInvalidBody) mIndex[mBlocks[i].body] = i;
	}
	mBlocks.pop_back();
}

void LooseBlocks::Release(Engine& engine, Block& b)
{
	Unqueue(b);
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
	for (Pile& pile : mPiles) DestroyPile(engine, pile);
	mPiles.clear();
	mPileIndex.clear();
	mPileBodies.clear();
	mSettledCount = 0;
	mPendingCount = 0;
	mYardPaid.fill(0);
	mYardCount = 0;
}

void LooseBlocks::Shutdown(Engine& engine)
{
	Clear(engine);
	for (auto& [key, mesh] : mMeshes) engine.GetRenderer().DestroyMesh(mesh);
	mMeshes.clear();
}

int LooseBlocks::TakeFromYard(Engine& engine, int count)
{
	// The town's blocks, highest first.
	std::vector<std::pair<float, std::pair<int, int>>> town;
	for (int p = 0; p < static_cast<int>(mPiles.size()); ++p)
		for (int i = 0; i < static_cast<int>(mPiles[p].blocks.size()); ++i)
			if (mPiles[p].blocks[i].town) town.push_back({ -mPiles[p].blocks[i].position.y, { p, i } });
	if (town.empty() || count <= 0) return 0;
	const size_t n = std::min(town.size(), static_cast<size_t>(count));
	std::partial_sort(town.begin(), town.begin() + n, town.end());
	std::vector<std::pair<int, int>> take;
	for (size_t k = 0; k < n; ++k) take.push_back(town[k].second);
	// Highest index first within each pile, so removing one doesn't move the others.
	std::sort(take.begin(), take.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second > b.second; });
	for (const auto& [p, i] : take)
	{
		Pile& pile = mPiles[p];
		pile.blocks[i] = pile.blocks.back();
		pile.blocks.pop_back();
		pile.dirty = true;
		--mSettledCount;
		--mYardCount;
	}
	FlushPiles(engine, false);
	return static_cast<int>(n);
}

void LooseBlocks::Push(Engine& engine, const BoundingBox& area, const XMFLOAT3& velocity)
{
	PhysicsWorld& physics = engine.GetPhysics();
	const float s = cfg::kVoxelSize;
	const float mass = cfg::kRockDensity * s * s * s;
	for (Block& b : mBlocks)
	{
		if (b.body == kInvalidBody) continue;
		XMFLOAT3 p;
		XMFLOAT4 r;
		physics.GetTransform(b.body, p, r, false);
		if (area.Contains(XMLoadFloat3(&p)) == DISJOINT) continue;
		const XMFLOAT3 v = physics.GetLinearVelocity(b.body);
		physics.AddImpulse(b.body, { (velocity.x - v.x) * mass * 0.5f, 0.0f, (velocity.z - v.z) * mass * 0.5f }, p);
		b.still = 0.0f;
		Unqueue(b);
	}
}
