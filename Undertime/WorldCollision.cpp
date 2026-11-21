#include "WorldCollision.h"
#include <algorithm>
#include <chrono>

using namespace DirectX;

namespace
{
	constexpr int kChunkVoxels = cfg::kCollisionChunk * cfg::kVoxelsPerMeter;

	float MsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
}

void WorldCollision::Build(PhysicsWorld& physics, const VoxelWorld& world)
{
	const auto start = std::chrono::steady_clock::now();
	Destroy(physics);

	mCount[0] = (cfg::kVoxX + kChunkVoxels - 1) / kChunkVoxels;
	mCount[1] = (cfg::kVoxY + kChunkVoxels - 1) / kChunkVoxels;
	mCount[2] = (cfg::kVoxZ + kChunkVoxels - 1) / kChunkVoxels;
	const int total = mCount[0] * mCount[1] * mCount[2];
	mChunks.assign(total, kInvalidBody);
	mDirty.assign(total, 0);
	mDirtyList.clear();
	for (int i = 0; i < total; ++i)
	{
		BuildChunk(physics, world, i);
	}

	// Everything outside the grid that VoxelWorld::IsSolid treats as solid.
	const float cx = cfg::kCenterX + 0.5f;
	const float ext = cfg::kOutsideExtent;
	const float gx = static_cast<float>(cfg::kGridX), gy = static_cast<float>(cfg::kGridY), gz = static_cast<float>(cfg::kGridZ);
	const float far = 600.0f, tall = 160.0f;
	auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1)
	{
		mStatics.push_back(physics.CreateStaticBox({ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f },
			{ (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f }));
	};
	box(0.0f, -4.0f, 0.0f, gx, 0.0f, far);                                           // below the grid
	box(0.0f, 0.0f, gz, gx, gy, far);                                                // behind the far wall
	if (mValleyOutside)
	{
		// The valley's terrain is its own collision; just walls at the world's edge.
		const float x0 = cfg::kValleyWorld[0], z0 = cfg::kValleyWorld[1], x1 = cfg::kValleyWorld[2], z1 = cfg::kValleyWorld[3];
		box(x0 - 5.0f, -50.0f, z0 - 5.0f, x0, 400.0f, z1 + 5.0f);
		box(x1, -50.0f, z0 - 5.0f, x1 + 5.0f, 400.0f, z1 + 5.0f);
		box(x0, -50.0f, z0 - 5.0f, x1, 400.0f, z0);
		box(x0, -50.0f, z1, x1, 400.0f, z1 + 5.0f);
		mBuildMs = MsSince(start);
		return;
	}
	box(cx - ext - 5.0f, -4.0f, -ext - 5.0f, cx + ext + 5.0f, cfg::kGroundY, 0.0f);   // ground in front of the mountain
	box(-far, -4.0f, 0.0f, 0.0f, tall, far);                                         // mountain left of the grid
	box(gx, -4.0f, 0.0f, far, tall, far);                                            // mountain right of the grid
	box(0.0f, gy, 0.0f, gx, tall, far);                                              // above the grid
	box(-far, -4.0f, -far, cx - ext, tall, 0.0f);                                    // play-area walls
	box(cx + ext, -4.0f, -far, far, tall, 0.0f);
	box(cx - ext, -4.0f, -far, cx + ext, tall, -ext);
	mBuildMs = MsSince(start);
}

void WorldCollision::BuildChunk(PhysicsWorld& physics, const VoxelWorld& world, int chunk)
{
	physics.RemoveBody(mChunks[chunk]);
	mChunks[chunk] = kInvalidBody;

	const int cx = chunk % mCount[0];
	const int cy = (chunk / mCount[0]) % mCount[1];
	const int cz = chunk / (mCount[0] * mCount[1]);
	const int lo[3] = { cx * kChunkVoxels, cy * kChunkVoxels, cz * kChunkVoxels };
	const int hi[3] = { lo[0] + kChunkVoxels, lo[1] + kChunkVoxels, lo[2] + kChunkVoxels };

	// Most chunks are buried in solid rock: skip them without building a mask.
	if (!world.TouchesAir(lo, hi)) return;

	mScratch.clear();
	world.BuildCollisionTriangles(lo, hi, mScratch);
	if (!mScratch.empty())
	{
		mChunks[chunk] = physics.CreateStaticMesh(mScratch);
	}
}

void WorldCollision::MarkDirty(const VoxelWorld::Box& box)
{
	if (box.Empty() || mChunks.empty()) return;
	// A voxel change exposes or hides faces of its neighbours too.
	int lo[3], hi[3];
	for (int a = 0; a < 3; ++a)
	{
		lo[a] = std::max(box.min[a] - 1, 0) / kChunkVoxels;
		hi[a] = std::min((box.max[a] + 1) / kChunkVoxels, mCount[a] - 1);
	}
	for (int z = lo[2]; z <= hi[2]; ++z)
		for (int y = lo[1]; y <= hi[1]; ++y)
			for (int x = lo[0]; x <= hi[0]; ++x)
			{
				const int i = ChunkIndex(x, y, z);
				if (!mDirty[i])
				{
					mDirty[i] = 1;
					mDirtyList.push_back(i);
				}
			}
}

void WorldCollision::Flush(PhysicsWorld& physics, const VoxelWorld& world)
{
	if (mDirtyList.empty()) return;
	const auto start = std::chrono::steady_clock::now();
	for (int chunk : mDirtyList)
	{
		BuildChunk(physics, world, chunk);
		mDirty[chunk] = 0;
	}
	mLastFlushChunks = static_cast<int>(mDirtyList.size());
	mDirtyList.clear();
	mLastFlushMs = MsSince(start);
}

void WorldCollision::Destroy(PhysicsWorld& physics)
{
	for (BodyId id : mChunks) physics.RemoveBody(id);
	for (BodyId id : mStatics) physics.RemoveBody(id);
	mChunks.clear();
	mStatics.clear();
	mDirty.clear();
	mDirtyList.clear();
}
