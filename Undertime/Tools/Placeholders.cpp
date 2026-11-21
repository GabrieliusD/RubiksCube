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

	// ---- Camp buildings and props ------------------------------------------------------------
	// Their own palette; the fronts face -z (towards the viewer in the model's default turn).
	enum : uint8_t
	{
		BLog = 1, BLogDark, BPlank, BPlankDark, BRoof, BRoofDark, BCanvas, BCanvasDark, BStone, BStoneDark,
		BGlass, BFire, BEmber, BIron, BSign, BCopper, BWindowLit, BClock, BHand, BSlate, BSlateDark,
	};

	VoxelModel NewBuilding(uint32_t x, uint32_t y, uint32_t z)
	{
		VoxelModel m;
		m.Resize({ x, y, z });
		const uint32_t colours[] = { 0, 0x7a5232, 0x5e3f27, 0xa07c52, 0x6b4a2b, 0x4f3a28, 0x3d2c1f, 0xd9ceb0, 0x4a4035, 0x8a8680, 0x6c6a66,
			0x1c2530, 0xff9a3c, 0xc8431c, 0x3a3a3e, 0xd8c8a0, 0xc8743a, 0xffc477, 0xf0ead8, 0x1c1a18, 0x46505e, 0x3a434f };
		for (int i = 1; i < static_cast<int>(std::size(colours)); ++i) m.palette[i] = Srgb(colours[i]);
		m.materials[BFire].emissive = 4.0f;
		m.materials[BEmber].emissive = 1.5f;
		m.materials[BWindowLit].emissive = 2.2f;
		return m;
	}

	void Fill(VoxelModel& m, int x0, int y0, int z0, int x1, int y1, int z1, uint8_t c)
	{
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x) m.Set(x, y, z, c);
	}

	// A stepped gable roof, one voxel thick. ridgeAlongX: the ridge runs along x and the roof
	// slopes down towards z0 and z1 (else along z, sloping towards x0 and x1). The gable
	// ends are filled with `gable`.
	void GableRoof(VoxelModel& m, bool ridgeAlongX, int a0, int a1, int b0, int b1, int y0, uint8_t roof, uint8_t roof2, uint8_t gable)
	{
		for (int k = 0; ; ++k)
		{
			const int lo = b0 - 1 + k, hi = b1 + 1 - k;
			if (lo > hi) break;
			for (int a = a0 - 1; a <= a1 + 1; ++a)
			{
				const uint8_t c = (k % 2) ? roof : roof2;
				if (ridgeAlongX) { m.Set(a, y0 + k, lo, c); m.Set(a, y0 + k, hi, c); }
				else { m.Set(lo, y0 + k, a, c); m.Set(hi, y0 + k, a, c); }
			}
			for (int b = lo + 1; b < hi; ++b)
			{
				if (ridgeAlongX) { m.Set(a0, y0 + k, b, gable); m.Set(a1, y0 + k, b, gable); }
				else { m.Set(b, y0 + k, a0, gable); m.Set(b, y0 + k, a1, gable); }
			}
		}
	}

	// A log cabin trading post with a porch, an awning and a sign (the Camp era's only shop).
	VoxelModel TradingPost()
	{
		VoxelModel m = NewBuilding(34, 26, 40);
		const int x0 = 1, x1 = 32, z0 = 9, z1 = 38, wall = 12;
		for (int y = 0; y < wall; ++y)
		{
			const uint8_t log = (y / 2) % 2 ? BLog : BLogDark;
			Fill(m, x0, y, z0, x1, y, z0, log);
			Fill(m, x0, y, z1, x1, y, z1, log);
			Fill(m, x0, y, z0, x0, y, z1, log);
			Fill(m, x1, y, z0, x1, y, z1, log);
			if ((y / 2) % 2) { m.Set(x0 - 1, y, z0, BLogDark); m.Set(x1 + 1, y, z0, BLogDark); m.Set(x0 - 1, y, z1, BLogDark); m.Set(x1 + 1, y, z1, BLogDark); }
		}
		Fill(m, x0 + 1, 0, z0 + 1, x1 - 1, 0, z1 - 1, BPlank);
		Fill(m, 14, 1, z0, 19, 9, z0, BPlankDark);                      // the door
		Fill(m, 13, 10, z0, 20, 10, z0, BLogDark);
		for (int wx : { 5, 25 }) Fill(m, wx, 4, z0, wx + 3, 7, z0, BGlass);
		for (int wz : { 16, 28 }) { Fill(m, x0, 4, wz, x0, 7, wz + 3, BGlass); Fill(m, x1, 4, wz, x1, 7, wz + 3, BGlass); }
		GableRoof(m, true, x0, x1, z0, z1, wall, BRoof, BRoofDark, BLog);
		// Porch: floor, posts and a flat awning, a sign hanging from it and crates.
		Fill(m, 2, 0, 1, 31, 0, z0 - 1, BPlank);
		for (int px : { 2, 31 }) Fill(m, px, 1, 1, px, 9, 1, BLogDark);
		Fill(m, 1, 10, 0, 32, 10, z0 - 1, BRoofDark);
		Fill(m, 11, 7, 0, 22, 9, 0, BSign);
		Fill(m, 15, 8, 0, 18, 8, 0, BCopper);
		Fill(m, 4, 1, 3, 7, 4, 6, BPlank);
		Fill(m, 4, 4, 3, 7, 4, 6, BPlankDark);
		Fill(m, 25, 1, 4, 28, 3, 7, BPlank);
		return m;
	}

	// A canvas A-frame tent with its door flap open (dark inside).
	VoxelModel Tent()
	{
		VoxelModel m = NewBuilding(15, 11, 18);
		const int c = 7;
		for (int y = 0; y <= 8; ++y)
		{
			const int half = 7 - (y * 7) / 9;
			for (int z = 1; z <= 16; ++z)
			{
				m.Set(c - half, y, z, y % 3 == 2 ? BCanvasDark : BCanvas);
				m.Set(c + half, y, z, BCanvas);
			}
			for (int x = c - half; x <= c + half; ++x) { m.Set(x, y, 16, BCanvas); m.Set(x, y, 1, BCanvas); }
		}
		Fill(m, c - 2, 0, 1, c + 2, 4, 1, BCanvasDark);   // the open flap
		for (int z : { 0, 17 }) Fill(m, c, 0, z, c, 9, z, BLogDark);
		Fill(m, c, 9, 0, c, 9, 17, BLogDark);
		return m;
	}

	// Stones in a ring, crossed logs, embers and a flame.
	VoxelModel Campfire()
	{
		VoxelModel m = NewBuilding(9, 6, 9);
		for (int z = 0; z < 9; ++z)
			for (int x = 0; x < 9; ++x)
			{
				const float d = std::hypot(x - 4.0f, z - 4.0f);
				if (d > 3.2f && d < 4.6f) m.Set(x, 0, z, (x + z) % 3 ? BStone : BStoneDark);
				else if (d < 2.0f) m.Set(x, 0, z, BEmber);
			}
		Fill(m, 2, 1, 4, 6, 1, 4, BLogDark);
		Fill(m, 4, 1, 2, 4, 1, 6, BLog);
		Fill(m, 3, 2, 3, 5, 2, 5, BFire);
		Fill(m, 4, 3, 3, 4, 4, 4, BFire);
		m.Set(4, 5, 4, BFire);
		return m;
	}

	VoxelModel Crates()
	{
		VoxelModel m = NewBuilding(9, 9, 9);
		auto crate = [&](int x, int y, int z)
		{
			Fill(m, x, y, z, x + 3, y + 3, z + 3, BPlank);
			for (int e = 0; e < 4; ++e) { m.Set(x + e, y + 3, z, BPlankDark); m.Set(x + e, y, z, BPlankDark); m.Set(x, y + e, z, BPlankDark); m.Set(x + 3, y + e, z, BPlankDark); }
		};
		crate(0, 0, 0);
		crate(4, 0, 2);
		crate(2, 4, 1);
		return m;
	}

	// Cut logs stacked three, two, one; light ends.
	VoxelModel LogPile()
	{
		VoxelModel m = NewBuilding(14, 9, 10);
		const float logs[][2] = { { 1.8f, 1.6f }, { 5.0f, 1.6f }, { 8.2f, 1.6f }, { 3.4f, 4.4f }, { 6.6f, 4.4f }, { 5.0f, 7.2f } };   // z, y centres
		for (const auto& log : logs)
			for (int y = 0; y < 9; ++y)
				for (int z = 0; z < 10; ++z)
				{
					if ((z + 0.5f - log[0]) * (z + 0.5f - log[0]) + (y + 0.5f - log[1]) * (y + 0.5f - log[1]) > 2.4f) continue;
					for (int x = 0; x < 14; ++x) m.Set(x, y, z, x == 0 || x == 13 ? BPlank : (y + x / 4) % 2 ? BLog : BLogDark);
				}
		return m;
	}

	// The blacksmith's open-air lean-to: a sloped roof on four posts, an anvil on a stump, a forge.
	VoxelModel SmithyLeanTo()
	{
		VoxelModel m = NewBuilding(16, 14, 14);
		for (int x : { 0, 15 })
		{
			Fill(m, x, 0, 0, x, 12, 0, BLogDark);
			Fill(m, x, 0, 13, x, 9, 13, BLogDark);
		}
		for (int z = 0; z < 14; ++z)
		{
			const int y = 12 - (z * 3) / 13;
			Fill(m, 0, y + 1, z, 15, y + 1, z, z % 2 ? BRoof : BRoofDark);
		}
		Fill(m, 1, 0, 13, 14, 9, 13, BPlankDark);   // back wall
		Fill(m, 7, 0, 5, 8, 2, 6, BLogDark);        // stump
		Fill(m, 6, 3, 5, 9, 4, 6, BIron);           // anvil
		m.Set(10, 4, 5, BIron);
		Fill(m, 11, 0, 8, 14, 4, 12, BStone);       // forge
		Fill(m, 12, 4, 9, 13, 4, 11, BEmber);
		m.Set(12, 5, 10, BFire);
		return m;
	}

	// The clockmaker's cottage: stone, a steep slate roof with a clock in its gable, windows
	// that are always lit. It never changes, in any era.
	VoxelModel ClockmakerCottage()
	{
		VoxelModel m = NewBuilding(26, 32, 22);
		const int x0 = 1, x1 = 24, z0 = 1, z1 = 20, wall = 13;
		for (int y = 0; y < wall; ++y)
			for (int z = z0; z <= z1; ++z)
				for (int x = x0; x <= x1; ++x)
				{
					if (x != x0 && x != x1 && z != z0 && z != z1) continue;
					m.Set(x, y, z, Hash01(x / 2, y, z / 2, 9) < 0.35f ? BStoneDark : BStone);
				}
		Fill(m, x0 + 1, 0, z0 + 1, x1 - 1, 0, z1 - 1, BPlank);
		Fill(m, 11, 1, z0, 14, 9, z0, BPlankDark);
		for (int wx : { 4, 18 }) Fill(m, wx, 5, z0, wx + 3, 8, z0, BWindowLit);
		for (int wz : { 7, 14 }) { Fill(m, x0, 5, wz, x0, 8, wz + 2, BWindowLit); Fill(m, x1, 5, wz, x1, 8, wz + 2, BWindowLit); }
		GableRoof(m, false, z0, z1, x0, x1, wall, BSlate, BSlateDark, BStone);
		// The clock in the front gable.
		const float cx = 12.5f, cy = wall + 5.0f;
		for (int y = wall; y < wall + 11; ++y)
			for (int x = 0; x < 26; ++x)
			{
				const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
				if (d < 4.2f) m.Set(x, y, 0, d > 3.3f ? BHand : BClock);
			}
		for (int i = 0; i < 3; ++i) m.Set(12, static_cast<int>(cy) + i, 0, BHand);   // hands at ten to two
		for (int i = 1; i < 3; ++i) m.Set(12 + i, static_cast<int>(cy) - i / 2, 0, BHand);
		Fill(m, 19, wall, 15, 21, 30, 17, BStoneDark);   // chimney
		return m;
	}

	// A water wheel turning on an axle along x, its centre half-way up the model (spin it
	// around a parent placed at the axle).
	VoxelModel WaterWheel()
	{
		VoxelModel m = NewBuilding(4, 17, 17);
		const float c = 8.5f;
		for (int y = 0; y < 17; ++y)
			for (int z = 0; z < 17; ++z)
			{
				const float dy = y + 0.5f - c, dz = z + 0.5f - c, r = std::hypot(dy, dz);
				const float angle = std::atan2(dy, dz);
				const bool rim = r > 6.6f && r < 8.4f;
				const bool spoke = r < 6.6f && std::fabs(std::fmod(angle + 6.2831853f, 0.7853982f) - 0.39f) < 0.13f;
				const bool paddle = r > 5.0f && r < 8.4f && std::fabs(std::fmod(angle + 6.2831853f, 0.5235988f)) < 0.09f;
				if (r < 1.5f) Fill(m, 0, y, z, 3, y, z, BIron);
				else if (paddle) Fill(m, 0, y, z, 3, y, z, BPlank);
				else if (rim || spoke) { m.Set(0, y, z, BPlankDark); m.Set(3, y, z, BPlankDark); }
			}
		return m;
	}

	// ---- Settlement buildings and props -------------------------------------------------------
	// More building colours, appended to the building palette.
	enum : uint8_t { BWhite = BSlateDark + 1, BWhiteDark, BShingle, BShingleDark, BDirt, BCrop, BCropLight, BGrass };

	VoxelModel NewTown(uint32_t x, uint32_t y, uint32_t z)
	{
		VoxelModel m = NewBuilding(x, y, z);
		const uint32_t colours[] = { 0xe6dccb, 0xcfc4b0, 0x5a4a40, 0x4a3c34, 0x6b5238, 0x4c7a34, 0x6a9a40, 0x5f7d3a };
		for (int i = 0; i < static_cast<int>(std::size(colours)); ++i) m.palette[BWhite + i] = Srgb(colours[i]);
		return m;
	}

	struct BuildingSpec
	{
		int width = 24, depth = 28, wall = 12;   // voxels (front along x, at z = 0)
		bool whitewash = false;
		bool falseFront = false;
		int porch = 0;                // porch depth in front, voxels (0 = none)
		uint8_t sign = 0;             // sign board colour (0 = none)
		uint8_t signMark = 0;         // a mark on the sign
		bool chimney = false;
		bool openDoor = false;        // a wide dark opening (workshops)
		int floors = 1;               // rows of windows
	};

	// A plank (or whitewashed) building with a shingle roof; the ridge runs along x, so the
	// gable ends are the sides. An optional false front hides the roof from the street.
	VoxelModel PlankBuilding(const BuildingSpec& b)
	{
		const int x0 = 1, x1 = b.width, z0 = b.porch + 1, z1 = b.porch + b.depth;
		const int rise = b.depth / 2 + 2;
		const int height = b.wall + rise + (b.falseFront ? 6 : 2) + (b.chimney ? 6 : 0);
		VoxelModel m = NewTown(b.width + 2, height, z1 + 2);
		const uint8_t wallA = b.whitewash ? BWhite : BPlank, wallB = b.whitewash ? BWhiteDark : BPlankDark;
		for (int y = 0; y < b.wall; ++y)
		{
			const uint8_t c = (y / 2) % 2 ? wallA : wallB;
			Fill(m, x0, y, z0, x1, y, z0, c);
			Fill(m, x0, y, z1, x1, y, z1, c);
			Fill(m, x0, y, z0, x0, y, z1, c);
			Fill(m, x1, y, z0, x1, y, z1, c);
		}
		for (int y = 0; y < b.wall; ++y) { m.Set(x0, y, z0, BLogDark); m.Set(x1, y, z0, BLogDark); m.Set(x0, y, z1, BLogDark); m.Set(x1, y, z1, BLogDark); }
		Fill(m, x0 + 1, 0, z0 + 1, x1 - 1, 0, z1 - 1, BPlank);
		const int cx = (x0 + x1) / 2;
		if (b.openDoor) Fill(m, cx - 4, 1, z0, cx + 4, 9, z0, BCanvasDark);
		else Fill(m, cx - 2, 1, z0, cx + 2, 9, z0, BLogDark);
		for (int f = 0; f < b.floors; ++f)
		{
			const int wy = 4 + f * 9;
			if (wy + 3 >= b.wall) break;
			for (int wx = x0 + 3; wx + 3 < x1 - 2; wx += 8)
			{
				if (std::abs(wx + 2 - cx) < 5 && f == 0) continue;   // not over the door
				Fill(m, wx, wy, z0, wx + 3, wy + 3, z0, BGlass);
			}
			for (int wz = z0 + 4; wz + 3 < z1 - 2; wz += 9) { Fill(m, x0, wy, wz, x0, wy + 3, wz + 3, BGlass); Fill(m, x1, wy, wz, x1, wy + 3, wz + 3, BGlass); }
		}
		GableRoof(m, true, x0, x1, z0, z1, b.wall, BShingle, BShingleDark, wallA);
		if (b.falseFront)
		{
			const int top = b.wall + rise + 4;
			Fill(m, x0, b.wall, z0 - 1, x1, top, z0 - 1, wallA);
			for (int x = x0; x <= x1; ++x) m.Set(x, top, z0 - 1, BLogDark);
		}
		if (b.sign)
		{
			const int sy = b.falseFront ? b.wall + 2 : b.wall - 3;
			const int sz = b.falseFront ? z0 - 2 : z0 - 1;
			Fill(m, cx - 6, sy, sz, cx + 6, sy + 2, sz, b.sign);
			if (b.signMark) Fill(m, cx - 1, sy + 1, sz, cx + 1, sy + 1, sz, b.signMark);
		}
		if (b.porch > 0)
		{
			Fill(m, x0, 0, 1, x1, 0, z0 - 1, BPlank);
			for (int px : { x0, x1, cx - 8, cx + 8 }) if (px >= x0 && px <= x1) Fill(m, px, 1, 1, px, 9, 1, BLogDark);
			Fill(m, x0, 10, 0, x1, 10, z0 - 1, BShingleDark);
		}
		if (b.chimney) Fill(m, x1 - 5, b.wall, z1 - 6, x1 - 3, height - 1, z1 - 4, BStoneDark);
		return m;
	}

	// A fence around a plot with a gate in the middle of its front; vegetable rows or grass inside.
	VoxelModel GardenPlot(int w, int d, bool vegetables)
	{
		VoxelModel m = NewTown(w, 6, d);
		for (int x = 0; x < w; ++x)
			for (int z = 0; z < d; ++z)
			{
				const bool edge = x == 0 || z == 0 || x == w - 1 || z == d - 1;
				const bool gate = z == 0 && std::abs(x - w / 2) < 4;
				if (edge)
				{
					if (gate) continue;
					const bool post = (x % 8 == 0 || x == w - 1) && (z % 8 == 0 || z == d - 1 || x == 0 || x == w - 1);
					if (post || (x % 8 == 0 && (z == 0 || z == d - 1)) || (z % 8 == 0 && (x == 0 || x == w - 1))) Fill(m, x, 0, z, x, 5, z, BLogDark);
					m.Set(x, 2, z, BPlankDark);
					m.Set(x, 4, z, BPlankDark);
				}
				else if (vegetables && x > 2 && x < w - 3 && z > d / 2 && z < d - 3)
				{
					m.Set(x, 0, z, z % 3 == 0 ? BCrop : BDirt);
					if (z % 3 == 0 && x % 2 == 0) m.Set(x, 1, z, BCropLight);
				}
			}
		return m;
	}

	// Timber scaffolding around the stone base of the clock tower being built.
	VoxelModel ClockTowerScaffold()
	{
		VoxelModel m = NewTown(24, 50, 24);
		Fill(m, 2, 0, 2, 21, 7, 21, BStone);
		Fill(m, 4, 8, 4, 19, 11, 19, BStoneDark);
		for (int x : { 0, 11, 23 })
			for (int z : { 0, 11, 23 })
				if (x != 11 || z != 11) Fill(m, x, 0, z, x, 48, z, BLogDark);
		for (int y = 12; y <= 48; y += 12)
		{
			for (int i = 0; i < 24; ++i) { m.Set(i, y, 0, BPlank); m.Set(i, y, 23, BPlank); m.Set(0, y, i, BPlank); m.Set(23, y, i, BPlank); }
			Fill(m, 0, y, 0, 23, y, 2, BPlank);
		}
		for (int y = 1; y < 48; y += 2) m.Set(1, y, 12, BPlankDark);   // a ladder
		return m;
	}

	// A small whitewashed chapel with a belfry over its front.
	VoxelModel Chapel()
	{
		BuildingSpec b;
		b.width = 22; b.depth = 38; b.wall = 14; b.whitewash = true;
		VoxelModel base = PlankBuilding(b);
		VoxelModel m = NewTown(base.size.x, base.size.y + 16, base.size.z);
		for (uint32_t z = 0; z < base.size.z; ++z)
			for (uint32_t y = 0; y < base.size.y; ++y)
				for (uint32_t x = 0; x < base.size.x; ++x) m.Set(x, y, z, base.At(x, y, z));
		const int cx = 11, top = static_cast<int>(base.size.y) - 2;
		Fill(m, cx - 3, top - 6, 1, cx + 3, top + 4, 6, BWhite);
		Fill(m, cx - 2, top - 1, 1, cx + 2, top + 2, 1, BCanvasDark);   // the bell opening
		m.Set(cx, top, 2, BCopper);
		Fill(m, cx - 4, top + 5, 0, cx + 4, top + 5, 7, BShingleDark);
		Fill(m, cx, top + 6, 3, cx, top + 11, 3, BLogDark);             // the cross
		Fill(m, cx - 2, top + 9, 3, cx + 2, top + 9, 3, BLogDark);
		return m;
	}

	VoxelModel Grave()
	{
		VoxelModel m = NewTown(5, 7, 9);
		Fill(m, 0, 0, 2, 4, 0, 8, BDirt);
		Fill(m, 1, 1, 3, 3, 1, 7, BGrass);
		Fill(m, 2, 0, 0, 2, 6, 0, BPlank);
		Fill(m, 0, 4, 0, 4, 4, 0, BPlank);
		return m;
	}

	// The first sawmill: a saw pit under a roof on posts, a log across it, sawn planks stacked.
	VoxelModel SawPit()
	{
		VoxelModel m = NewTown(24, 16, 18);
		for (int x : { 0, 23 }) for (int z : { 0, 17 }) Fill(m, x, 0, z, x, 12, z, BLogDark);
		Fill(m, 0, 13, 0, 23, 13, 17, BShingle);
		Fill(m, 0, 14, 7, 23, 14, 10, BShingleDark);
		Fill(m, 6, 0, 6, 17, 0, 11, BDirt);                 // the pit
		for (int x : { 7, 16 }) Fill(m, x, 1, 7, x, 4, 10, BLogDark);   // trestles
		for (int z = 7; z <= 10; ++z) for (int y = 5; y <= 7; ++y) if ((z - 8.5f) * (z - 8.5f) + (y - 6) * (y - 6) < 3.0f) Fill(m, 2, y, z, 21, y, z, (y + z) % 2 ? BLog : BLogDark);
		Fill(m, 19, 1, 1, 22, 3, 5, BPlank);               // sawn planks
		return m;
	}

	// A plank footbridge (2 m wide, 11 m long along z) with rails, on posts.
	VoxelModel Footbridge()
	{
		VoxelModel m = NewTown(8, 12, 44);
		Fill(m, 0, 6, 0, 7, 6, 43, BPlank);
		for (int z = 0; z < 44; z += 2) Fill(m, 0, 6, z, 7, 6, z, BPlankDark);
		for (int x : { 0, 7 })
		{
			for (int z = 0; z < 44; z += 6) Fill(m, x, 7, z, x, 10, z, BLogDark);
			Fill(m, x, 10, 0, x, 10, 43, BLog);
		}
		for (int z : { 10, 33 }) for (int x : { 1, 6 }) Fill(m, x, 0, z, x, 5, z, BLogDark);   // posts into the riverbed
		return m;
	}

	// Timbers around the cave mouth: two posts and a lintel.
	VoxelModel MineFrame()
	{
		VoxelModel m = NewTown(12, 14, 2);
		for (int x : { 0, 1, 10, 11 }) Fill(m, x, 0, 0, x, 11, 1, BLogDark);
		Fill(m, 0, 12, 0, 11, 13, 1, BLog);
		return m;
	}

	VoxelModel Stump()
	{
		VoxelModel m = NewTown(3, 3, 3);
		Fill(m, 0, 0, 0, 2, 1, 2, BLogDark);
		Fill(m, 0, 2, 0, 2, 2, 2, BPlank);
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
	ok &= Write(folder, "TradingPost.vox", TradingPost());
	ok &= Write(folder, "Tent.vox", Tent());
	ok &= Write(folder, "Campfire.vox", Campfire());
	ok &= Write(folder, "Crates.vox", Crates());
	ok &= Write(folder, "LogPile.vox", LogPile());
	ok &= Write(folder, "SmithyLeanTo.vox", SmithyLeanTo());
	ok &= Write(folder, "ClockmakerCottage.vox", ClockmakerCottage());
	ok &= Write(folder, "WaterWheel.vox", WaterWheel());
	auto building = [&](const char* name, BuildingSpec spec) { ok &= Write(folder, name, PlankBuilding(spec)); };
	{ BuildingSpec b; b.width = 36; b.depth = 36; b.wall = 14; b.falseFront = true; b.porch = 8; b.sign = BSign; b.signMark = BGlass; building("GeneralStore.vox", b); }
	{ BuildingSpec b; b.width = 24; b.depth = 26; b.sign = BSign; b.signMark = BCopper; b.porch = 6; building("AssayOffice.vox", b); }
	{ BuildingSpec b; b.width = 24; b.depth = 26; b.whitewash = true; b.sign = BLogDark; b.signMark = BWhite; building("PostOffice.vox", b); }
	{ BuildingSpec b; b.width = 40; b.depth = 44; b.wall = 22; b.floors = 2; b.falseFront = true; b.porch = 8; b.sign = BSign; b.signMark = BFire; building("Saloon.vox", b); }
	{ BuildingSpec b; b.width = 30; b.depth = 30; b.openDoor = true; b.chimney = true; building("Blacksmith.vox", b); }
	{ BuildingSpec b; b.width = 22; b.depth = 20; b.wall = 11; b.chimney = true; building("Home.vox", b); }
	{ BuildingSpec b; b.width = 22; b.depth = 20; b.wall = 11; b.whitewash = true; b.chimney = true; building("HomeWhite.vox", b); }
	{ BuildingSpec b; b.width = 40; b.depth = 30; b.wall = 20; b.floors = 2; b.whitewash = true; b.chimney = true; b.porch = 6; building("FoundersHouse.vox", b); }
	ok &= Write(folder, "GardenPlot.vox", GardenPlot(40, 56, true));
	ok &= Write(folder, "FoundersPlot.vox", GardenPlot(72, 88, false));
	ok &= Write(folder, "ClockTowerScaffold.vox", ClockTowerScaffold());
	ok &= Write(folder, "Chapel.vox", Chapel());
	ok &= Write(folder, "Grave.vox", Grave());
	ok &= Write(folder, "SawPit.vox", SawPit());
	ok &= Write(folder, "Footbridge.vox", Footbridge());
	ok &= Write(folder, "MineFrame.vox", MineFrame());
	ok &= Write(folder, "Stump.vox", Stump());
	return ok ? 0 : 1;
}
