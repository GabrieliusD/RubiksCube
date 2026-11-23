#pragma once
// The valley outside the cave (Undertime/Docs/Design.md §6): ground heights, the river and
// the lake, what the ground is made of, and where trees grow. Pure C++ (no engine), the
// same every run: the valley never changes between loops, only the cave does.
//
// World metres: the cave grid occupies x 0..kGridX, z 0..kGridZ, y 0..kGridY, its mouth in
// the plane z = 0; the valley floor lies to the south (-z) at about y = kGroundY.
#include "Noise.h"
#include <cstdint>
#include <vector>

enum class Ground : uint8_t
{
	Grass,
	Grass2,     // a second shade, in patches
	Dirt,       // under grass (block sides)
	Sand,       // river banks and the lake shore
	Gravel,     // under water
	Rock,
	Rock2,
	Snow,
	Path,       // trodden trails between places
	Moss,       // the forest floor: darker, mossy
	Count
};

// Flat ground under a building or plot: inside the rectangle the ground is at `height`,
// blending back into the land over cfg::kPadBlend metres around it.
struct GroundPad
{
	float x = 0, z = 0;           // centre
	float halfX = 1, halfZ = 1;   // half size (axis-aligned)
	float height = 0;
};

struct TreeSpot
{
	float x = 0, z = 0;
	int kind = 0;           // index into the tree models (0..2 conifers small to large, 3 broadleaf)
	float yaw = 0;          // radians
	bool distant = false;   // on the mountains (outside Detailed): sparser, conifers only
};

// A rock outcrop on steep ground (placeholder crags in three sizes).
struct OutcropSpot
{
	float x = 0, z = 0;
	int kind = 0;           // 0..2, small to large
	float yaw = 0;          // radians
	bool distant = false;   // on the mountains (outside Detailed): a crag, four times the size
};

class Valley
{
public:
	Valley();

	// Ground surface height (not yet rounded to blocks), including the river bed and lake.
	float Height(float x, float z) const;
	// Height of the water surface here, or kNoWater.
	float WaterLevel(float x, float z) const;
	// Both at once (cheaper than calling the two).
	void Sample(float x, float z, float& height, float& water) const;
	static constexpr float kNoWater = -1e9f;
	// The valley floor and the cave mouth (dense trees, small outcrops), against the mountains
	// (sparser trees, big crags). Decided per terrain chunk (cfg::kTerrainChunk).
	bool Detailed(float x, float z) const;
	// The river runs through this terrain chunk: it's always built from 0.25 m blocks (water
	// levels in coarser blocks wouldn't meet, and the rock would show through between them).
	bool RiverChunk(float x, float z) const;

	// Flat pads under the town's buildings (applied by Height/Sample; empty by default).
	void SetPads(std::vector<GroundPad> pads) { mPads = std::move(pads); }
	const std::vector<GroundPad>& Pads() const { return mPads; }
	// Over the cave grid: the ground there starts at the grid's top (the grid is the rock below).
	static bool OverCave(float x, float z);
	// Crags on steep ground, away from water, trails, clearings and the cave's face.
	std::vector<OutcropSpot> Outcrops() const;
	// What the top of a column is made of. `drop`: the biggest height difference to its
	// neighbours (steep means rock); `block`: the column size (0.25, 0.5 or 1 m).
	Ground GroundAt(float x, float z, float height, float drop, float block) const;
	// Every tree, in a fixed order. Heights aren't included: put them on the terrain.
	std::vector<TreeSpot> Trees() const;

	// Distance from the river's centre line, and how far along it (metres from its source).
	float RiverDistance(float x, float z, float* along = nullptr) const;
	float RiverLevelAt(float along) const;
	// The point on the river's centre line `along` metres from its source.
	void RiverPointAt(float along, float& x, float& z) const;
	float RiverLength() const { return mRiverLength.empty() ? 0.0f : mRiverLength.back(); }
	float LakeLevel() const { return mLakeLevel; }
	// The ground without the river and lake cut into it.
	float BaseHeight(float x, float z) const;
	// 0 in the middle of the valley floor, 1 at its edge, more in the mountains.
	float ValleyDistance(float x, float z) const;
	// Distance to the nearest trail (cfg::kPathSegments).
	static float PathDistance(float x, float z);
	// Is a tree allowed here? (clearings, water, steepness, tree line)
	bool TreeAllowed(float x, float z) const;
	// How wooded it is here: the forest grows where this is above about -0.15, thickly from
	// about 0.3 (the same measure Trees uses).
	float ForestPatch(float x, float z) const;

private:
	float Noise2(float x, float z, int octaves) const { return mNoise.Fbm(x, 0.37f, z, octaves); }

	Perlin3 mNoise;
	std::vector<float> mRiverLength;   // cumulative length at each point of cfg::kRiverPoints
	std::vector<float> mRiverLevel;    // water level per metre along the river (never rises downstream)
	float mLakeLevel = 0.0f;
	std::vector<GroundPad> mPads;
};
