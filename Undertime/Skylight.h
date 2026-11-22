#pragma once
// Daylight reaching into the mountain (Design.md, "The cave"): a level 0..255 per 0.5 m cell
// of the cave grid, flowing in from the face (z = 0) and from openings in the grid's top
// (the natural shafts). Each cell it passes costs kSkylightStep sideways or up, and only
// kSkylightDownStep down, straight or diagonally (light falls down a shaft easily, even a
// leaning one), so the first caverns are dim and deep ones dark. The renderer reads it as
// the interior's light volume.
//
// Mining only ever removes rock, so after the first build light only needs adding: Update
// lets it into the cells an edit opened. Pure C++ (tested in UndertimeTests).
#include "VoxelWorld.h"
#include <cstdint>
#include <vector>

class Skylight
{
public:
	static constexpr int kCell = 2;   // voxels per cell edge
	static constexpr int kX = cfg::kVoxX / kCell, kY = cfg::kVoxY / kCell, kZ = cfg::kVoxZ / kCell;

	struct CellBox
	{
		int min[3] = { 0, 0, 0 };
		int max[3] = { -1, -1, -1 };
		bool Empty() const { return max[0] < min[0]; }
	};

	void Build(const VoxelWorld& world);
	// After the voxels in `box` changed: lets light into the cells they opened. Returns the
	// cells whose GPU levels changed (empty if none).
	CellBox Update(const VoxelWorld& world, const VoxelWorld::Box& box);

	uint8_t Level(int cx, int cy, int cz) const { return mLevel[Index(cx, cy, cz)]; }
	bool IsOpen(int cx, int cy, int cz) const { return mOpen[Index(cx, cy, cz)] != 0; }
	// 0..1 at a point (metres), the cell it's in.
	float LevelAt(float x, float y, float z) const;

	static CellBox All() { return { { 0, 0, 0 }, { kX - 1, kY - 1, kZ - 1 } }; }
	// What the renderer gets for `box` (x fastest, then y, then z): levels shaped by
	// kSkylightGamma; solid cells take their brightest open neighbour's, so a wall shows the
	// light of the air in front of it.
	std::vector<uint8_t> GpuLevels(const CellBox& box) const;

private:
	static int Index(int cx, int cy, int cz) { return cx + kX * (cy + kY * cz); }
	static bool InCells(int cx, int cy, int cz) { return cx >= 0 && cy >= 0 && cz >= 0 && cx < kX && cy < kY && cz < kZ; }
	static bool CellOpen(const VoxelWorld& world, int cx, int cy, int cz);
	uint8_t SourceLevel(int cx, int cy, int cz) const;   // 255 where daylight enters, else 0
	void Spread(CellBox* changed);

	std::vector<uint8_t> mLevel;
	std::vector<uint8_t> mOpen;
	std::vector<std::vector<int>> mBuckets;   // cells to spread from, by level
};
