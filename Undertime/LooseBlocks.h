#pragma once
#include "VoxelWorld.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <DirectXMath.h>
#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// Where voxels knocked out of a rock face go: each one is mirrored through the face plane
// into the air in front of it (so the pile starts outside the hole instead of jammed in
// it, and no two blocks overlap). A voxel whose mirrored cell isn't free stays in its own
// (now empty) cell. `axis`/`sign` are the face normal (+x = axis 0, sign +1); `faceVoxel`
// is the coordinate of the voxel that was hit along that axis. Pure, for tests.
std::vector<DirectX::XMINT3> MirrorOutOfFace(const std::vector<VoxelWorld::RemovedVoxel>& voxels, int axis, int sign, int faceVoxel,
	const std::function<bool(int, int, int)>& isFree);

// Every mined voxel becomes one loose block: a small cube with its own physics body on the
// debris layer (it tumbles and piles up against the world, boulders and other blocks, but
// passes through the player). Blocks never go back into the world; the shovel scoops them
// into the backpack.
class LooseBlocks
{
public:
	using Counts = std::array<int, static_cast<int>(cfg::Block::Count)>;

	void Initialize(MaterialHandle material);
	void Shutdown(Engine& engine);   // also frees the shared cube meshes

	// One block per voxel, centred at `center` (world metres).
	void Spawn(Engine& engine, cfg::Block block, int colorX, int colorY, int colorZ, const DirectX::XMFLOAT3& center,
		const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& velocity, const DirectX::XMFLOAT3& spin);
	// World voxel cells holding a block's centre right now (for placing new ones clear of the pile).
	std::function<bool(int, int, int)> OccupiedCells(Engine& engine) const;

	void Update(Engine& engine, float deltaTime, const DirectX::XMFLOAT3& collector);

	// The blocks the shovel would take at `point`: nearest first, at most `max`.
	std::vector<BodyId> InReach(Engine& engine, const DirectX::XMFLOAT3& point, float radius, int max) const;
	// Takes those blocks (they fly into the backpack); adds them per type to `taken`.
	int Collect(Engine& engine, const std::vector<BodyId>& bodies, Counts& taken);
	void Outline(Engine& engine, const std::vector<BodyId>& bodies, uint32_t color) const;

	bool Owns(BodyId body) const { return mIndex.count(body) != 0; }
	void Clear(Engine& engine);

	int Count() const { return static_cast<int>(mIndex.size()); }
	int Crumbled() const { return mCrumbled; }

private:
	struct Block
	{
		Entity entity = INVALID_ENTITY;
		BodyId body = kInvalidBody;   // kInvalidBody once it's flying to the backpack or crumbling
		cfg::Block block = cfg::Block::Stone;
		float age = 0.0f;
		float leaving = -1.0f;        // >= 0: seconds into the scoop / crumble animation
		bool collected = false;       // leaving towards the player (else crumbling where it is)
		DirectX::XMFLOAT3 from{ 0, 0, 0 };
	};

	MeshHandle MeshFor(Engine& engine, cfg::Block block, int x, int y, int z);
	void StartLeaving(Engine& engine, Block& b, bool collected);
	void Release(Engine& engine, Block& b);

	std::vector<Block> mBlocks;
	std::unordered_map<BodyId, size_t> mIndex;    // body -> index in mBlocks (only blocks with a body)
	std::unordered_map<uint32_t, MeshHandle> mMeshes;   // one cube mesh per colour
	MaterialHandle mMaterial;
	int mCrumbled = 0;
};
