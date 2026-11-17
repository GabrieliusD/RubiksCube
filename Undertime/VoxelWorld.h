#pragma once
#include "Config.h"
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <vector>

// The mountain interior as a voxel grid, plus the solid/air rules for the
// outside, ray casting and exposed-face chunk meshing. No engine state: the
// game owns the meshes built from it.
class VoxelWorld
{
public:
	using Block = cfg::Block;

	struct RayHit
	{
		bool hit = false;
		bool inGrid = false;       // false: the ray hit outside ground / mountain rock
		int x = 0, y = 0, z = 0;   // hit cell
		int normal[3] = { 0, 0, 0 };
		float distance = 0.0f;
	};

	void Generate(uint32_t seed);

	static bool InGrid(int x, int y, int z);
	Block Get(int x, int y, int z) const;   // Air outside the grid
	void Set(int x, int y, int z, Block block);

	// Solidity of any world cell: the grid inside, the mountain around it, the ground
	// and the boundary outside.
	bool IsSolid(int x, int y, int z) const;
	// True if an axis-aligned box overlaps any solid cell.
	bool BoxOverlapsSolid(const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max) const;

	// Amanatides-Woo DDA from `origin` along normalized `direction`.
	RayHit Raycast(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float maxDistance) const;

	static int ChunkCount() { return (cfg::kGridZ + cfg::kChunkDepth - 1) / cfg::kChunkDepth; }
	static int ChunkOf(int z) { return z / cfg::kChunkDepth; }
	// Faces of solid grid cells that touch a non-solid cell, in world space.
	MeshData BuildChunkMesh(int chunk) const;

	float EffectiveHardness(int x, int y, int z) const;

private:
	static int Index(int x, int y, int z) { return x + cfg::kGridX * (y + cfg::kGridY * z); }
	DirectX::XMFLOAT4 CellColor(Block block, int x, int y, int z) const;

	std::vector<Block> mCells;
};
