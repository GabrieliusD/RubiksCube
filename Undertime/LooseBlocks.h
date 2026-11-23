#pragma once
#include "VoxelWorld.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <DirectXMath.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Where voxels knocked out of a rock face go: each one is mirrored through the face plane
// into the air in front of it (so the pile starts outside the hole instead of jammed in
// it, and no two blocks overlap). A voxel whose mirrored cell isn't free stays in its own
// (now empty) cell. `axis`/`sign` are the face normal (+x = axis 0, sign +1); `faceVoxel`
// is the coordinate of the voxel that was hit along that axis. Pure, for tests.
std::vector<DirectX::XMINT3> MirrorOutOfFace(const std::vector<VoxelWorld::RemovedVoxel>& voxels, int axis, int sign, int faceVoxel,
	const std::function<bool(int, int, int)>& isFree);

// The pile cell a point falls in (cfg::kPileCell metres per side), packed into one key. Pure.
int64_t PileCellKey(const DirectX::XMFLOAT3& position);

// Every mined voxel becomes one loose block. Nothing is ever lost (unless the old cap is
// switched on): a block is
//  - live while it falls and tumbles: its own small physics body and entity;
//  - settled once it has slept a while: a record in the pile of its cell. Each pile cell
//    is one merged mesh and one static collision mesh, so resting blocks cost nothing each.
// Settled blocks turn live again when the rock under or beside them is mined (Wake). The
// shovel takes blocks of either kind into the backpack. Blocks never go back into the world.
class LooseBlocks
{
public:
	using Counts = std::array<int, static_cast<int>(cfg::Block::Count)>;

	// Switched while playing (the editor's Game panel) to try each design decision.
	struct Options
	{
		bool cap = cfg::kLooseCapDefault;            // past kMaxLooseBlocks the oldest crumble away
		bool piles = cfg::kLoosePilesDefault;        // resting blocks settle into piles
		bool solidBlocks = cfg::kSolidBlocksDefault; // live blocks collide with the player
		bool solidPiles = cfg::kSolidPilesDefault;   // piles collide with the player
	};

	// A block the shovel can take: a live one (`body`) or a settled one (`pile`, `index`).
	// Only valid until the blocks next change.
	struct Ref
	{
		BodyId body = kInvalidBody;
		int pile = -1;
		int index = -1;
	};

	void Initialize(MaterialHandle material);
	void Shutdown(Engine& engine);   // also frees the shared cube meshes

	// One block per voxel, centred at `center` (world metres).
	void Spawn(Engine& engine, cfg::Block block, int colorX, int colorY, int colorZ, const DirectX::XMFLOAT3& center,
		const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& velocity, const DirectX::XMFLOAT3& spin);
	// World voxel cells holding a block's centre right now (for placing new ones clear of the pile).
	std::function<bool(int, int, int)> OccupiedCells(Engine& engine) const;

	// Animates leaving blocks, settles resting ones into piles (never inside the player's
	// capsule, `playerFeet` with the body's radius and height) and rebuilds changed piles.
	void Update(Engine& engine, float deltaTime, const DirectX::XMFLOAT3& collector, const DirectX::XMFLOAT3& playerFeet);

	// The rock in `box` (world voxel cells) changed: settled blocks within kPileWakeMargin of
	// it, and everything resting on those, turn live again and fall if nothing holds them.
	void Wake(Engine& engine, const VoxelWorld::Box& box);

	// The nearest settled block along a ray (for aiming the shovel, whatever the piles' layer).
	std::optional<float> RaycastPiles(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float maxDistance) const;

	// The blocks the shovel would take at `point`: nearest first, at most `max`.
	std::vector<Ref> InReach(Engine& engine, const DirectX::XMFLOAT3& point, float radius, int max) const;
	// Takes those blocks (they fly into the backpack); adds them per type to `taken`.
	int Collect(Engine& engine, const std::vector<Ref>& blocks, Counts& taken);
	void Outline(Engine& engine, const std::vector<Ref>& blocks, uint32_t color) const;

	bool Owns(BodyId body) const { return mIndex.count(body) != 0 || mPileBodies.count(body) != 0; }
	void Clear(Engine& engine);

	const Options& GetOptions() const { return mOptions; }
	void SetOptions(Engine& engine, const Options& options);

