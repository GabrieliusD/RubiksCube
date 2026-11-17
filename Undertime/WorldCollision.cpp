#include "WorldCollision.h"
#include <algorithm>
#include <chrono>

using namespace DirectX;

namespace
{
	constexpr int kSlabVoxels = cfg::kPhysicsSlabDepth * cfg::kVoxelsPerMeter;
}

void WorldCollision::Build(PhysicsWorld& physics, const VoxelWorld& world)
{
	Destroy(physics);

	const int slabs = (cfg::kVoxZ + kSlabVoxels - 1) / kSlabVoxels;
	mSlabs.assign(slabs, kInvalidBody);
	for (int i = 0; i < slabs; ++i)
	{
		BuildSlab(physics, world, i);
	}

	// Everything outside the grid that VoxelWorld::IsSolid treats as solid.
	const float cx = cfg::kCenterX + 0.5f;
	const float ext = cfg::kOutsideExtent;
	const float gx = static_cast<float>(cfg::kGridX), gy = static_cast<float>(cfg::kGridY), gz = static_cast<float>(cfg::kGridZ);
	const float far = 400.0f, tall = 120.0f;
	auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1)
	{
		mStatics.push_back(physics.CreateStaticBox({ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f },
			{ (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f }));
	};
	box(cx - ext - 5.0f, -4.0f, -ext - 5.0f, cx + ext + 5.0f, cfg::kGroundY, 0.0f);   // ground in front of the mountain
	box(-far, -4.0f, 0.0f, 0.0f, tall, far);                                         // mountain left of the grid
	box(gx, -4.0f, 0.0f, far, tall, far);                                            // mountain right of the grid
	box(0.0f, gy, 0.0f, gx, tall, far);                                              // above the grid
	box(0.0f, -4.0f, 0.0f, gx, 0.0f, far);                                           // below the grid
	box(0.0f, 0.0f, gz, gx, gy, far);                                                // behind the far wall
	box(-far, -4.0f, -far, cx - ext, tall, 0.0f);                                    // play-area walls
	box(cx + ext, -4.0f, -far, far, tall, 0.0f);
	box(cx - ext, -4.0f, -far, cx + ext, tall, -ext);
}

void WorldCollision::BuildSlab(PhysicsWorld& physics, const VoxelWorld& world, int slab)
{
	physics.RemoveBody(mSlabs[slab]);
	mSlabs[slab] = kInvalidBody;
	mScratch.clear();
	world.BuildCollisionTriangles(slab * kSlabVoxels, (slab + 1) * kSlabVoxels, mScratch);
	if (!mScratch.empty())
	{
		mSlabs[slab] = physics.CreateStaticMesh(mScratch);
	}
}

void WorldCollision::Rebuild(PhysicsWorld& physics, const VoxelWorld& world, int zMin, int zMax)
{
	const auto start = std::chrono::steady_clock::now();
	// A voxel change exposes or hides faces of its neighbours too.
	const int first = std::max(zMin - 1, 0) / kSlabVoxels;
	const int last = std::min(zMax + 1, cfg::kVoxZ - 1) / kSlabVoxels;
	for (int slab = first; slab <= last && slab < static_cast<int>(mSlabs.size()); ++slab)
	{
		BuildSlab(physics, world, slab);
	}
	mLastRebuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void WorldCollision::Destroy(PhysicsWorld& physics)
{
	for (BodyId id : mSlabs) physics.RemoveBody(id);
	for (BodyId id : mStatics) physics.RemoveBody(id);
	mSlabs.clear();
	mStatics.clear();
}
