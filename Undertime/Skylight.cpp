#include "Skylight.h"
#include <algorithm>
#include <cmath>

namespace
{
	struct Step { int dx, dy, dz; };
	// Light spreads to the six neighbours, and also falls diagonally (down and to a side) as
	// easily as straight down, so it follows a leaning shaft.
	constexpr Step kSteps[14] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
		{ 1, -1, 0 }, { -1, -1, 0 }, { 0, -1, 1 }, { 0, -1, -1 }, { 1, -1, 1 }, { 1, -1, -1 }, { -1, -1, 1 }, { -1, -1, -1 } };
	constexpr int kAxisSteps = 6;   // the first six: the faces a wall shares with the air

	int Cost(const Step& s) { return s.dy < 0 ? cfg::kSkylightDownStep : cfg::kSkylightStep; }

	void Grow(Skylight::CellBox& box, int x, int y, int z)
	{
		if (box.Empty())
		{
			box.min[0] = box.max[0] = x; box.min[1] = box.max[1] = y; box.min[2] = box.max[2] = z;
			return;
		}
		box.min[0] = std::min(box.min[0], x); box.max[0] = std::max(box.max[0], x);
		box.min[1] = std::min(box.min[1], y); box.max[1] = std::max(box.max[1], y);
		box.min[2] = std::min(box.min[2], z); box.max[2] = std::max(box.max[2], z);
	}
}

bool Skylight::CellOpen(const VoxelWorld& world, int cx, int cy, int cz)
{
	// Open when at least half its voxels are air.
	int air = 0;
	for (int z = 0; z < kCell; ++z)
		for (int y = 0; y < kCell; ++y)
			for (int x = 0; x < kCell; ++x)
				air += world.Get(cx * kCell + x, cy * kCell + y, cz * kCell + z) == cfg::Block::Air ? 1 : 0;
	return air * 2 >= kCell * kCell * kCell;
}

uint8_t Skylight::SourceLevel(int cx, int cy, int cz) const
{
	return (cz == 0 || cy == kY - 1) && mOpen[Index(cx, cy, cz)] ? 255 : 0;
}

void Skylight::Build(const VoxelWorld& world)
{
	mLevel.assign(static_cast<size_t>(kX) * kY * kZ, 0);
	mOpen.assign(mLevel.size(), 0);
	mBuckets.assign(256, {});
	for (int cz = 0; cz < kZ; ++cz)
		for (int cy = 0; cy < kY; ++cy)
			for (int cx = 0; cx < kX; ++cx)
				mOpen[Index(cx, cy, cz)] = CellOpen(world, cx, cy, cz) ? 1 : 0;
	for (int cz = 0; cz < kZ; ++cz)
		for (int cy = 0; cy < kY; ++cy)
			for (int cx = 0; cx < kX; ++cx)
			{
				const uint8_t source = SourceLevel(cx, cy, cz);
				if (source == 0) continue;
				const int i = Index(cx, cy, cz);
				mLevel[i] = source;
				mBuckets[source].push_back(i);
			}
	Spread(nullptr);
}

void Skylight::Spread(CellBox* changed)
{
	// Brightest first (a bucket per level): each cell is final when its bucket comes up.
	for (int level = 255; level > 0; --level)
	{
		std::vector<int>& bucket = mBuckets[level];
		for (size_t b = 0; b < bucket.size(); ++b)
		{
			const int i = bucket[b];
			if (mLevel[i] != level) continue;
			const int cx = i % kX, cy = (i / kX) % kY, cz = i / (kX * kY);
			for (const Step& s : kSteps)
			{
				const int nx = cx + s.dx, ny = cy + s.dy, nz = cz + s.dz;
				if (!InCells(nx, ny, nz)) continue;
				const int n = Index(nx, ny, nz);
				const int next = level - Cost(s);
				if (!mOpen[n] || next <= mLevel[n]) continue;
				mLevel[n] = static_cast<uint8_t>(next);
				mBuckets[next].push_back(n);
				if (changed) Grow(*changed, nx, ny, nz);
			}
		}
		bucket.clear();
	}
}

