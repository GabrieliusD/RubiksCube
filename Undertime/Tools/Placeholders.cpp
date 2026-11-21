// UndertimePlaceholders: writes UNDERTIME's placeholder voxel models as MagicaVoxel files into
// Content/Models/Placeholders/, so they load like real art and can be opened and improved in
// MagicaVoxel. Run it again after changing a generator here (it overwrites the files).
//
//   UndertimePlaceholders.exe [content folder]
#include <Voxel/VoxelModel.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace DirectX;

namespace
{
	namespace fs = std::filesystem;

	uint32_t Hash(int x, int y, int z, uint32_t salt)
	{
		uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(z) * 2246822519u + salt * 3266489917u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return h ^ (h >> 16);
	}
	float Hash01(int x, int y, int z, uint32_t salt) { return (Hash(x, y, z, salt) & 0xFFFFFF) / 16777216.0f; }

	XMFLOAT4 Srgb(uint32_t hex) { return { ((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, 1.0f }; }

	// Palette shared by the trees.
	enum : uint8_t { Trunk = 1, TrunkDark, LeafDark, Leaf, LeafLight, BroadDark, Broad, BroadLight };

	VoxelModel NewModel(uint32_t x, uint32_t y, uint32_t z)
	{
		VoxelModel m;
		m.Resize({ x, y, z });
		m.palette[Trunk] = Srgb(0x5a3d24);
		m.palette[TrunkDark] = Srgb(0x48311e);
		m.palette[LeafDark] = Srgb(0x24452a);
		m.palette[Leaf] = Srgb(0x2c5530);
		m.palette[LeafLight] = Srgb(0x386638);
		m.palette[BroadDark] = Srgb(0x3d6a2a);
		m.palette[Broad] = Srgb(0x4b7a32);
		m.palette[BroadLight] = Srgb(0x5b8c3a);
		return m;
	}

	void Trunk2x2(VoxelModel& m, int height)
	{
		const int c = static_cast<int>(m.size.x) / 2;
		for (int y = 0; y < height; ++y)
			for (int z = c - 1; z <= c; ++z)
				for (int x = c - 1; x <= c; ++x) m.Set(x, y, z, (y / 3) % 2 ? TrunkDark : Trunk);
	}

	// A conifer: a trunk and stacked tiers of stepped discs, narrowing upwards. Radii change
	// every two layers so the faces merge into few large rectangles.
	VoxelModel Conifer(int height, float radius, int tiers, uint32_t salt)
	{
		const int width = static_cast<int>(std::ceil(radius)) * 2 + 2;
		VoxelModel m = NewModel(width, height, width);
		Trunk2x2(m, height / 3);
		const float c = width * 0.5f;
		const int start = height / 6;
		const int tierHeight = (height - start) / tiers + 2;
		for (int t = 0; t < tiers; ++t)
		{
			const int y0 = start + t * (tierHeight - 3);
			const float base = radius * (1.0f - 0.22f * t);
			for (int y = y0; y < std::min(y0 + tierHeight, height); ++y)
			{
				const int step = (y - y0) / 2 * 2;   // radius changes every 2 layers
				const float r = std::max(base * (1.0f - static_cast<float>(step) / tierHeight), 0.8f);
				for (int z = 0; z < width; ++z)
					for (int x = 0; x < width; ++x)
					{
						const float dx = x + 0.5f - c, dz = z + 0.5f - c;
						if (dx * dx + dz * dz > r * r) continue;
						// Darker underneath each tier, a few lighter tufts on top.
						const uint8_t colour = y - y0 < 2 ? LeafDark : (Hash01(x, y, z, salt) < 0.12f ? LeafLight : Leaf);
						m.Set(x, y, z, colour);
					}
			}
		}
		// A point on top.
		const int tip = static_cast<int>(c) - 1;
		for (int y = height - 3; y < height; ++y) m.Set(tip, y, tip, Leaf), m.Set(tip + 1, y, tip, Leaf), m.Set(tip, y, tip + 1, Leaf), m.Set(tip + 1, y, tip + 1, Leaf);
		return m;
	}

	// A broadleaf tree: a trunk and a lumpy crown of overlapping balls.
	VoxelModel Broadleaf(uint32_t salt)
	{
		const int width = 26, height = 34;
		VoxelModel m = NewModel(width, height, width);
		Trunk2x2(m, 18);
		const float balls[][4] = { { 13, 22, 13, 9.0f }, { 8, 19, 12, 6.5f }, { 18, 20, 14, 6.5f }, { 13, 19, 8, 6.0f }, { 12, 27, 15, 6.0f } };
		for (const auto& b : balls)
			for (int y = 0; y < height; ++y)
				for (int z = 0; z < width; ++z)
					for (int x = 0; x < width; ++x)
					{
						const float dx = x + 0.5f - b[0], dy = (y + 0.5f - b[1]) * 1.15f, dz = z + 0.5f - b[2];
						if (dx * dx + dy * dy + dz * dz > b[3] * b[3]) continue;
						const float h = Hash01(x / 2, y / 2, z / 2, salt);
						m.Set(x, y, z, y < b[1] - 3 ? BroadDark : (h < 0.2f ? BroadLight : Broad));
					}
		return m;
	}

	// The far version of a conifer: meant to be baked with 1 m voxels.
	VoxelModel ConiferFar()
	{
		VoxelModel m = NewModel(5, 10, 5);
		for (int y = 0; y < 3; ++y) m.Set(2, y, 2, Trunk);
		const float radii[] = { 2.6f, 2.6f, 2.0f, 2.0f, 1.5f, 1.5f, 1.0f, 0.6f };
		for (int i = 0; i < 8; ++i)
			for (int z = 0; z < 5; ++z)
				for (int x = 0; x < 5; ++x)
				{
					const float dx = x - 2.0f, dz = z - 2.0f;
					if (dx * dx + dz * dz <= radii[i] * radii[i]) m.Set(x, 2 + i, z, i % 2 ? Leaf : LeafDark);
				}
		return m;
	}

	bool Write(const fs::path& folder, const char* name, const VoxelModel& model)
	{
		std::string error;
		const fs::path file = folder / name;
		if (!SaveVox(file, model, error))
		{
			std::printf("FAILED %s: %s\n", file.generic_string().c_str(), error.c_str());
			return false;
		}
		std::printf("wrote %s (%ux%ux%u, %zu voxels)\n", file.generic_string().c_str(), model.size.x, model.size.y, model.size.z, model.SolidCount());
		return true;
	}
}

int main(int argc, char** argv)
{
	const fs::path content = argc > 1 ? fs::path(argv[1]) : fs::path(UNDERTIME_CONTENT_DIR);
	const fs::path folder = content / "Models" / "Placeholders";
	bool ok = true;
	ok &= Write(folder, "ConiferSmall.vox", Conifer(26, 6.5f, 3, 1));
	ok &= Write(folder, "ConiferMedium.vox", Conifer(38, 8.5f, 3, 2));
	ok &= Write(folder, "ConiferLarge.vox", Conifer(54, 10.5f, 4, 3));
	ok &= Write(folder, "Broadleaf.vox", Broadleaf(4));
	ok &= Write(folder, "ConiferFar.vox", ConiferFar());
	return ok ? 0 : 1;
}
