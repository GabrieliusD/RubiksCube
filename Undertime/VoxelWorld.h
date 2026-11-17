#pragma once
#include "Config.h"
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <array>
#include <vector>

// The mountain interior as a fine voxel grid (cfg::kVoxelsPerMeter per metre), plus the
// solid/air rules for the outside, ray casting, swing carving, and the data both
// renderers need (palette indices for the ray-marched volume, exposed-face meshes).
//
// Voxel coordinates: voxel (x, y, z) spans [x, x+1] * kVoxelSize in metres on each axis.
// Public functions taking DirectX::XMFLOAT3 positions use metres.
class VoxelWorld
{
public:
	using Block = cfg::Block;

	struct RayHit
	{
		bool hit = false;
		bool inGrid = false;       // false: the ray hit outside ground / mountain rock
		int x = 0, y = 0, z = 0;   // hit voxel
		int normal[3] = { 0, 0, 0 };
		float distance = 0.0f;     // metres
		DirectX::XMFLOAT3 point{ 0, 0, 0 };
	};

	// Voxel-space box, inclusive.
	struct Box
	{
		int min[3] = { 0, 0, 0 };
		int max[3] = { -1, -1, -1 };
		bool Empty() const { return max[0] < min[0]; }
	};

	struct RemovedVoxel
	{
		int x, y, z;
		Block block;
	};

	struct CarveResult
	{
		int removed = 0;
		std::vector<RemovedVoxel> voxels;
		std::array<int, static_cast<int>(Block::Count)> removedByType{};
		Box dirty;
	};

	// Procedural cave world for `seed` (see CaveGenerator).
	void Generate(uint64_t seed);
	// Fixed layout used by tests: bedrock shell, solid stone, entrance passage and one chamber.
	void GenerateBasic();
	// Raw access for the generator.
	std::vector<Block>& Voxels() { return mVoxels; }
	const std::vector<Block>& Voxels() const { return mVoxels; }
	static int Index(int x, int y, int z) { return x + cfg::kVoxX * (y + cfg::kVoxY * z); }
	uint64_t Hash() const;   // content hash (determinism tests)

	static bool InGrid(int x, int y, int z);
	Block Get(int x, int y, int z) const;   // Air outside the grid
	void Set(int x, int y, int z, Block block);

	// Solidity of any voxel-sized cell of the world: the grid inside, the mountain around it,
	// the ground and the play-area boundary outside.
	bool IsSolid(int x, int y, int z) const;
	// True if an axis-aligned box (metres) overlaps any solid voxel.
	bool BoxOverlapsSolid(const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max) const;

	// Amanatides-Woo DDA from `origin` (m) along normalized `direction`, up to maxDistance (m).
	RayHit Raycast(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float maxDistance) const;

	// Removes voxels around `center` (m) for one pick swing. `mustBreak` (if in the grid and not
	// bedrock) always breaks, so every swing makes progress.
	CarveResult Carve(const DirectX::XMFLOAT3& center, float pickSpeed, int mustBreakX, int mustBreakY, int mustBreakZ, uint32_t salt);

	float EffectiveHardness(Block block, int voxelZ) const;

	// ---- Ray-marched renderer ----
	static std::vector<DirectX::XMFLOAT4> BuildPalette();       // sRGB
	static uint8_t PaletteIndex(Block block, int voxelZ);
	std::vector<uint8_t> PaletteVoxels() const;                  // whole grid
	std::vector<uint8_t> PaletteVoxels(const Box& box) const;    // a box, x fastest

	// ---- Physics ----
	// Exposed faces of solid voxels with voxel z in [zBegin, zEnd), merged into large
	// rectangles (greedy meshing), as triangles in metres wound so (b-a)x(c-a) points into air.
	void BuildCollisionTriangles(int zBegin, int zEnd, std::vector<DirectX::XMFLOAT3>& out) const;
	// Same for the voxel box [lo, hi) on every axis.
	void BuildCollisionTriangles(const int lo[3], const int hi[3], std::vector<DirectX::XMFLOAT3>& out) const;
	// True if the box [lo, hi) contains or touches any air (i.e. it can have exposed faces).
	bool TouchesAir(const int lo[3], const int hi[3]) const;

	// ---- Mesh renderer ----
	static int ChunkCount() { return (cfg::kGridZ + cfg::kChunkDepth - 1) / cfg::kChunkDepth; }
	static int ChunkOfVoxelZ(int z) { return z / (cfg::kChunkDepth * cfg::kVoxelsPerMeter); }
	MeshData BuildChunkMesh(int chunk) const;

private:
	DirectX::XMFLOAT4 VoxelColor(Block block, int x, int y, int z) const;

	std::vector<Block> mVoxels;
};
