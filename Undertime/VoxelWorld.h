#pragma once
#include "Config.h"
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <array>
#include <unordered_map>
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
		Box dirty;     // voxels removed
		Box cracked;   // voxels damaged but still standing (colour changes only)
	};

	// Generated worlds come from GenerateCaves() (CaveGenerator.h).
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
	// crack: 0 = intact, 1 = lightly cracked, 2 = badly cracked.
	static uint8_t PaletteIndex(Block block, int voxelZ, int crack = 0);
	// The palette's layout for shaders (Content/Shaders/CaveRock.hlsl): stone bands, the first
	// ore entry (one per block type), the first cracked entry and the size of a cracked set.
	static DirectX::XMFLOAT4 PaletteLayout();
	int CrackLevel(int x, int y, int z) const;
	float Damage(int x, int y, int z) const;
	// Sets a voxel's pick damage (0 intact .. about 1 nearly broken): the showcase's cracked blocks.
	void SetDamage(int x, int y, int z, float damage);
	void ClearDamage() { mDamage.clear(); }
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
	// Linear colour of a voxel of `block` at world voxel (x, y, z), as the mesh renderer draws it.
	static DirectX::XMFLOAT4 VoxelColor(Block block, int x, int y, int z);

private:

	std::vector<Block> mVoxels;
	std::unordered_map<uint32_t, float> mDamage;   // accumulated pick damage of standing voxels
};
