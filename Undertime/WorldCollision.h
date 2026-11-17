#pragma once
#include "VoxelWorld.h"
#include <Physics/PhysicsWorld.h>
#include <vector>

// Static physics for the world: the voxel grid as greedy-meshed 3D chunks
// (cfg::kCollisionChunk metres per side) plus boxes for everything outside the grid
// (ground, mountain, play-area walls).
//
// Edits only mark chunks dirty; Flush() rebuilds each dirty chunk once, so a cave-in that
// touches the same chunk many times in one frame costs a single rebuild.
class WorldCollision
{
public:
	void Build(PhysicsWorld& physics, const VoxelWorld& world);
	// Marks the chunks whose faces can change when voxels in `box` change (box grown by one).
	void MarkDirty(const VoxelWorld::Box& box);
	void Flush(PhysicsWorld& physics, const VoxelWorld& world);
	void Destroy(PhysicsWorld& physics);

	float LastFlushMs() const { return mLastFlushMs; }
	int LastFlushChunks() const { return mLastFlushChunks; }
	float BuildMs() const { return mBuildMs; }
	int ChunkCount() const { return static_cast<int>(mChunks.size()); }

private:
	void BuildChunk(PhysicsWorld& physics, const VoxelWorld& world, int chunk);
	int ChunkIndex(int cx, int cy, int cz) const { return cx + mCount[0] * (cy + mCount[1] * cz); }

	int mCount[3] = { 0, 0, 0 };
	std::vector<BodyId> mChunks;
	std::vector<uint8_t> mDirty;
	std::vector<int> mDirtyList;
	std::vector<BodyId> mStatics;
	std::vector<DirectX::XMFLOAT3> mScratch;
	float mLastFlushMs = 0.0f;
	int mLastFlushChunks = 0;
	float mBuildMs = 0.0f;
};