Skylight::CellBox Skylight::Update(const VoxelWorld& world, const VoxelWorld::Box& box)
{
	CellBox changed;
	if (box.Empty() || mLevel.empty()) return changed;
	const int c0[3] = { std::max(box.min[0] / kCell, 0), std::max(box.min[1] / kCell, 0), std::max(box.min[2] / kCell, 0) };
	const int c1[3] = { std::min(box.max[0] / kCell, kX - 1), std::min(box.max[1] / kCell, kY - 1), std::min(box.max[2] / kCell, kZ - 1) };
	for (int cz = c0[2]; cz <= c1[2]; ++cz)
		for (int cy = c0[1]; cy <= c1[1]; ++cy)
			for (int cx = c0[0]; cx <= c1[0]; ++cx)
			{
				const int i = Index(cx, cy, cz);
				if (mOpen[i] || !CellOpen(world, cx, cy, cz)) continue;
				mOpen[i] = 1;
				Grow(changed, cx, cy, cz);   // its GPU value changes (it no longer copies a neighbour)
				// The light it gets: daylight entering here, or its brightest neighbour's.
				int level = SourceLevel(cx, cy, cz);
				for (const Step& s : kSteps)
				{
					const int nx = cx - s.dx, ny = cy - s.dy, nz = cz - s.dz;   // the neighbour light would come from
					if (InCells(nx, ny, nz) && mOpen[Index(nx, ny, nz)]) level = std::max(level, mLevel[Index(nx, ny, nz)] - Cost(s));
				}
				if (level > mLevel[i])
				{
					mLevel[i] = static_cast<uint8_t>(level);
					mBuckets[level].push_back(i);
				}
			}
	Spread(&changed);
	if (changed.Empty()) return changed;
	// Solid neighbours copy the brightest open cell next to them: they change too.
	for (int a = 0; a < 3; ++a)
	{
		changed.min[a] = std::max(changed.min[a] - 1, 0);
		changed.max[a] = std::min(changed.max[a] + 1, (a == 0 ? kX : a == 1 ? kY : kZ) - 1);
	}
	return changed;
}

float Skylight::LevelAt(float x, float y, float z) const
{
	const int cx = static_cast<int>(std::floor(x / (cfg::kVoxelSize * kCell)));
	const int cy = static_cast<int>(std::floor(y / (cfg::kVoxelSize * kCell)));
	const int cz = static_cast<int>(std::floor(z / (cfg::kVoxelSize * kCell)));
	if (!InCells(cx, cy, cz) || mLevel.empty()) return 0.0f;
	return mLevel[Index(cx, cy, cz)] / 255.0f;
}

std::vector<uint8_t> Skylight::GpuLevels(const CellBox& box) const
{
	std::vector<uint8_t> out;
	if (box.Empty() || mLevel.empty()) return out;
	out.reserve(static_cast<size_t>(box.max[0] - box.min[0] + 1) * (box.max[1] - box.min[1] + 1) * (box.max[2] - box.min[2] + 1));
	uint8_t shaped[256];
	for (int l = 0; l < 256; ++l) shaped[l] = static_cast<uint8_t>(std::lround(255.0f * std::pow(l / 255.0f, cfg::kSkylightGamma)));
	for (int cz = box.min[2]; cz <= box.max[2]; ++cz)
		for (int cy = box.min[1]; cy <= box.max[1]; ++cy)
			for (int cx = box.min[0]; cx <= box.max[0]; ++cx)
			{
				const int i = Index(cx, cy, cz);
				int level = mLevel[i];
				if (!mOpen[i])
				{
					for (int k = 0; k < kAxisSteps; ++k)
					{
						const Step& s = kSteps[k];
						const int nx = cx + s.dx, ny = cy + s.dy, nz = cz + s.dz;
						if (InCells(nx, ny, nz) && mOpen[Index(nx, ny, nz)]) level = std::max<int>(level, mLevel[Index(nx, ny, nz)]);
					}
				}
				out.push_back(shaped[level]);
			}
	return out;
}
