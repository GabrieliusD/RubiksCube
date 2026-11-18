#pragma once
#include "CaveGenerator.h"
#include "Hud.h"
#include <vector>

// Debug map of the mountain seen from above (M): depth runs left to right, width top to
// bottom. Each 1 m column is coloured by what it holds: zone caves, sealed pockets, ore,
// relics, with the seal bands shaded and the player marked.
class CaveMap
{
public:
	// Rebuilds the column summary from the world (call after generation and edits).
	void Rebuild(const VoxelWorld& world, const CaveLayout& layout);
	void Draw(const hud::Frame& frame, const CaveLayout& layout, const DirectX::XMFLOAT3& player, float playerYaw) const;

private:
	enum class Cell : uint8_t { Rock, Cave, Pocket, Ore, Relic };
	std::vector<Cell> mCells;   // cfg::kGridX * cfg::kGridZ, x fastest
	std::vector<uint8_t> mZone; // zone index per column (255 = seal / none)
};
