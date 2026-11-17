#include "Structure.h"
#include <algorithm>
#include <unordered_set>
#include <vector>

using cfg::Block;

namespace
{
	uint32_t Key(int x, int y, int z)
	{
		return static_cast<uint32_t>(x + cfg::kVoxX * (y + cfg::kVoxY * z));
	}
}

DetachResult DetachUnsupported(VoxelWorld& world, const VoxelWorld::Box& changed, int budget)
{
	DetachResult result;
	if (changed.Empty()) return result;

	// Seeds: solid voxels in the changed box grown by one (the neighbours that may have lost support).
	const int x0 = std::max(changed.min[0] - 1, 0), x1 = std::min(changed.max[0] + 1, cfg::kVoxX - 1);
	const int y0 = std::max(changed.min[1] - 1, 0), y1 = std::min(changed.max[1] + 1, cfg::kVoxY - 1);
	const int z0 = std::max(changed.min[2] - 1, 0), z1 = std::min(changed.max[2] + 1, cfg::kVoxZ - 1);

	std::unordered_set<uint32_t> anchored;   // voxels known to be connected to the mountain
	std::vector<int> stack;
	std::vector<int> component;
	std::unordered_set<uint32_t> inComponent;

	static const int kNeighbours[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };

	for (int z = z0; z <= z1; ++z)
	for (int y = y0; y <= y1; ++y)
	for (int x = x0; x <= x1; ++x)
	{
		const Block seed = world.Get(x, y, z);
		if (seed == Block::Air || seed == Block::Bedrock) continue;
		if (anchored.count(Key(x, y, z))) continue;

		// Flood fill the region containing this seed.
		component.clear();
		inComponent.clear();
		stack.clear();
		stack.push_back(x); stack.push_back(y); stack.push_back(z);
		inComponent.insert(Key(x, y, z));
		bool isAnchored = false;
		while (!stack.empty())
		{
			const int cz = stack.back(); stack.pop_back();
			const int cy = stack.back(); stack.pop_back();
			const int cx = stack.back(); stack.pop_back();
			component.push_back(cx); component.push_back(cy); component.push_back(cz);
			if (static_cast<int>(component.size() / 3) > budget)
			{
				isAnchored = true;   // too big to be a loose piece: part of the mountain
				break;
			}
			for (const auto& n : kNeighbours)
			{
				const int nx = cx + n[0], ny = cy + n[1], nz = cz + n[2];
				if (!VoxelWorld::InGrid(nx, ny, nz)) { isAnchored = true; break; }
				const Block b = world.Get(nx, ny, nz);
				if (b == Block::Air) continue;
				const uint32_t key = Key(nx, ny, nz);
				if (b == Block::Bedrock || anchored.count(key)) { isAnchored = true; break; }
				if (inComponent.insert(key).second)
				{
					stack.push_back(nx); stack.push_back(ny); stack.push_back(nz);
				}
			}
			if (isAnchored) break;
		}
		result.visited += static_cast<int>(inComponent.size());

		if (isAnchored)
		{
			// Everything reached so far is connected to the mountain; never search it again.
			anchored.insert(inComponent.begin(), inComponent.end());
			continue;
		}

		// A loose region: cut it out of the world as a fragment.
		int mn[3] = { cfg::kVoxX, cfg::kVoxY, cfg::kVoxZ };
		int mx[3] = { -1, -1, -1 };
		for (size_t i = 0; i < component.size(); i += 3)
		{
			for (int a = 0; a < 3; ++a)
			{
				mn[a] = std::min(mn[a], component[i + a]);
				mx[a] = std::max(mx[a], component[i + a]);
			}
		}
		FragmentVoxels fragment;
		fragment.size = { mx[0] - mn[0] + 1, mx[1] - mn[1] + 1, mx[2] - mn[2] + 1 };
		fragment.worldOrigin = { mn[0], mn[1], mn[2] };
		fragment.voxels.assign(static_cast<size_t>(fragment.size.x) * fragment.size.y * fragment.size.z, Block::Air);
		for (size_t i = 0; i < component.size(); i += 3)
		{
			const int vx = component[i], vy = component[i + 1], vz = component[i + 2];
			fragment.voxels[(vx - mn[0]) + fragment.size.x * ((vy - mn[1]) + fragment.size.y * (vz - mn[2]))] = world.Get(vx, vy, vz);
			world.Set(vx, vy, vz, Block::Air);
		}
		VoxelWorld::Box box;
		for (int a = 0; a < 3; ++a) { box.min[a] = mn[a]; box.max[a] = mx[a]; }
		result.removed.push_back(box);
		result.fragments.push_back(std::move(fragment));
	}
	return result;
}
