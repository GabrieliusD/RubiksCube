// Tests for UNDERTIME's pure voxel/physics helpers (no GPU, no physics simulation).
#include "../VoxelWorld.h"
#include "../Fragments.h"
#include "../Structure.h"
#include "../CaveGenerator.h"
#include "../LooseBlocks.h"
#include <queue>
#include <climits>
#include <cmath>
#include <cstdio>
#include <random>
#include <unordered_set>

using namespace DirectX;
using cfg::Block;

int ValleyTests();   // ValleyTests.cpp
int ClockTests();    // ClockTests.cpp
int LightTests();    // LightTests.cpp

namespace
{
	int gFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gFailures;
	}

	FragmentVoxels RandomFragment(std::mt19937& rng, int n, float fill)
	{
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		FragmentVoxels f;
		f.size = { n, n + 1, n + 2 };
		f.voxels.resize(static_cast<size_t>(f.size.x) * f.size.y * f.size.z);
		for (auto& v : f.voxels) v = unit(rng) < fill ? Block::Stone : Block::Air;
		return f;
	}

	bool BoxesCoverExactly(const FragmentVoxels& f, const std::vector<PhysicsBox>& boxes)
	{
		const float s = cfg::kVoxelSize;
		for (int z = 0; z < f.size.z; ++z)
		for (int y = 0; y < f.size.y; ++y)
		for (int x = 0; x < f.size.x; ++x)
		{
			const XMFLOAT3 c{ (x + 0.5f) * s, (y + 0.5f) * s, (z + 0.5f) * s };
			int inside = 0;
			for (const PhysicsBox& b : boxes)
			{
				if (std::fabs(c.x - b.center.x) < b.halfExtents.x && std::fabs(c.y - b.center.y) < b.halfExtents.y && std::fabs(c.z - b.center.z) < b.halfExtents.z)
					++inside;
			}
			const bool solid = f.At(x, y, z) != Block::Air;
			if (inside != (solid ? 1 : 0)) return false;
		}
		return true;
	}

	// Carves a room in front of the chamber wall and leaves a 1 m block inside it.
	// `bridge` keeps a 1-voxel column joining the block to the ceiling.
	VoxelWorld::Box MakeHangingBlock(VoxelWorld& world, bool bridge, int& blockVoxels)
	{
		const int V = cfg::kVoxelsPerMeter;
		VoxelWorld::Box room;
		room.min[0] = 9 * V;  room.max[0] = 12 * V - 1;
		room.min[1] = 1 * V;  room.max[1] = 4 * V - 1;
		room.min[2] = 12 * V; room.max[2] = 15 * V - 1;
		const int bx0 = 10 * V, by0 = 2 * V, bz0 = 13 * V;
		blockVoxels = 0;
		for (int z = room.min[2]; z <= room.max[2]; ++z)
		for (int y = room.min[1]; y <= room.max[1]; ++y)
		for (int x = room.min[0]; x <= room.max[0]; ++x)
		{
			const bool inBlock = x >= bx0 && x < bx0 + V && y >= by0 && y < by0 + V && z >= bz0 && z < bz0 + V;
			const bool inBridge = bridge && x == bx0 && z == bz0 && y >= by0 + V;
			world.Set(x, y, z, (inBlock || inBridge) ? Block::Stone : Block::Air);
			if (inBlock) ++blockVoxels;
		}
		return room;
	}
}

namespace
{
	constexpr int V = cfg::kVoxelsPerMeter;
	int Vox(float metres) { return static_cast<int>(std::floor(metres * V)); }