	int Count() const { return LiveCount() + mSettledCount; }   // every block lying around
	int LiveCount() const { return static_cast<int>(mIndex.size()); }
	int SettledCount() const { return mSettledCount; }
	int PileCount() const { return static_cast<int>(mPiles.size()); }
	int Crumbled() const { return mCrumbled; }
	int Rescued() const { return mRescued; }   // fell out of the world and were put back at the mine mouth
	int PendingCount() const { return mPendingCount; }   // still blocks waiting for their pile's rebuild
	// The last frame that rebuilt piles: total, and the parts spent on the mesh and the collision.
	float LastPileBuildMs() const { return mLastPileBuildMs; }
	float LastPileMeshMs() const { return mLastPileMeshMs; }
	float LastPileCollisionMs() const { return mLastPileCollisionMs; }
	int LastPileBuilds() const { return mLastPileBuilds; }
	int LastPileBlocks() const { return mLastPileBlocks; }   // blocks in the piles that frame rebuilt

private:
	struct Block
	{
		Entity entity = INVALID_ENTITY;
		BodyId body = kInvalidBody;   // kInvalidBody once it's flying to the backpack or crumbling
		cfg::Block block = cfg::Block::Stone;
		uint16_t colour = 0;          // quantised colour (MeshFor)
		float age = 0.0f;
		float still = 0.0f;           // seconds it has stayed put (within kBlockStillDrift / kBlockStillTurn of `anchor`)
		DirectX::XMFLOAT3 anchor{ 0, 0, 0 };
		DirectX::XMFLOAT4 anchorRotation{ 0, 0, 0, 1 };
		int64_t pendingPile = 0;      // pile cell it waits to join
		bool pending = false;         // waiting in that pile's queue (still live until the rebuild)
		float leaving = -1.0f;        // >= 0: seconds into the scoop / crumble animation
		bool collected = false;       // leaving towards the player (else crumbling where it is)
		DirectX::XMFLOAT3 from{ 0, 0, 0 };
	};

	struct Settled
	{
		DirectX::XMFLOAT3 position{ 0, 0, 0 };
		DirectX::XMFLOAT4 rotation{ 0, 0, 0, 1 };
		cfg::Block block = cfg::Block::Stone;
		uint16_t colour = 0;
	};

	struct Pile
	{
		int64_t key = 0;
		std::vector<Settled> blocks;
		Entity entity = INVALID_ENTITY;
		MeshHandle mesh;
		BodyId body = kInvalidBody;
		bool dirty = true;            // blocks left: rebuild this frame
		std::vector<BodyId> pending;  // live blocks waiting to join at the next rebuild
		float waiting = 0.0f;         // seconds since the first of them started waiting
	};

	static uint16_t ColourKey(cfg::Block block, int x, int y, int z);
	MeshHandle MeshFor(Engine& engine, uint16_t colour);
	void SpawnLive(Engine& engine, cfg::Block block, uint16_t colour, const DirectX::XMFLOAT3& center,
		const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& velocity, const DirectX::XMFLOAT3& spin);
	void StartLeaving(Engine& engine, Block& b, bool collected);
	// Takes a settled block out of its pile into a leaving (animated, bodiless) block.
	void TakeSettled(Engine& engine, int pile, int index, bool collected);
	void Settle(Engine& engine, const Block& b);
	void CrumbleOverCap(Engine& engine);
	int PileFor(Engine& engine, int64_t key);
	void RebuildPile(Engine& engine, int index, float& meshMs, float& collisionMs);
	// Takes a block out of its pile's waiting list (it moved, left or was taken).
	void Unqueue(Block& b);
	void DestroyPile(Engine& engine, Pile& pile);
	// Drops empty piles and rebuilds changed ones: at once where blocks left, within
	// kPileBuildBudgetMs where blocks are waiting to join (unless `joinWaiting` is false).
	void FlushPiles(Engine& engine, bool joinWaiting);
	// Moves a pile's waiting blocks into it (their bodies and entities go).
	int JoinWaiting(Engine& engine, Pile& pile);
	void Release(Engine& engine, Block& b);
	// Releases mBlocks[i] and swap-and-pops it, fixing the index of the block that moved.
	void RemoveAt(Engine& engine, size_t i);

	Options mOptions;
	std::vector<Block> mBlocks;
	std::unordered_map<BodyId, size_t> mIndex;    // body -> index in mBlocks (only blocks with a body)
	std::vector<Pile> mPiles;
	std::unordered_map<int64_t, int> mPileIndex;  // cell key -> index in mPiles
	std::unordered_map<BodyId, int> mPileBodies;  // pile collision body -> index in mPiles
	std::unordered_map<uint32_t, MeshHandle> mMeshes;   // one cube mesh per colour
	MaterialHandle mMaterial;
	int mSettledCount = 0;
	int mPendingCount = 0;
	int mCrumbled = 0;
	int mRescued = 0;
	float mLastPileBuildMs = 0.0f;
	float mLastPileMeshMs = 0.0f;
	float mLastPileCollisionMs = 0.0f;
	int mLastPileBuilds = 0;
	int mLastPileBlocks = 0;
};
