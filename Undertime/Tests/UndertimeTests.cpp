// Tests for UNDERTIME's pure voxel/physics helpers (no GPU, no physics simulation).
#include "../VoxelWorld.h"
#include "../Fragments.h"
#include "../Structure.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace DirectX;
using cfg::Block;

namespace
{
	int gFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gFailures;
	}

	FragmentVoxels RandomFragment(std::mt19937& rng, int n, float fill)
	{
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		FragmentVoxels f;
		f.size = { n, n + 1, n + 2 };
		f.voxels.resize(static_cast<size_t>(f.size.x) * f.size.y * f.size.z);
		for (auto& v : f.voxels) v = unit(rng) < fill ? Block::Stone : Block::Air;
		return f;
	}

	bool BoxesCoverExactly(const FragmentVoxels& f, const std::vector<PhysicsBox>& boxes)
	{
		const float s = cfg::kVoxelSize;
		for (int z = 0; z < f.size.z; ++z)
		for (int y = 0; y < f.size.y; ++y)
		for (int x = 0; x < f.size.x; ++x)
		{
			const XMFLOAT3 c{ (x + 0.5f) * s, (y + 0.5f) * s, (z + 0.5f) * s };
			int inside = 0;
			for (const PhysicsBox& b : boxes)
			{
				if (std::fabs(c.x - b.center.x) < b.halfExtents.x && std::fabs(c.y - b.center.y) < b.halfExtents.y && std::fabs(c.z - b.center.z) < b.halfExtents.z)
					++inside;
			}
			const bool solid = f.At(x, y, z) != Block::Air;
			if (inside != (solid ? 1 : 0)) return false;
		}
		return true;
	}

	// Carves a room in front of the chamber wall and leaves a 1 m block inside it.
	// `bridge` keeps a 1-voxel column joining the block to the ceiling.
	VoxelWorld::Box MakeHangingBlock(VoxelWorld& world, bool bridge, int& blockVoxels)
	{
		const int V = cfg::kVoxelsPerMeter;
		VoxelWorld::Box room;
		room.min[0] = 9 * V;  room.max[0] = 12 * V - 1;
		room.min[1] = 1 * V;  room.max[1] = 4 * V - 1;
		room.min[2] = 12 * V; room.max[2] = 15 * V - 1;
		const int bx0 = 10 * V, by0 = 2 * V, bz0 = 13 * V;
		blockVoxels = 0;
		for (int z = room.min[2]; z <= room.max[2]; ++z)
		for (int y = room.min[1]; y <= room.max[1]; ++y)
		for (int x = room.min[0]; x <= room.max[0]; ++x)
		{
			const bool inBlock = x >= bx0 && x < bx0 + V && y >= by0 && y < by0 + V && z >= bz0 && z < bz0 + V;
			const bool inBridge = bridge && x == bx0 && z == bz0 && y >= by0 + V;
			world.Set(x, y, z, (inBlock || inBridge) ? Block::Stone : Block::Air);
			if (inBlock) ++blockVoxels;
		}
		return room;
	}
}

