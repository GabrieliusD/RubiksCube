#include "CaveMap.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;
using cfg::Block;

namespace
{
	const ImU32 kZoneColors[] =
	{
		IM_COL32(110, 170, 230, 255),
		IM_COL32(120, 210, 140, 255),
		IM_COL32(235, 140, 90, 255),
		IM_COL32(220, 130, 200, 255),
		IM_COL32(150, 140, 240, 255),
	};
}

void CaveMap::Rebuild(const VoxelWorld& world, const CaveLayout& layout)
{
	const int V = cfg::kVoxelsPerMeter;
	mCells.assign(static_cast<size_t>(cfg::kGridX) * cfg::kGridZ, Cell::Rock);
	mZone.assign(mCells.size(), 255);

	for (int mz = 0; mz < cfg::kGridZ; ++mz)
	{
		// The first zone that hasn't ended yet (the entrance passage counts as zone 1; a
		// column straddling a zone edge takes the zone it reaches into).
		uint8_t zone = 255;
		for (size_t i = 0; i < layout.zones.size(); ++i)
		{
			if (mz < layout.zones[i].z1) { zone = static_cast<uint8_t>(i); break; }
		}

		for (int mx = 0; mx < cfg::kGridX; ++mx)
		{
			bool air = false, ore = false, relic = false;
			for (int z = mz * V; z < (mz + 1) * V; ++z)
				for (int x = mx * V; x < (mx + 1) * V; ++x)
					for (int y = 0; y < cfg::kVoxY; ++y)
					{
						const Block b = world.Get(x, y, z);
						if (b == Block::Air) air = true;
						else if (b == Block::StrangeWatch) relic = true;
						else if (b != Block::Stone && !cfg::Unbreakable(b)) ore = true;
					}

			Cell cell = Cell::Rock;
			if (relic) cell = Cell::Relic;
			else if (air)
			{
				cell = Cell::Cave;
				for (const CavePocket& p : layout.pockets)
				{
					const float dx = mx + 0.5f - p.shape.center.x, dz = mz + 0.5f - p.shape.center.z;
					if (dx * dx + dz * dz < (p.shape.radius + 0.6f) * (p.shape.radius + 0.6f)) cell = Cell::Pocket;
				}
			}
			else if (ore) cell = Cell::Ore;
			mCells[mx + static_cast<size_t>(cfg::kGridX) * mz] = cell;
			mZone[mx + static_cast<size_t>(cfg::kGridX) * mz] = zone;
		}
	}
}

void CaveMap::Draw(const hud::Frame& f, const CaveLayout& layout, const XMFLOAT3& player, float playerYaw) const
{
	if (mCells.empty()) return;

	// Depth (z) left to right, width (x) top to bottom.
	const float cell = std::floor(std::min(f.size.x * 0.92f / cfg::kGridZ, f.size.y * 0.55f / cfg::kGridX) * 4.0f) / 4.0f;
	const float width = cell * cfg::kGridZ, height = cell * cfg::kGridX;
	const ImVec2 origin{ f.origin.x + (f.size.x - width) * 0.5f, f.origin.y + (f.size.y - height) * 0.5f };
	auto toScreen = [&](float x, float z) { return ImVec2(origin.x + z * cell, origin.y + x * cell); };

	f.draw->AddRectFilled({ origin.x - 12, origin.y - 28 }, { origin.x + width + 12, origin.y + height + 32 }, IM_COL32(10, 9, 14, 225), 8.0f);
	f.draw->AddRectFilled(origin, { origin.x + width, origin.y + height }, IM_COL32(42, 38, 44, 255));

	// Seal bands.
	for (const auto& seal : layout.seals)
	{
		f.draw->AddRectFilled(toScreen(0, seal.first), toScreen(static_cast<float>(cfg::kGridX), seal.second), IM_COL32(90, 45, 40, 255));
	}

	for (int mz = 0; mz < cfg::kGridZ; ++mz)
	{
		for (int mx = 0; mx < cfg::kGridX; ++mx)
		{
			const size_t i = mx + static_cast<size_t>(cfg::kGridX) * mz;
			ImU32 color = 0;
			switch (mCells[i])
			{
			case Cell::Rock: continue;
			case Cell::Cave: color = mZone[i] < 5 ? kZoneColors[mZone[i]] : IM_COL32(200, 200, 200, 255); break;
			case Cell::Pocket: color = IM_COL32(255, 220, 90, 255); break;
			case Cell::Ore: color = IM_COL32(120, 95, 70, 255); break;
			case Cell::Relic: color = IM_COL32(255, 95, 210, 255); break;
			}
			const ImVec2 a = toScreen(static_cast<float>(mx), static_cast<float>(mz));
			f.draw->AddRectFilled(a, { a.x + cell, a.y + cell }, color);
		}
	}

	// Pocket outlines and zone labels.
	for (const CavePocket& p : layout.pockets)
	{
		f.draw->AddCircle(toScreen(p.shape.center.x, p.shape.center.z), (p.shape.radius + 1.5f) * cell, IM_COL32(255, 220, 90, 200), 20, 1.5f);
	}
	for (size_t i = 0; i < layout.zones.size(); ++i)
	{
		char label[16];
		std::snprintf(label, sizeof(label), "Zone %zu", i + 1);
		f.draw->AddText(toScreen(-2.4f, layout.zones[i].z0 + 0.5f), IM_COL32(220, 210, 200, 255), label);
	}

	// Player: dot plus facing line (only while inside the mapped area).
	if (player.z > -2.0f)
	{
		const ImVec2 p = toScreen(player.x, std::max(player.z, 0.0f));
		const ImVec2 tip = toScreen(player.x + std::sin(playerYaw) * 3.0f, std::max(player.z, 0.0f) + std::cos(playerYaw) * 3.0f);
		f.draw->AddLine(p, tip, IM_COL32(255, 255, 255, 255), 2.0f);
		f.draw->AddCircleFilled(p, std::max(cell * 0.8f, 3.0f), IM_COL32(255, 255, 255, 255));
	}

	// Legend: one swatch per kind of cell.
	float x = origin.x;
	const float y = origin.y + height + 8;
	const float swatch = f.fontSize * 0.8f;
	char seedText[64];
	std::snprintf(seedText, sizeof(seedText), "Mountain seed %llu   (top-down, entrance on the left)", static_cast<unsigned long long>(layout.seed));
	f.draw->AddText({ x, y }, IM_COL32(255, 210, 150, 255), seedText);
	x += ImGui::CalcTextSize(seedText).x + swatch * 2.0f;
	const struct { ImU32 color; const char* label; } kLegend[] =
	{
		{ kZoneColors[0], "caves (by zone)" },
		{ IM_COL32(255, 220, 90, 255), "sealed pockets" },
		{ IM_COL32(120, 95, 70, 255), "ore" },
		{ IM_COL32(255, 95, 210, 255), "relics" },
		{ IM_COL32(90, 45, 40, 255), "seal bands" },
	};
	for (const auto& item : kLegend)
	{
		f.draw->AddRectFilled({ x, y + 2 }, { x + swatch, y + 2 + swatch }, item.color);
		x += swatch * 1.4f;
		f.draw->AddText({ x, y }, IM_COL32(220, 210, 200, 255), item.label);
		x += ImGui::CalcTextSize(item.label).x + swatch * 1.6f;
	}
	f.draw->AddText({ x, y }, IM_COL32(150, 140, 130, 255), "M closes");
}
