#pragma once
// Voxel models made in code at runtime (the yard fence, the barrow, building pieces, crops,
// machines), baked into meshes with the engine's voxel baker. Placeholders until hand-made
// models replace them; the shapes are simple on purpose.
#include <Core/Engine.h>
#include <Voxel/VoxelModel.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace kit
{
	struct Builder
	{
		VoxelModel model;
		Builder(int x, int y, int z) { model.Resize({ static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z) }); }
		// A palette entry for an sRGB colour (reused if it's there already); `glow` > 0 emits.
		uint8_t Colour(uint32_t srgb, float glow = 0.0f, float glass = 0.0f);
		// Fills [x0, x1) x [y0, y1) x [z0, z1), clipped to the model.
		void Box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t colour);
		// Like Box, but every voxel picks one of `colours` by a hash of its position (wood grain, stone).
		void Speckle(int x0, int y0, int z0, int x1, int y1, int z1, const std::vector<uint8_t>& colours, uint32_t salt = 0);
		void Clear(int x0, int y0, int z0, int x1, int y1, int z1) { Box(x0, y0, z0, x1, y1, z1, 0); }
	private:
		int mUsed = 0;
	};

	MeshHandle Bake(Engine& engine, const VoxelModel& model, VoxelPivot pivot = VoxelPivot::BottomCenter, float voxelSize = 0.25f);
	// An entity drawing `mesh` at `position`, turned `yaw` radians about +Y.
	Entity Spawn(Engine& engine, MeshHandle mesh, MaterialHandle material, const DirectX::XMFLOAT3& position, float yaw = 0.0f);
	// The colours everything uses.
	namespace colour
	{
		constexpr uint32_t kWood = 0x8a6038, kWoodDark = 0x6a4628, kWoodLight = 0xa47a4c;
		constexpr uint32_t kIron = 0x4a4a50, kIronDark = 0x303034, kCopper = 0xb86a34, kBrass = 0xc8a050;
		constexpr uint32_t kStone = 0x8a8278, kStoneDark = 0x6e675e, kStoneLight = 0x9c958a;
		constexpr uint32_t kRope = 0xc8b48a, kCloth = 0xd8ccb0, kSoil = 0x5a4030, kSoilDark = 0x46301f;
	}
}
