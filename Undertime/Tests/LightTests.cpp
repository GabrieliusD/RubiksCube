// Tests for daylight in the cave (Skylight): where it reaches, that digging lets it in the
// same as building it again would, and what the renderer gets.
#include "../Skylight.h"
#include "../CaveGenerator.h"
#include "../Config.h"
#include <cstdio>

namespace
{
	int gLightFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gLightFailures;
	}

	void Dig(VoxelWorld& world, int x0, int y0, int z0, int x1, int y1, int z1, VoxelWorld::Box& box)
	{
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x) world.Set(x, y, z, cfg::Block::Air);
		box.min[0] = x0; box.min[1] = y0; box.min[2] = z0;
		box.max[0] = x1; box.max[1] = y1; box.max[2] = z1;
	}

	bool Same(const Skylight& a, const Skylight& b)
	{
		for (int z = 0; z < Skylight::kZ; ++z)
			for (int y = 0; y < Skylight::kY; ++y)
				for (int x = 0; x < Skylight::kX; ++x)
					if (a.Level(x, y, z) != b.Level(x, y, z)) return false;
		return true;
	}
}

int LightTests()
{
	// ---- The basic world: a passage from the face, a chamber, solid rock beyond ------------
	{
		VoxelWorld world;
		world.GenerateBasic();
		Skylight sky;
		sky.Build(world);
		const float x = cfg::kCenterX + 0.5f;
		Check(sky.LevelAt(x, 1.5f, 0.2f) > 0.99f, "daylight: full at the mouth");
		Check(sky.LevelAt(x, 1.5f, 4.5f) < sky.LevelAt(x, 1.5f, 1.5f) && sky.LevelAt(x, 1.5f, 4.5f) > 0.0f, "daylight: it dims along the passage");
		Check(sky.LevelAt(x, 1.5f, 60.0f) == 0.0f && sky.LevelAt(10.0f, 12.0f, 20.0f) == 0.0f, "daylight: none in solid rock or past its reach");

		// Digging on from the chamber: the update matches building it all again.
		VoxelWorld::Box box;
		const int cx = cfg::kCenterX * cfg::kVoxelsPerMeter;
		Dig(world, cx, 4, 11 * 4, cx + 3, 11, 14 * 4, box);
		const Skylight::CellBox changed = sky.Update(world, box);
		Skylight fresh;
		fresh.Build(world);
		Check(!changed.Empty() && Same(sky, fresh), "daylight: digging lets it in exactly as a rebuild would");
		Check(sky.LevelAt(x, 1.5f, 12.0f) > 0.0f, "daylight: it reaches into a newly dug tunnel");

		// A chimney from the grid's top: light falls a long way down it.
		Dig(world, 40, 8, 320, 41, cfg::kVoxY - 1, 321, box);
		sky.Update(world, box);
		const float bottom = sky.LevelAt(10.2f, 2.2f, 80.2f);
		std::printf("        (at the foot of a 22 m chimney: %.2f)\n", bottom);
		Check(bottom > 0.5f, "daylight: it falls down a shaft with little loss");

		// What the renderer gets: a wall takes the light of the air in front of it.
		Skylight::CellBox one;
		const int pz = 2, py = 2, px = cfg::kCenterX * cfg::kVoxelsPerMeter / Skylight::kCell;
		one.min[0] = px - 1; one.max[0] = px - 1;
		one.min[1] = one.max[1] = py;
		one.min[2] = one.max[2] = pz;
		Check(!sky.IsOpen(px - 1, py, pz) && sky.IsOpen(px, py, pz) && sky.GpuLevels(one)[0] > 0, "daylight: walls show the light of the air beside them");
	}

	// ---- A generated mountain ---------------------------------------------------------------
	{
		VoxelWorld world;
		const CaveLayout layout = GenerateCaves(world, 1000);
		Skylight sky;
		sky.Build(world);
		bool shaftsLit = true;
		for (const CaveShaft& shaft : layout.shafts)
		{
			const float level = sky.LevelAt(shaft.bottom.x, shaft.bottom.y, shaft.bottom.z);
			std::printf("        (shaft into zone %d: %.2f at its foot)\n", shaft.zone + 1, level);
			shaftsLit &= level > 0.4f;
		}
		Check(!layout.shafts.empty() && shaftsLit, "daylight: every shaft lights its cavern");
		const CaveRoom& deep = layout.zones.back().rooms[0];
		Check(sky.LevelAt(deep.center.x, deep.center.y + 1.0f, deep.center.z) == 0.0f, "daylight: the deepest zone is dark");
	}
	return gLightFailures;
}
