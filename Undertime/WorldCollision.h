#pragma once
#include "VoxelWorld.h"
#include <Physics/PhysicsWorld.h>
#include <vector>

// Static physics for the world: the voxel grid as greedy-meshed slabs (rebuilt when they
// change) plus boxes for everything outside the grid (ground, mountain, play-area walls).
class WorldCollision
{
public:
	void Build(PhysicsWorld& physics, const VoxelWorld& world);
	// Rebuilds the slabs whose faces can change when voxels with z in [zMin, zMax] change.
	void Rebuild(PhysicsWorld& physics, const VoxelWorld& world, int voxelZMin, int voxelZMax);
	void Destroy(PhysicsWorld& physics);

	float LastRebuildMs() const { return mLastRebuildMs; }
	int SlabCount() const { return static_cast<int>(mSlabs.size()); }

private:
	void BuildSlab(PhysicsWorld& physics, const VoxelWorld& world, int slab);

	std::vector<BodyId> mSlabs;
	std::vector<BodyId> mStatics;
	std::vector<DirectX::XMFLOAT3> mScratch;
	float mLastRebuildMs = 0.0f;
};