	// Air voxels reachable from (x, y, z), 6-connected.
	std::vector<uint8_t> FloodAir(const VoxelWorld& world, int x, int y, int z)
	{
		std::vector<uint8_t> seen(world.Voxels().size(), 0);
		if (world.Get(x, y, z) != Block::Air) return seen;
		std::vector<int> stack = { VoxelWorld::Index(x, y, z) };
		seen[stack.back()] = 1;
		static const int d[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
		while (!stack.empty())
		{
			const int i = stack.back(); stack.pop_back();
			const int cx = i % cfg::kVoxX, cy = (i / cfg::kVoxX) % cfg::kVoxY, cz = i / (cfg::kVoxX * cfg::kVoxY);
			for (const auto& s : d)
			{
				const int nx = cx + s[0], ny = cy + s[1], nz = cz + s[2];
				if (!VoxelWorld::InGrid(nx, ny, nz) || world.Get(nx, ny, nz) != Block::Air) continue;
				const int n = VoxelWorld::Index(nx, ny, nz);
				if (!seen[n]) { seen[n] = 1; stack.push_back(n); }
			}
		}
		return seen;
	}

	bool Seen(const std::vector<uint8_t>& seen, const XMFLOAT3& p)
	{
		const int x = Vox(p.x), y = Vox(p.y), z = Vox(p.z);
		return VoxelWorld::InGrid(x, y, z) && seen[VoxelWorld::Index(x, y, z)];
	}

	// A place the 1.7 m player can stand: air with rock below and headroom above.
	bool CanStand(const VoxelWorld& world, int x, int y, int z)
	{
		if (!world.IsSolid(x, y - 1, z)) return false;
		for (int h = 0; h < 7; ++h) if (world.IsSolid(x, y + h, z)) return false;
		return true;
	}

	// Coarse walk check: steps to neighbouring columns climbing at most one voxel (the
	// character's step-up) or dropping up to three metres.
	bool Walkable(const VoxelWorld& world, const XMFLOAT3& from, const XMFLOAT3& to)
	{
		auto findStand = [&](const XMFLOAT3& p, int& sx, int& sy, int& sz)
		{
			sx = Vox(p.x); sz = Vox(p.z);
			for (int dy = 0; dy <= 6; ++dy)
			{
				for (int sign : { 1, -1 })
				{
					sy = Vox(p.y) + sign * dy;
					if (CanStand(world, sx, sy, sz)) return true;
				}
			}
			return false;
		};
		int x, y, z, gx, gy, gz;
		if (!findStand(from, x, y, z) || !findStand(to, gx, gy, gz)) return false;

		std::vector<uint8_t> seen(world.Voxels().size(), 0);
		std::queue<int> open;
		open.push(VoxelWorld::Index(x, y, z));
		seen[open.front()] = 1;
		static const int d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
		int closest = INT_MAX, closestX = 0, closestY = 0, closestZ = 0;
		while (!open.empty())
		{
			const int i = open.front(); open.pop();
			const int cx = i % cfg::kVoxX, cy = (i / cfg::kVoxX) % cfg::kVoxY, cz = i / (cfg::kVoxX * cfg::kVoxY);
			if (std::abs(cx - gx) <= V && std::abs(cz - gz) <= V && std::abs(cy - gy) <= V) return true;
			const int distance = std::abs(cx - gx) + std::abs(cz - gz) + 2 * std::abs(cy - gy);
			if (distance < closest) { closest = distance; closestX = cx; closestY = cy; closestZ = cz; }
			for (const auto& s : d)
			{
				for (int dy = 1; dy >= -3 * V; --dy)
				{
					const int nx = cx + s[0], ny = cy + dy, nz = cz + s[1];
					if (!VoxelWorld::InGrid(nx, ny, nz) || !CanStand(world, nx, ny, nz)) continue;
					if (dy == 1 && world.IsSolid(cx, cy + 7, cz)) continue;   // no headroom to step up
					const int n = VoxelWorld::Index(nx, ny, nz);
					if (!seen[n]) { seen[n] = 1; open.push(n); }
				}
			}
		}
		std::printf("          walk stopped nearest the goal at (%.2f, %.2f, %.2f) m; goal (%.2f, %.2f, %.2f)\n",
			closestX * 0.25f, closestY * 0.25f, closestZ * 0.25f, gx * 0.25f, gy * 0.25f, gz * 0.25f);
		return false;
	}

	// Rock not connected to the bedrock shell (would collapse immediately).
	int FloatingRock(const VoxelWorld& world)
	{
		const auto& v = world.Voxels();
		std::vector<uint8_t> seen(v.size(), 0);
		std::vector<int> stack = { 0 };
		seen[0] = 1;
		static const int d[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
		while (!stack.empty())
		{
			const int i = stack.back(); stack.pop_back();
			const int cx = i % cfg::kVoxX, cy = (i / cfg::kVoxX) % cfg::kVoxY, cz = i / (cfg::kVoxX * cfg::kVoxY);
			for (const auto& s : d)
			{
				const int nx = cx + s[0], ny = cy + s[1], nz = cz + s[2];
				if (!VoxelWorld::InGrid(nx, ny, nz)) continue;
				const int n = VoxelWorld::Index(nx, ny, nz);
				if (!seen[n] && v[n] != Block::Air) { seen[n] = 1; stack.push_back(n); }
			}
		}
		int floating = 0;
		for (size_t i = 0; i < v.size(); ++i) floating += (v[i] != Block::Air && !seen[i]) ? 1 : 0;
		return floating;
	}

	void CaveGenerationTests()
	{
#if defined(NDEBUG)
		const int kSeeds = 20;
#else
		const int kSeeds = 3;
#endif
		{
			VoxelWorld a, b, c;
			GenerateCaves(a, 42);
			GenerateCaves(b, 42);
			GenerateCaves(c, 43);
			Check(a.Hash() == b.Hash() && a.Hash() != c.Hash(), "caves: same seed gives the same mountain, a different seed differs");
		}

		int entranceOk = 0, zonesConnected = 0, sealsHold = 0, walkable = 0, pocketsSealed = 0, solidOk = 0;
		int oreInBand = 0, oreAllTypes = 0;
		int zoneTotal = 0, pocketTotal = 0, walkTotal = 0;
		int shaftsOpen = 0, shaftTotal = 0;
		for (int i = 0; i < kSeeds; ++i)
		{
			VoxelWorld world;
			const CaveLayout layout = GenerateCaves(world, 1000 + i * 7919);

			const std::vector<uint8_t> fromEntrance = FloodAir(world, Vox(cfg::kCenterX + 0.5f), Vox(1.5f), 0);
			const CaveRoom& firstRoom = layout.zones[0].rooms[0];
			entranceOk += Seen(fromEntrance, { firstRoom.center.x, firstRoom.center.y + 0.5f, firstRoom.center.z }) ? 1 : 0;

			std::vector<uint8_t> anyZone(world.Voxels().size(), 0);
			for (size_t zi = 0; zi < layout.zones.size(); ++zi)
			{
				const CaveZone& zone = layout.zones[zi];
				++zoneTotal;
				const CaveRoom& entry = zone.rooms[0];
				const CaveRoom& exit = zone.rooms[1];
				const auto seen = FloodAir(world, Vox(entry.center.x), Vox(entry.center.y + 0.5f), Vox(entry.center.z));
				zonesConnected += Seen(seen, { exit.center.x, exit.center.y + 0.5f, exit.center.z }) ? 1 : 0;
				// Its natural shafts: open at the grid's top, down to a cavern of this zone's network.
				for (const CaveShaft& shaft : layout.shafts)
				{
					if (shaft.zone != static_cast<int>(zi)) continue;
					++shaftTotal;
					const bool top = world.Get(Vox(shaft.top.x), cfg::kVoxY - 1, Vox(shaft.top.z)) == Block::Air;
					const bool reached = Seen(seen, { shaft.top.x, shaft.top.y - 0.1f, shaft.top.z }) && Seen(seen, shaft.bottom);
					shaftsOpen += top && reached ? 1 : 0;
				}

				// Nothing reachable beyond this zone's far end (the seal holds).
				bool contained = true;
				const int limit = static_cast<int>(std::ceil(zone.z1 * V));
				for (size_t v = 0; v < seen.size(); ++v)
				{
					if (!seen[v]) continue;
					anyZone[v] = 1;
					if (static_cast<int>(v / (cfg::kVoxX * cfg::kVoxY)) >= limit) contained = false;
				}
				sealsHold += contained ? 1 : 0;

				++walkTotal;
				const bool ok = Walkable(world, RoomStandPoint(entry), RoomStandPoint(exit));
				walkable += ok ? 1 : 0;
				if (!ok)
				{
					std::printf("        (seed %d zone %zu: no walkable path; %.1f-%.1f m)\n", 1000 + i * 7919, zi + 1, zone.z0, zone.z1);
					for (size_t r = 0; r < zone.rooms.size(); ++r)
					{
						const CaveRoom& room = zone.rooms[r];
						std::printf("          room %zu  x %.1f  floor %.1f  z %.1f  radius %.1f  height %.1f\n", r, room.center.x, room.center.y, room.center.z, room.radius, room.height);
					}
					for (const auto& t : zone.tunnels) std::printf("          tunnel %d-%d\n", t.first, t.second);
				}
			}
			for (const CavePocket& pocket : layout.pockets)
			{
				++pocketTotal;
				const XMFLOAT3 inside{ pocket.shape.center.x, pocket.shape.center.y + 0.5f, pocket.shape.center.z };
				const bool hollow = world.Get(Vox(inside.x), Vox(inside.y), Vox(inside.z)) == Block::Air;
				pocketsSealed += (hollow && !Seen(anyZone, inside) && !Seen(fromEntrance, inside)) ? 1 : 0;
			}
			solidOk += FloatingRock(world) == 0 ? 1 : 0;

			// Ore only within its depth band; every ore type and some relics present.
			bool inBand = true;
			int found[static_cast<int>(Block::Count)] = {};
			const auto& voxels = world.Voxels();
			for (size_t v = 0; v < voxels.size(); ++v)
			{
				const Block b = voxels[v];
				if (b == Block::Air || b == Block::Stone || cfg::Unbreakable(b)) continue;
				++found[static_cast<int>(b)];
				const float depth = static_cast<float>(v / (cfg::kVoxX * cfg::kVoxY)) / cfg::kVoxZ;
				if (b == Block::StrangeWatch)
				{
					if (depth < cfg::kWatchMinDepth - 0.01f) inBand = false;
					continue;
				}
				for (const cfg::OreRule& rule : cfg::kOreRules)
					if (rule.block == static_cast<int>(b) && (depth < rule.minDepth - 0.001f || depth > rule.maxDepth + 0.001f)) inBand = false;
			}
			bool allTypes = found[static_cast<int>(Block::StrangeWatch)] > 0;
			for (const cfg::OreRule& rule : cfg::kOreRules) allTypes = allTypes && found[rule.block] > 0;
			oreInBand += inBand ? 1 : 0;
			oreAllTypes += allTypes ? 1 : 0;
			if (i == 0)
			{
				std::printf("        (seed %d: %d veins, %zu watches; copper %d, silver %d, gold %d, crystal %d voxels)\n", 1000, layout.veins, layout.watches.size(),
					found[static_cast<int>(Block::Copper)], found[static_cast<int>(Block::Silver)], found[static_cast<int>(Block::Gold)], found[static_cast<int>(Block::TimeCrystal)]);
			}
		}
		{
			char oreText[128];
			std::snprintf(oreText, sizeof(oreText), "caves: ore stays inside its depth band (%d/%d seeds)", oreInBand, kSeeds);
			Check(oreInBand == kSeeds, oreText);
			std::snprintf(oreText, sizeof(oreText), "caves: every ore type and some relics appear (%d/%d seeds)", oreAllTypes, kSeeds);
			Check(oreAllTypes == kSeeds, oreText);
		}
		char text[160];
		std::snprintf(text, sizeof(text), "caves: the entrance reaches zone 1 (%d/%d seeds)", entranceOk, kSeeds);
		Check(entranceOk == kSeeds, text);
		std::snprintf(text, sizeof(text), "caves: every zone's entry cavern connects to its exit cavern (%d/%d)", zonesConnected, zoneTotal);
		Check(zonesConnected == zoneTotal, text);
		std::snprintf(text, sizeof(text), "caves: no zone's air leaks past its seal (%d/%d)", sealsHold, zoneTotal);
		Check(sealsHold == zoneTotal, text);
		std::snprintf(text, sizeof(text), "caves: entry to exit is walkable in every zone (%d/%d)", walkable, walkTotal);
		Check(walkable == walkTotal, text);
		std::snprintf(text, sizeof(text), "caves: side pockets are hollow and sealed off (%d/%d)", pocketsSealed, pocketTotal);
		Check(pocketTotal > 0 && pocketsSealed == pocketTotal, text);
		std::snprintf(text, sizeof(text), "caves: every natural shaft opens to the sky and down into its zone (%d/%d)", shaftsOpen, shaftTotal);
		Check(shaftTotal == kSeeds * static_cast<int>(std::size(cfg::kShafts)) && shaftsOpen == shaftTotal, text);
		std::snprintf(text, sizeof(text), "caves: no floating rock at the start (%d/%d seeds)", solidOk, kSeeds);
		Check(solidOk == kSeeds, text);
	}
}

int main()
{
	std::mt19937 rng(1234);

	{
		// Cracking: shallow stone pops in one hit, deep stone cracks for a few hits first.
		auto hitsToBreak = [](float zMetres, bool& crackedFirst)
		{
			VoxelWorld world;
			world.GenerateBasic();
			const int V = cfg::kVoxelsPerMeter;
			const int x = 10 * V, y = 5 * V, z = static_cast<int>(zMetres * V);
			const XMFLOAT3 c{ (x + 0.5f) * cfg::kVoxelSize, (y + 0.5f) * cfg::kVoxelSize, (z + 0.5f) * cfg::kVoxelSize };
			crackedFirst = false;
			for (int hit = 1; hit <= 20; ++hit)
			{
				const VoxelWorld::CarveResult r = world.Carve(c, cfg::kPickSpeed[0], x, y, z, hit);
				if (hit == 1) crackedFirst = r.removed == 0 && !r.cracked.Empty() && world.CrackLevel(x, y, z) >= 0;
				if (world.Get(x, y, z) == Block::Air) return hit;
			}
			return 99;
		};
		bool shallowCracked = false, deepCracked = false;
		const int shallow = hitsToBreak(2.0f, shallowCracked);
		const int deep = hitsToBreak(150.0f, deepCracked);
		std::printf("        hits to break: %d at 2 m, %d at 150 m\n", shallow, deep);
		Check(shallow == 1, "cracking: stone near the entrance breaks in one hit");
		Check(deep >= 3 && deep <= 8 && deepCracked, "cracking: deep stone cracks first and breaks after 3-8 hits");
	}

	{
		bool ok = true;
		for (int i = 0; i < 30 && ok; ++i)
		{
			const FragmentVoxels f = RandomFragment(rng, 2 + i % 5, 0.6f);
			ok = BoxesCoverExactly(f, DecomposeIntoBoxes(f, 100000));
		}
		Check(ok, "greedy boxes cover exactly the solid voxels, no overlaps (30 random fragments)");
	}
	{
		FragmentVoxels solid;
		solid.size = { 6, 6, 6 };
		solid.voxels.assign(216, Block::Stone);
		Check(DecomposeIntoBoxes(solid, 48).size() == 1, "a solid block becomes a single box");
	}
	{
		const FragmentVoxels f = RandomFragment(rng, 10, 0.5f);
		Check(DecomposeIntoBoxes(f, 100000).size() > 8, "a noisy fragment needs many boxes at full resolution");
		Check(DecomposeIntoBoxes(f, 8).size() <= 8, "the box cap coarsens the shape until it fits");
	}
	{
		// A real bite out of a flat wall (rock at z >= 40, air in front): every mined voxel
		// becomes one block, all in the air in front of the face and none on top of another.
		VoxelWorld world;
		world.GenerateBasic();
		for (int z = 20; z < 60; ++z)
			for (int y = 4; y < 40; ++y)
				for (int x = 4; x < 40; ++x) world.Set(x, y, z, z < 40 ? Block::Air : Block::Stone);
		const float s = cfg::kVoxelSize;
		const XMFLOAT3 center{ 20.5f * s, 20.5f * s, 40.0f * s + 0.08f };
		const VoxelWorld::CarveResult bite = world.Carve(center, 50.0f, 20, 20, 40, 1);   // a strong pick: breaks the whole bite
		auto isFree = [&](int x, int y, int z) { return world.Get(x, y, z) == Block::Air; };
		const std::vector<XMINT3> cells = MirrorOutOfFace(bite.voxels, 2, -1, 40, isFree);
		std::unordered_set<int> distinct;
		bool inFront = true;
		for (const XMINT3& c : cells)
		{
			distinct.insert(VoxelWorld::Index(c.x, c.y, c.z));
			inFront = inFront && c.z < 40;
		}
		char text[160];
		std::snprintf(text, sizeof(text), "loose blocks: a %d-voxel bite becomes %d blocks, all in front of the face, none overlapping",
			static_cast<int>(bite.voxels.size()), static_cast<int>(cells.size()));
		Check(bite.voxels.size() > 10 && cells.size() == bite.voxels.size() && distinct.size() == cells.size() && inFront, text);

		// In a corner (rock in front too) the blocks that can't mirror stay in their own cells.
		auto never = [](int, int, int) { return false; };
		const std::vector<XMINT3> stay = MirrorOutOfFace(bite.voxels, 2, -1, 40, never);
		bool same = true;
		for (size_t i = 0; i < stay.size(); ++i) same = same && stay[i].x == bite.voxels[i].x && stay[i].y == bite.voxels[i].y && stay[i].z == bite.voxels[i].z;
		Check(same, "loose blocks: with no room in front, a block falls out of its own (now empty) cell");
	}
	{
		// Pile cells: kPileCell metres per side, negative coordinates included (blocks in
		// front of the face have z < 0), and distinct cells never share a key.
		const float c = cfg::kPileCell;
		const bool sameCell = PileCellKey({ 0.1f, 0.1f, 0.1f }) == PileCellKey({ c - 0.01f, c - 0.01f, c - 0.01f });
		const bool edges = PileCellKey({ c - 0.01f, 0.5f, 0.5f }) != PileCellKey({ c + 0.01f, 0.5f, 0.5f });
		const bool negative = PileCellKey({ 0.5f, 0.5f, -0.01f }) != PileCellKey({ 0.5f, 0.5f, 0.01f })
			&& PileCellKey({ 0.5f, 0.5f, -0.01f }) == PileCellKey({ 0.5f, 0.5f, -c + 0.01f });
		std::unordered_set<int64_t> keys;
		for (int x = -3; x <= 3; ++x)
			for (int y = -3; y <= 3; ++y)
				for (int z = -3; z <= 3; ++z) keys.insert(PileCellKey({ (x + 0.5f) * c, (y + 0.5f) * c, (z + 0.5f) * c }));
		Check(sameCell && edges && negative && keys.size() == 343, "loose blocks: pile cells split space into kPileCell cubes with distinct keys");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, false, blockVoxels);
		DetachResult result = DetachUnsupported(world, room, cfg::kSupportSearchBudget);
		Check(result.fragments.size() == 1 && result.fragments[0].SolidCount() == blockVoxels, "support: a block hanging in a room detaches as one fragment");
		Check(world.Get(10 * cfg::kVoxelsPerMeter, 2 * cfg::kVoxelsPerMeter, 13 * cfg::kVoxelsPerMeter) == Block::Air, "support: detached voxels are removed from the world");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, true, blockVoxels);
		Check(DetachUnsupported(world, room, cfg::kSupportSearchBudget).fragments.empty(), "support: a one-voxel bridge to the ceiling holds");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		int blockVoxels = 0;
		const VoxelWorld::Box room = MakeHangingBlock(world, false, blockVoxels);
		Check(DetachUnsupported(world, room, blockVoxels / 2).fragments.empty(), "support: regions larger than the budget count as anchored");
	}
	{
		VoxelWorld world;
		world.GenerateBasic();
		// Greedy collision faces must cover the same area as the exposed voxel faces.
		const int z0 = 0, z1 = 16;
		std::vector<XMFLOAT3> tris;
		world.BuildCollisionTriangles(z0, z1, tris);
		double area = 0.0;
		bool outward = true;
		for (size_t i = 0; i + 2 < tris.size(); i += 3)
		{
			const XMVECTOR a = XMLoadFloat3(&tris[i]), b = XMLoadFloat3(&tris[i + 1]), c = XMLoadFloat3(&tris[i + 2]);
			const XMVECTOR n = XMVector3Cross(b - a, c - a);
			area += 0.5 * XMVectorGetX(XMVector3Length(n));
			// Step from the triangle's centre along its normal: that must be air.
			XMFLOAT3 probe;
			XMStoreFloat3(&probe, (a + b + c) / 3.0f + XMVector3Normalize(n) * (cfg::kVoxelSize * 0.5f));
			const int V = cfg::kVoxelsPerMeter;
			if (world.IsSolid(static_cast<int>(std::floor(probe.x * V)), static_cast<int>(std::floor(probe.y * V)), static_cast<int>(std::floor(probe.z * V)))) outward = false;
		}
		int faces = 0;
		for (int z = z0; z < z1; ++z)
		for (int y = 0; y < cfg::kVoxY; ++y)
		for (int x = 0; x < cfg::kVoxX; ++x)
		{
			if (world.Get(x, y, z) == Block::Air) continue;
			faces += !world.IsSolid(x + 1, y, z) + !world.IsSolid(x - 1, y, z) + !world.IsSolid(x, y + 1, z)
				+ !world.IsSolid(x, y - 1, z) + !world.IsSolid(x, y, z + 1) + !world.IsSolid(x, y, z - 1);
		}
		const double expected = faces * cfg::kVoxelSize * cfg::kVoxelSize;
		Check(std::fabs(area - expected) < 1e-3 * expected, "collision: greedy faces cover the same area as the exposed voxel faces");
		Check(outward, "collision: every triangle faces into air");
		std::printf("        (%zu triangles for %d voxel faces)\n", tris.size() / 3, faces);
	}

	CaveGenerationTests();
	gFailures += ValleyTests();
	gFailures += ClockTests();
	gFailures += LightTests();

	std::printf("%s (%d failure%s)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