int main()
{
	std::mt19937 rng(1234);

	{
		bool ok = true;
		for (int i = 0; i < 30 && ok; ++i)
		{
			const FragmentVoxels f = RandomFragment(rng, 2 + i % 5, 0.6f);
			ok = BoxesCoverExactly(f, DecomposeIntoBoxes(f, 100000));
		}
		Check(ok, "greedy boxes cover exactly the solid voxels, no overlaps (30 random fragments)");
	}
	{
		FragmentVoxels solid;
		solid.size = { 6, 6, 6 };
		solid.voxels.assign(216, Block::Stone);
		Check(DecomposeIntoBoxes(solid, 48).size() == 1, "a solid block becomes a single box");
	}
	{
		const FragmentVoxels f = RandomFragment(rng, 10, 0.5f);
		Check(DecomposeIntoBoxes(f, 100000).size() > 8, "a noisy fragment needs many boxes at full resolution");
		Check(DecomposeIntoBoxes(f, 8).size() <= 8, "the box cap coarsens the shape until it fits");
	}
	{
		FragmentVoxels f;
		f.size = { 2, 1, 1 };
		f.voxels = { Block::Stone, Block::Gold };
		const float s = cfg::kVoxelSize;
		auto baked = SnapToGrid(f, { 10 * s, 20 * s, 30 * s }, { 0, 0, 0, 1 });
		Check(baked.size() == 2 && baked[0].x == 10 && baked[0].y == 20 && baked[0].z == 30 && baked[1].x == 11 && baked[1].block == Block::Gold,
			"snap to grid: unrotated fragment lands on its cells and keeps ore");

		XMFLOAT4 q;
		XMStoreFloat4(&q, XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XM_PIDIV2));
		baked = SnapToGrid(f, { 10 * s, 20 * s, 30 * s }, q);
		Check(baked.size() == 2 && baked[0].z != baked[1].z && baked[0].x == baked[1].x, "snap to grid: 90 degree turn maps x onto z");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, false, blockVoxels);
		DetachResult result = DetachUnsupported(world, room, cfg::kSupportSearchBudget);
		Check(result.fragments.size() == 1 && result.fragments[0].SolidCount() == blockVoxels, "support: a block hanging in a room detaches as one fragment");
		Check(world.Get(10 * cfg::kVoxelsPerMeter, 2 * cfg::kVoxelsPerMeter, 13 * cfg::kVoxelsPerMeter) == Block::Air, "support: detached voxels are removed from the world");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, true, blockVoxels);
		Check(DetachUnsupported(world, room, cfg::kSupportSearchBudget).fragments.empty(), "support: a one-voxel bridge to the ceiling holds");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, false, blockVoxels);
		Check(DetachUnsupported(world, room, blockVoxels / 2).fragments.empty(), "support: regions larger than the budget count as anchored");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		// Greedy collision faces must cover the same area as the exposed voxel faces.
		const int z0 = 0, z1 = 16;
		std::vector<XMFLOAT3> tris;
		world.BuildCollisionTriangles(z0, z1, tris);
		double area = 0.0;
		bool outward = true;
		for (size_t i = 0; i + 2 < tris.size(); i += 3)
		{
			const XMVECTOR a = XMLoadFloat3(&tris[i]), b = XMLoadFloat3(&tris[i + 1]), c = XMLoadFloat3(&tris[i + 2]);
			const XMVECTOR n = XMVector3Cross(b - a, c - a);
			area += 0.5 * XMVectorGetX(XMVector3Length(n));
			// Step from the triangle's centre along its normal: that must be air.
			XMFLOAT3 probe;
			XMStoreFloat3(&probe, (a + b + c) / 3.0f + XMVector3Normalize(n) * (cfg::kVoxelSize * 0.5f));
			const int V = cfg::kVoxelsPerMeter;
			if (world.IsSolid(static_cast<int>(std::floor(probe.x * V)), static_cast<int>(std::floor(probe.y * V)), static_cast<int>(std::floor(probe.z * V)))) outward = false;
		}
		int faces = 0;
		for (int z = z0; z < z1; ++z)
		for (int y = 0; y < cfg::kVoxY; ++y)
		for (int x = 0; x < cfg::kVoxX; ++x)
		{
			if (world.Get(x, y, z) == Block::Air) continue;
			faces += !world.IsSolid(x + 1, y, z) + !world.IsSolid(x - 1, y, z) + !world.IsSolid(x, y + 1, z)
				+ !world.IsSolid(x, y - 1, z) + !world.IsSolid(x, y, z + 1) + !world.IsSolid(x, y, z - 1);
		}
		const double expected = faces * cfg::kVoxelSize * cfg::kVoxelSize;
		Check(std::fabs(area - expected) < 1e-3 * expected, "collision: greedy faces cover the same area as the exposed voxel faces");
		Check(outward, "collision: every triangle faces into air");
		std::printf("        (%zu triangles for %d voxel faces)\n", tris.size() / 3, faces);
	}

	std::printf("%s (%d failure%s)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
