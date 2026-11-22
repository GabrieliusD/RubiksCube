#pragma once
#include "CherryAPI.h"
#include "Renderer/RenderTypes.h"
#include "Physics/PhysicsWorld.h"
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Voxel models: small blocky objects (buildings, props, trees) made of coloured voxels.
//
// VoxelModel is the data, independent of the tool that made it. Files are MagicaVoxel
// ".vox" (LoadVox / SaveVox), so MagicaVoxel, the engine and a future built-in voxel
// editor all read and write the same files. For drawing, a model is baked into an ordinary
// mesh (BakeVoxelMesh); for collision, into a few boxes (BakeVoxelBoxes).
//
// Axes are the engine's: +X right, +Y up, +Z forward. MagicaVoxel is Z-up, so its
// (x, y, z) is the engine's (x, z, y); the swap also turns its right-handed space into the
// engine's left-handed one, so models aren't mirrored.

// What a palette colour is made of, beyond its colour.
struct VoxelMaterial
{
	float emissive = 0.0f;     // glow strength: emitted light = colour x this (0 = lit normally)
	float glass = 0.0f;        // see-through: how much light passes, 0..1 (0 = opaque). MagicaVoxel's glass "_alpha"
};

struct VoxelModel
{
	DirectX::XMUINT3 size{ 0, 0, 0 };
	std::vector<uint8_t> voxels;                    // palette indices, x fastest, then y, then z; 0 = empty
	std::array<DirectX::XMFLOAT4, 256> palette{};   // sRGB 0..1; index 0 is unused
	std::array<VoxelMaterial, 256> materials{};

	void Resize(const DirectX::XMUINT3& newSize)   // clears to empty
	{
		size = newSize;
		voxels.assign(static_cast<size_t>(size.x) * size.y * size.z, 0);
	}
	bool InBounds(int x, int y, int z) const
	{
		return x >= 0 && y >= 0 && z >= 0 && x < static_cast<int>(size.x) && y < static_cast<int>(size.y) && z < static_cast<int>(size.z);
	}
	uint8_t At(int x, int y, int z) const { return InBounds(x, y, z) ? voxels[Index(x, y, z)] : 0; }
	void Set(int x, int y, int z, uint8_t index) { if (InBounds(x, y, z)) voxels[Index(x, y, z)] = index; }
	size_t Index(int x, int y, int z) const { return x + static_cast<size_t>(size.x) * (y + static_cast<size_t>(size.y) * z); }
	size_t SolidCount() const
	{
		size_t count = 0;
		for (uint8_t v : voxels) count += v != 0;
		return count;
	}
};

// ---- Files ------------------------------------------------------------------------------
// Reads a MagicaVoxel file (version 150 or 200). Several models in one file are combined
// into one, placed by their scene translations (rotations aren't supported yet and are
// ignored with a warning in `error`, which is otherwise left empty on success).
CHERRY_API bool ParseVox(const uint8_t* data, size_t length, VoxelModel& out, std::string& error);
CHERRY_API bool LoadVox(const std::filesystem::path& file, VoxelModel& out, std::string& error);
// Writes one model, its palette and its emissive materials, in a form MagicaVoxel opens.
CHERRY_API std::vector<uint8_t> WriteVox(const VoxelModel& model);
CHERRY_API bool SaveVox(const std::filesystem::path& file, const VoxelModel& model, std::string& error);

// ---- Baking -----------------------------------------------------------------------------
// Where the model's local origin is.
enum class VoxelPivot
{
	BottomCenter,   // centre of the bottom face: place it on the ground at its position
	Corner,         // the minimum corner (voxel 0, 0, 0)
};

// Which voxels a mesh is baked from: a model with glass is drawn as two meshes, the opaque
// part (its faces show through the glass) and the glass part (drawn translucent).
enum class VoxelPart { All, Opaque, Glass };

struct VoxelBakeOptions
{
	float voxelSize = 0.25f;            // metres per voxel (UNDERTIME's grid)
	VoxelPivot pivot = VoxelPivot::BottomCenter;
	float aoStrength = 0.6f;            // corner darkening baked into vertex colours, 0..1
	VoxelPart part = VoxelPart::All;
};

// Whether any voxel of the model uses a glass colour.
CHERRY_API bool HasGlass(const VoxelModel& model);

// Model-space offset (metres) that BakeVoxelMesh / BakeVoxelBoxes add for the pivot.
CHERRY_API DirectX::XMFLOAT3 VoxelPivotOffset(const VoxelModel& model, const VoxelBakeOptions& options);

// One mesh for the whole model: only faces between a voxel and empty space, merged into
// large rectangles where colour and corner shading match. Vertex colour = the palette
// colour (linear) x corner shading; uv.x = the colour's emissive strength, uv.y = the voxel
// size (the built-in "engine/VoxelModel" material turns these into glow and a slightly
// different brightness per voxel).
CHERRY_API MeshData BakeVoxelMesh(const VoxelModel& model, const VoxelBakeOptions& options = {});

// The solid voxels as a few boxes (merged greedily), for static or dynamic collision.
CHERRY_API std::vector<PhysicsBox> BakeVoxelBoxes(const VoxelModel& model, const VoxelBakeOptions& options = {});
