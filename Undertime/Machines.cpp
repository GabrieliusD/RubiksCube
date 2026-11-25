// Machines (Docs/Hauling.md H8-H11). Every one works in its own time: the rate of where it
// stands (OwnRate), so a drill shallower than you runs faster than you while you dive deep,
// and the surface (the mill) runs fastest of all. Fuel burns and wear builds in that time too.
//  - Drill rigs bore a 1 m x 1.5 m tunnel straight ahead into a hopper (ore kept apart), until
//    the hopper is full, the coal runs out, sealing rock stops them, or they wear out (parts).
//    The hand borer works only while you're beside it; steam burns coal; electric needs the
//    power line (Industry).
//  - Belts carry blocks: the first of a line takes from a drill's hopper, each passes to the
//    next, the last drops them. They need a steam engine within reach (or the power line).
//  - The stamp mill (beside the water wheel) crushes stone you tip in and washes out ore; its
//    tailings pile up beside it.
//  - Rails and the hand cart: lay rails in 1 m pieces; the cart runs along them (E pushes it,
//    it rolls downhill on its own), the shovel fills it, Q tips it.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include <Core/Log.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr float kFine = 0.125f;

	int DrillKind(const std::string& item) { return item == "hand_borer" ? 0 : item == "steam_drill" ? 1 : item == "electric_drill" ? 2 : -1; }
	XMFLOAT3 Dir(float yaw) { return { std::sin(yaw), 0.0f, std::cos(yaw) }; }
	int Total(const LooseBlocks::Counts& c) { int n = 0; for (int v : c) n += v; return n; }

	VoxelModel DrillModel(int kind)
	{
		kit::Builder b(8, 9, 12);
		const uint8_t iron = b.Colour(kit::colour::kIron), dark = b.Colour(kit::colour::kIronDark), wood = b.Colour(kit::colour::kWood);
		const uint8_t body = kind == 1 ? b.Colour(kit::colour::kCopper) : kind == 2 ? b.Colour(0x4a6a80) : wood;
		b.Box(0, 0, 0, 8, 2, 9, dark);            // the sled
		b.Box(1, 2, 1, 7, 7, 8, body);            // the works
		b.Box(3, 3, 8, 5, 6, 12, iron);           // the bit
		b.Box(4, 4, 11, 5, 5, 12, b.Colour(0xc8c8d0));
		if (kind == 1) b.Box(2, 7, 2, 4, 9, 4, dark);   // a chimney
		if (kind == 2) b.Box(1, 7, 1, 7, 8, 3, b.Colour(0xffe080, 2.0f));   // a lamp on the coil
		if (kind == 0) b.Box(0, 4, 2, 8, 5, 3, iron);   // the crank
		b.Box(0, 2, 0, 8, 4, 2, b.Colour(kit::colour::kWoodDark));   // the hopper at the back
		return b.model;
	}
}

bool UndertimeModule::IsMachine(const std::string& item) const
{
	return DrillKind(item) >= 0 || item == "belt" || item == "steam_engine" || item == "stamp_mill" || item == "rails" || item == "hand_cart";
}

MeshHandle UndertimeModule::MachineMesh(Engine& engine, const std::string& item)
{
	const int drill = DrillKind(item);
	if (drill >= 0) return kit::Bake(engine, DrillModel(drill), VoxelPivot::BottomCenter, kFine);
	if (item == "belt")
	{
		kit::Builder b(3, 2, 4);
		const uint8_t rubber = b.Colour(0x2a2a2c), roller = b.Colour(kit::colour::kIron), frame = b.Colour(kit::colour::kWoodDark);
		b.Box(0, 0, 0, 3, 1, 4, frame);
		b.Box(0, 1, 0, 3, 2, 4, rubber);
		b.Box(0, 1, 0, 3, 2, 1, roller);
		b.Box(0, 1, 3, 3, 2, 4, roller);
		return kit::Bake(engine, b.model);
	}
	if (item == "steam_engine")
	{
		kit::Builder b(6, 8, 8);
		const uint8_t copper = b.Colour(kit::colour::kCopper), iron = b.Colour(kit::colour::kIron), dark = b.Colour(kit::colour::kIronDark);
		b.Box(0, 0, 0, 6, 1, 8, dark);
		b.Box(1, 1, 1, 5, 5, 6, copper);          // the boiler
		b.Box(2, 5, 2, 3, 8, 3, dark);            // the chimney
		b.Box(5, 1, 5, 6, 6, 8, iron);            // the flywheel
		b.Box(5, 2, 4, 6, 5, 8, iron);
		b.Box(2, 2, 6, 4, 3, 7, b.Colour(0xff8030, 3.0f));   // the firebox
		return kit::Bake(engine, b.model);
	}
	if (item == "stamp_mill")
	{
		kit::Builder b(8, 12, 6);
		const uint8_t wood = b.Colour(kit::colour::kWood), dark = b.Colour(kit::colour::kWoodDark), iron = b.Colour(kit::colour::kIron);
		b.Box(0, 0, 0, 8, 2, 6, dark);            // the mortar box
		for (int x : { 0, 7 }) b.Box(x, 2, 2, x + 1, 12, 4, wood);   // the frame
		b.Box(0, 11, 2, 8, 12, 4, wood);
		for (int x = 1; x < 7; x += 2) b.Box(x, 3, 2, x + 1, 10, 3, iron);   // the stamps
		b.Box(1, 9, 3, 7, 10, 4, dark);           // the cam shaft
		b.Box(0, 2, 4, 8, 4, 6, wood);            // the bin
		return kit::Bake(engine, b.model);
	}
	if (item == "rails")
	{
		kit::Builder b(8, 2, 8);
		const uint8_t iron = b.Colour(kit::colour::kIron), wood = b.Colour(kit::colour::kWoodDark);
		for (int z = 0; z < 8; z += 2) b.Box(0, 0, z, 8, 1, z + 1, wood);
		b.Box(1, 1, 0, 2, 2, 8, iron);
		b.Box(6, 1, 0, 7, 2, 8, iron);
		return kit::Bake(engine, b.model, VoxelPivot::BottomCenter, kFine);
	}
	if (item == "hand_cart")
	{
		kit::Builder b(7, 7, 9);
		const uint8_t iron = b.Colour(kit::colour::kIron), dark = b.Colour(kit::colour::kIronDark), wood = b.Colour(kit::colour::kWood);
		for (int z : { 1, 7 }) { b.Box(0, 0, z, 1, 2, z + 1, dark); b.Box(6, 0, z, 7, 2, z + 1, dark); }   // wheels
		b.Box(0, 2, 0, 7, 3, 9, iron);            // the bed
		b.Box(0, 3, 0, 1, 7, 9, wood); b.Box(6, 3, 0, 7, 7, 9, wood);
		b.Box(0, 3, 0, 7, 7, 1, wood); b.Box(0, 3, 8, 7, 7, 9, wood);
		return kit::Bake(engine, b.model, VoxelPivot::BottomCenter, kFine);
	}
	return {};
}

void UndertimeModule::SpawnMachine(Engine& engine, Placed& p)
{
	// Drills move along their tunnel as they bore.
	const int drill = DrillKind(p.item);
	if (drill >= 0 && engine.GetScene().IsAlive(p.entity))
	{
		const XMFLOAT3 d = Dir(p.yaw);
		engine.GetScene().Get<Transform>(p.entity).position = { p.at.x + d.x * static_cast<float>(p.progress), p.at.y, p.at.z + d.z * static_cast<float>(p.progress) };
	}
}

bool UndertimeModule::PlaceMachine(Engine& engine, const std::string& item, const XMFLOAT3& point, const XMFLOAT3& n, Placed& out, std::string& why)
{
	const bool floor = n.y > 0.7f, wall = std::fabs(n.y) < 0.35f;
	const float s = cfg::kVoxelSize;
	auto snap = [s](float v) { return (std::floor(v / s) + 0.5f) * s; };
	if (DrillKind(item) >= 0)
	{
		if (!InCave(point)) { why = "drills bore into the mountain's rock"; return false; }
		if (!wall) { why = "set it against a rock face"; return false; }
		// Square to the face: it bores straight along the nearest axis into the rock.
		XMFLOAT3 into{ -n.x, 0.0f, -n.z };
		if (std::fabs(into.x) > std::fabs(into.z)) into = { into.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f };
		else into = { 0.0f, 0.0f, into.z > 0 ? 1.0f : -1.0f };
		const XMFLOAT3 front{ point.x - into.x * 0.75f, point.y, point.z - into.z * 0.75f };
		const auto down = engine.GetPhysics().Raycast({ front.x, point.y + 0.3f, front.z }, { 0, -1, 0 }, 4.0f);
		if (!down) { why = "no floor here"; return false; }
		out.at = { snap(front.x), down->point.y, snap(front.z) };
		out.yaw = std::atan2(into.x, into.z);
		return true;
	}
	if (!floor) { why = "it stands on the floor"; return false; }
	out.at = { snap(point.x), point.y, snap(point.z) };
	out.yaw = std::round(player.yaw / XM_PIDIV2) * XM_PIDIV2;
	if (item == "stamp_mill")
	{
		const float dx = point.x - cfg::kMillWheel[0], dz = point.z - cfg::kMillWheel[1];
		if (dx * dx + dz * dz > cfg::kMillReach * cfg::kMillReach) { why = "it needs the water wheel (by the clockmaker's)"; return false; }
	}
	if (item == "rails")
	{
		// Line up with a rail it continues, so lines join without gaps.
		for (const Placed& q : placed)
		{
			if (q.item != "rails") continue;
			const XMFLOAT3 d = Dir(q.yaw);
			for (float sign : { 1.0f, -1.0f })
			{
				const XMFLOAT3 next{ q.at.x + d.x * sign, q.at.y, q.at.z + d.z * sign };
				if (std::fabs(next.x - point.x) < 0.55f && std::fabs(next.z - point.z) < 0.55f && std::fabs(point.y - q.at.y) < 0.8f)
				{
					out.at = { next.x, point.y, next.z };
					out.yaw = q.yaw;
				}
			}
		}
		for (const Placed& q : placed)
			if (q.item == "rails" && std::fabs(q.at.x - out.at.x) < 0.3f && std::fabs(q.at.z - out.at.z) < 0.3f && std::fabs(q.at.y - out.at.y) < 0.5f) { why = "there's a rail here"; return false; }
	}
	if (item == "hand_cart")
	{
		int rail = -1;
		for (size_t i = 0; i < placed.size(); ++i)
			if (placed[i].item == "rails" && std::fabs(placed[i].at.x - point.x) < 0.6f && std::fabs(placed[i].at.z - point.z) < 0.6f) rail = static_cast<int>(i);
		if (rail < 0) { why = "set it on a rail"; return false; }
		out.at = { placed[rail].at.x, placed[rail].at.y + 0.08f, placed[rail].at.z };
		out.yaw = placed[rail].yaw;
	}
	return true;
}

bool UndertimeModule::EngineNear(const XMFLOAT3& at) const
{
	if (era >= cfg::kElectricFromEra) return true;   // the power line
	for (const Placed& p : placed)
	{
		if (p.item != "steam_engine" || p.fuel <= 0.0f || !p.on || p.wear >= cfg::kWearHours * 3600.0) continue;
		const float dx = p.at.x - at.x, dy = p.at.y - at.y, dz = p.at.z - at.z;
		if (dx * dx + dy * dy + dz * dz < cfg::kEngineReach * cfg::kEngineReach) return true;
	}
	return false;
}

void UndertimeModule::UpdateDrill(Engine& engine, Placed& p, double hours)
{
	const int kind = DrillKind(p.item);
	if (!p.on || p.wear >= cfg::kWearHours * 3600.0 || hours <= 0.0) return;
	if (kind == 0)
	{
		// Cranked: only while you're beside it.
		const XMFLOAT3 d = Dir(p.yaw);
		const float dx = p.at.x + d.x * static_cast<float>(p.progress) - player.position.x, dz = p.at.z + d.z * static_cast<float>(p.progress) - player.position.z;
		if (dx * dx + dz * dz > cfg::kHandBorerReach * cfg::kHandBorerReach) return;
	}
	if (kind == 1) hours = std::min(hours, static_cast<double>(p.fuel / cfg::kCoalPerHour));
	if (kind == 2 && era < cfg::kElectricFromEra) return;
	if (hours <= 0.0) return;
	const double metres = cfg::kDrillMetres[kind] * hours;
	const float s = cfg::kVoxelSize;
	const XMFLOAT3 d = Dir(p.yaw);
	const XMFLOAT3 side{ d.z, 0.0f, -d.x };
	const int floorVoxel = static_cast<int>(std::floor((p.at.y + 0.05f) / s));
	const int doneSlices = static_cast<int>(std::floor(p.progress / s));
	int targetSlices = static_cast<int>(std::floor((p.progress + metres) / s));
	VoxelWorld::Box changed;
	bool any = false, blocked = false;
	int slice = doneSlices;
	for (; slice < targetSlices && !blocked; ++slice)
	{
		if (Total(p.hopper) >= cfg::kHopperCapacity) { blocked = true; break; }
		const float along = 0.75f + (slice + 0.5f) * s;
		// First, whether anything in this slice stops it.
		std::vector<std::array<int, 3>> cells;
		for (int w = 0; w < cfg::kDrillWide; ++w)
			for (int h = 0; h < cfg::kDrillTall; ++h)
			{
				const float off = (w - cfg::kDrillWide * 0.5f + 0.5f) * s;
				const XMFLOAT3 c{ p.at.x + d.x * along + side.x * off, (floorVoxel + h + 0.5f) * s, p.at.z + d.z * along + side.z * off };
				const int x = static_cast<int>(std::floor(c.x / s)), y = static_cast<int>(std::floor(c.y / s)), z = static_cast<int>(std::floor(c.z / s));
				if (!VoxelWorld::InGrid(x, y, z)) { blocked = true; continue; }
				const Block b = world.Get(x, y, z);
				if (b == Block::Air) continue;
				if (cfg::Unbreakable(b) && !(b == Block::Bedrock && kind == 2 && SealBand(x, y, z) > 0)) { blocked = true; continue; }
				cells.push_back({ x, y, z });
			}
		if (blocked) break;
		for (const auto& c : cells)
		{
			Block b = world.Get(c[0], c[1], c[2]);
			if (b == Block::Bedrock) b = Block::Stone;
			++p.hopper[static_cast<int>(b)];
			world.Set(c[0], c[1], c[2], Block::Air);
			if (!any) { changed = { { c[0], c[1], c[2] }, { c[0], c[1], c[2] } }; any = true; }
			for (int k = 0; k < 3; ++k) { changed.min[k] = std::min(changed.min[k], c[k]); changed.max[k] = std::max(changed.max[k], c[k]); }
		}
	}
	const double before = p.progress;
	p.progress = blocked ? std::max(p.progress, static_cast<double>(slice * s)) : p.progress + metres;
	const double worked = (p.progress - before) / cfg::kDrillMetres[kind];
	if (kind == 1) p.fuel = std::max(0.0f, p.fuel - static_cast<float>(worked * cfg::kCoalPerHour));
	p.wear += worked * 3600.0;
	if (any) ApplyWorldChange(engine, changed, true);
	if (blocked) p.on = false;   // stopped by rock or a full hopper: E (emptying it) starts it again
	SpawnMachine(engine, p);
}

void UndertimeModule::UpdateMill(Engine& engine, Placed& p, double hours)
{
	if (p.wear >= cfg::kWearHours * 3600.0 || hours <= 0.0) return;
	int& bin = p.hopper[static_cast<int>(Block::Stone)];
	if (bin <= 0) return;
	const double before = p.progress;
	const double crushed = std::min(static_cast<double>(bin), cfg::kMillStonePerHour * hours + p.fuel);
	const int whole = static_cast<int>(crushed);
	p.fuel = static_cast<float>(crushed - whole);   // the part of a stone under the stamps
	if (whole <= 0) return;
	bin -= whole;
	p.progress += whole;
	// Ore washed out, a set share of what's crushed (deterministic: whole counts as the total grows).
	p.hopper[static_cast<int>(Block::Copper)] += static_cast<int>(std::floor(p.progress * cfg::kMillCopper)) - static_cast<int>(std::floor(before * cfg::kMillCopper));
	if (era >= 2) p.hopper[static_cast<int>(Block::Silver)] += static_cast<int>(std::floor(p.progress * cfg::kMillSilver)) - static_cast<int>(std::floor(before * cfg::kMillSilver));
	p.wear += whole / cfg::kMillStonePerHour * 3600.0;
	// The tailings heap grows beside it.
	Scene& scene = engine.GetScene();
	const float size = std::min(4.0f, 0.3f + std::sqrt(static_cast<float>(p.progress)) * 0.04f);
	if (!scene.IsAlive(p.extra))
	{
		kit::Builder b(4, 2, 4);
		b.Speckle(0, 0, 0, 4, 1, 4, { b.Colour(0x9a9288), b.Colour(0x8a8278), b.Colour(0xa8a094) });
		b.Speckle(1, 1, 1, 3, 2, 3, { b.Colour(0x9a9288), b.Colour(0x8a8278) });
		const XMFLOAT3 d = Dir(p.yaw);
		p.extra = kit::Spawn(engine, PlacedMeshFor(engine, "tailings", b.model), engine.GetAssets().GetMaterial("Materials/Town.mat"), { p.at.x - d.z * 2.2f, p.at.y, p.at.z + d.x * 2.2f });
	}
	scene.Get<Transform>(p.extra).scale = { size, size * 0.6f, size };
}

void UndertimeModule::UpdateBelts(Engine& engine, float dt)
{
	// Each belt passes its load on to the belt in front, or drops it at the end of the line.
	beltWake -= dt;
	const bool wake = beltWake <= 0.0f;
	if (wake) beltWake = 1.0f;
	for (size_t i = 0; i < placed.size(); ++i)
	{
		Placed& p = placed[i];
		if (p.item != "belt" || !p.on) continue;
		if (!EngineNear(p.at)) continue;
		const XMFLOAT3 d = Dir(p.yaw);
		const double ownSeconds = dt * OwnRate(p.at);
		// The first of a line takes from a drill or the mill just behind it.
		bool fed = false;
		for (const Placed& q : placed) fed = fed || (q.item == "belt" && &q != &p && std::fabs(q.at.x + std::sin(q.yaw) - p.at.x) < 0.5f && std::fabs(q.at.z + std::cos(q.yaw) - p.at.z) < 0.5f);
		if (!fed)
			for (Placed& q : placed)
			{
				if (DrillKind(q.item) < 0 && q.item != "stamp_mill") continue;
				const XMFLOAT3 qd = Dir(q.yaw);
				const float qx = q.at.x + qd.x * static_cast<float>(DrillKind(q.item) >= 0 ? q.progress : 0.0), qz = q.at.z + qd.z * static_cast<float>(DrillKind(q.item) >= 0 ? q.progress : 0.0);
				const float bx = p.at.x - d.x * 0.5f, bz = p.at.z - d.z * 0.5f;
				if ((qx - bx) * (qx - bx) + (qz - bz) * (qz - bz) > 1.6f * 1.6f) continue;
				int want = std::min(cfg::kBeltHolds - Total(p.hopper), static_cast<int>(ownSeconds * cfg::kBeltRate + 0.999));
				for (int t = 1; t < static_cast<int>(Block::Count) && want > 0; ++t)
				{
					if (q.item == "stamp_mill" && static_cast<Block>(t) == Block::Stone) continue;   // the mill's bin isn't its output
					const int take = std::min(want, q.hopper[t]);
					q.hopper[t] -= take;
					p.hopper[t] += take;
					want -= take;
				}
			}
		// Onwards: to the next belt, or off the end.
		int next = -1;
		for (size_t j = 0; j < placed.size(); ++j)
			if (j != i && placed[j].item == "belt" && std::fabs(placed[j].at.x - (p.at.x + d.x)) < 0.5f && std::fabs(placed[j].at.z - (p.at.z + d.z)) < 0.5f && std::fabs(placed[j].at.y - p.at.y) < 0.8f)
				next = static_cast<int>(j);
		int move = std::min(Total(p.hopper), static_cast<int>(ownSeconds * cfg::kBeltRate + 0.999));
		if (next < 0) move = std::min(move, cfg::kBeltSpawnPerFrame);
		else move = std::min(move, cfg::kBeltHolds - Total(placed[next].hopper));
		for (int t = 1; t < static_cast<int>(Block::Count) && move > 0; ++t)
		{
			const int n = std::min(move, p.hopper[t]);
			if (n <= 0) continue;
			p.hopper[t] -= n;
			move -= n;
			if (next >= 0) placed[next].hopper[t] += n;
			else
				for (int k = 0; k < n; ++k)
				{
					const XMFLOAT3 c{ p.at.x + d.x * 0.65f, p.at.y + 0.45f + k * 0.26f, p.at.z + d.z * 0.65f };
					blocks.SpawnFree(engine, static_cast<Block>(t), cfg::kVoxX / 2, 4, 2, c, { d.x * 1.0f, 0.0f, d.z * 1.0f });
				}
		}
		// Loose blocks dropped on a belt ride along it (near you).
		const float dx = p.at.x - player.position.x, dz = p.at.z - player.position.z;
		if (dx * dx + dz * dz < 40.0f * 40.0f)
		{
			const BoundingBox area({ p.at.x, p.at.y + 0.35f, p.at.z }, { 0.5f, 0.3f, 0.5f });
			blocks.Push(engine, area, { d.x * 1.2f, 0.0f, d.z * 1.2f });
			if (wake)
			{
				const float s = cfg::kVoxelSize;
				VoxelWorld::Box box{ { static_cast<int>((p.at.x - 0.5f) / s), static_cast<int>((p.at.y) / s), static_cast<int>((p.at.z - 0.5f) / s) },
					{ static_cast<int>((p.at.x + 0.5f) / s), static_cast<int>((p.at.y + 0.5f) / s), static_cast<int>((p.at.z + 0.5f) / s) } };
				blocks.Wake(engine, box);
			}
		}
	}
}

void UndertimeModule::UpdateCart(Engine& engine, Placed& p, float dt)
{
	// The rail it's on, and where it goes from there.
	int rail = -1;
	float best = 0.8f;
	for (size_t i = 0; i < placed.size(); ++i)
	{
		const Placed& r = placed[i];
		if (r.item != "rails") continue;
		const XMFLOAT3 d = Dir(r.yaw);
		const float along = (p.at.x - r.at.x) * d.x + (p.at.z - r.at.z) * d.z;
		const float off = std::fabs((p.at.x - r.at.x) * d.z - (p.at.z - r.at.z) * d.x);
		if (std::fabs(along) <= 0.52f && off < best && std::fabs(p.at.y - r.at.y) < 1.0f) { best = off; rail = static_cast<int>(i); }
	}
	if (rail < 0) return;
	const Placed& r = placed[rail];
	const XMFLOAT3 d = Dir(r.yaw);
	float speed = p.fuel;   // the cart's speed along the rail (signed, +d), kept in `fuel`
	if (heldCart >= 0 && &placed[heldCart] == &p)
	{
		// Pushed: follows you along the line, a metre and a bit ahead.
		const float playerAlong = (player.position.x - p.at.x) * d.x + (player.position.z - p.at.z) * d.z;
		const float side = playerAlong < 0.0f ? -1.0f : 1.0f;
		const float want = playerAlong - side * 1.3f;
		speed = std::clamp(want * 4.0f, -cfg::kCartPushSpeed, cfg::kCartPushSpeed);
		const float away = std::sqrt((player.position.x - p.at.x) * (player.position.x - p.at.x) + (player.position.z - p.at.z) * (player.position.z - p.at.z));
		if (away > 3.5f) heldCart = -1;
	}
	else
	{
		// Rolls downhill: the slope to the next rail along its way.
		int ahead = -1;
		const float sign = speed >= 0.0f ? 1.0f : -1.0f;
		for (size_t j = 0; j < placed.size(); ++j)
			if (placed[j].item == "rails" && std::fabs(placed[j].at.x - (r.at.x + d.x * sign)) < 0.5f && std::fabs(placed[j].at.z - (r.at.z + d.z * sign)) < 0.5f)
				ahead = static_cast<int>(j);
		int behind = -1;
		for (size_t j = 0; j < placed.size(); ++j)
			if (placed[j].item == "rails" && std::fabs(placed[j].at.x - (r.at.x - d.x * sign)) < 0.5f && std::fabs(placed[j].at.z - (r.at.z - d.z * sign)) < 0.5f)
				behind = static_cast<int>(j);
		const float slope = ahead >= 0 ? (placed[ahead].at.y - r.at.y) * sign : behind >= 0 ? (r.at.y - placed[behind].at.y) * sign : 0.0f;
		speed += -slope * sign * cfg::kCartRollAccel * dt * (speed == 0.0f ? 1.0f : 1.0f);
		if (speed == 0.0f && slope != 0.0f) speed = slope > 0.0f ? -0.01f : 0.01f;
		speed *= std::pow(0.6f, dt);   // friction
		if (std::fabs(speed) < 0.02f) speed = 0.0f;
	}
	// Moves, but not off the end of the line.
	XMFLOAT3 next{ p.at.x + d.x * speed * dt, p.at.y, p.at.z + d.z * speed * dt };
	const float along = (next.x - r.at.x) * d.x + (next.z - r.at.z) * d.z;
	if (std::fabs(along) > 0.5f)
	{
		bool continues = false;
		const float sign = along > 0.0f ? 1.0f : -1.0f;
		for (const Placed& q : placed)
			if (q.item == "rails" && std::fabs(q.at.x - (r.at.x + d.x * sign)) < 0.5f && std::fabs(q.at.z - (r.at.z + d.z * sign)) < 0.5f) continues = true;
		if (!continues)
		{
			next = { r.at.x + d.x * 0.5f * sign, p.at.y, r.at.z + d.z * 0.5f * sign };
			speed = 0.0f;
		}
	}
	// Height: between this rail and the next, so it rides slopes smoothly.
	next.y = r.at.y + 0.08f;
	for (const Placed& q : placed)
	{
		if (q.item != "rails" || &q == &r) continue;
		const float t = (next.x - r.at.x) * d.x + (next.z - r.at.z) * d.z;
		if (std::fabs(q.at.x - (r.at.x + d.x * (t > 0 ? 1.0f : -1.0f))) < 0.5f && std::fabs(q.at.z - (r.at.z + d.z * (t > 0 ? 1.0f : -1.0f))) < 0.5f)
			next.y = r.at.y + (q.at.y - r.at.y) * std::fabs(t) + 0.08f;
	}
	p.at = next;
	p.fuel = speed;
	p.yaw = r.yaw;
	Scene& scene = engine.GetScene();
	if (scene.IsAlive(p.entity))
	{
		Transform& t = scene.Get<Transform>(p.entity);
		t.position = p.at;
		XMStoreFloat4(&t.rotation, XMQuaternionRotationRollPitchYaw(0.0f, p.yaw, 0.0f));
	}
}

void UndertimeModule::UpdateMachines(Engine& engine, float dt)
{
	if (state == State::Title) return;
	for (Placed& p : placed)
	{
		const double ownSeconds = dt * OwnRate(p.at);
		const double hours = ownSeconds / 3600.0;
		if (DrillKind(p.item) >= 0) UpdateDrill(engine, p, hours);
		else if (p.item == "stamp_mill") UpdateMill(engine, p, hours);
		else if (p.item == "steam_engine" && p.on && p.fuel > 0.0f)
		{
			p.fuel = std::max(0.0f, p.fuel - static_cast<float>(hours * cfg::kCoalPerHour));
			p.wear += ownSeconds;
		}
		else if (p.item == "hand_cart") UpdateCart(engine, p, dt);
	}
	UpdateBelts(engine, dt);
	// Q tips the cart you're pushing; R puts your pack's stone in one beside you.
	if (state == State::Playing && heldCart >= 0 && heldCart < static_cast<int>(placed.size()) && engine.GetInput().WasKeyPressed('Q'))
	{
		Placed& c = placed[heldCart];
		const XMFLOAT3 side{ std::cos(c.yaw), 0.0f, -std::sin(c.yaw) };
		int out = 0;
		for (int t = 1; t < static_cast<int>(Block::Count); ++t)
			for (; c.hopper[t] > 0 && out < 400; --c.hopper[t], ++out)
			{
				const XMFLOAT3 at{ c.at.x + side.x * 0.8f + ((out % 5) - 2) * 0.1f, c.at.y + 0.6f + (out / 25) * 0.05f, c.at.z + side.z * 0.8f + ((out / 5 % 5) - 2) * 0.1f };
				blocks.SpawnFree(engine, static_cast<Block>(t), cfg::kVoxX / 2, 4, 2, at, { side.x * 1.4f, 0.3f, side.z * 1.4f });
			}
		messages.Show("Tipped out " + std::to_string(out) + " blocks.", 1.5f);
	}
}

bool UndertimeModule::MachineInteract(Engine& engine, Placed& p)
{
	if (p.wear >= cfg::kWearHours * 3600.0 && p.item != "rails" && p.item != "hand_cart" && p.item != "belt")
	{
		if (purse.Count("parts") <= 0) { messages.Show("Broken. It needs machine parts (the smith).", 2.0f); return true; }
		purse.Add("parts", -1);
		p.wear = 0.0;
		p.on = true;
		messages.Show("Mended.", 1.5f);
		return true;
	}
	if (p.item == "hand_cart")
	{
		const int index = static_cast<int>(&p - placed.data());
		if (heldCart == index) heldCart = -1;
		else
		{
			heldCart = index;
			// The pack's stone goes in when you take hold (and R on the barrow is the same idea).
		}
		return true;
	}
	if (p.item == "steam_engine" || p.item == "steam_drill")
	{
		const int coal = std::min(purse.Count("coal"), static_cast<int>(cfg::kFuelTank - p.fuel));
		if (coal > 0)
		{
			purse.Add("coal", -coal);
			p.fuel += coal;
			p.on = true;
			messages.Show("+" + std::to_string(coal) + " coal.", 1.2f);
			if (p.item == "steam_engine") return true;
		}
		else if (p.item == "steam_engine") { messages.Show(p.fuel > 0.0f ? "It has coal." : "It needs coal (the store).", 1.5f); return true; }
	}
	if (p.item == "stamp_mill")
	{
		// Stone in (from your pack and a barrow beside you), ore out.
		int in = backpack[static_cast<int>(Block::Stone)];
		backpack[static_cast<int>(Block::Stone)] = 0;
		if (barrow.owned && !barrow.held)
		{
			const float dx = barrow.position.x - p.at.x, dz = barrow.position.z - p.at.z;
			if (dx * dx + dz * dz < 25.0f) { in += barrow.load[static_cast<int>(Block::Stone)]; barrow.load[static_cast<int>(Block::Stone)] = 0; }
		}
		int& bin = p.hopper[static_cast<int>(Block::Stone)];
		const int room = cfg::kMillBin - bin;
		const int accepted = std::min(in, room);
		bin += accepted;
		backpack[static_cast<int>(Block::Stone)] += in - accepted;
		int ore = 0;
		for (Block b : { Block::Copper, Block::Silver }) { backpack[static_cast<int>(b)] += p.hopper[static_cast<int>(b)]; ore += p.hopper[static_cast<int>(b)]; p.hopper[static_cast<int>(b)] = 0; }
		messages.Show(std::to_string(accepted) + " stone into the bin" + (ore > 0 ? ", " + std::to_string(ore) + " ore out." : "."), 2.0f);
		return true;
	}
	if (DrillKind(p.item) >= 0 || p.item == "belt")
	{
		// The hopper into your pack.
		const int room = PackCapacity() - BackpackCount();
		int took = 0;
		for (int t = 1; t < static_cast<int>(Block::Count) && took < room; ++t)
		{
			const int n = std::min(room - took, p.hopper[t]);
			backpack[t] += n;
			p.hopper[t] -= n;
			took += n;
		}
		if (DrillKind(p.item) >= 0 && !p.on) p.on = true;   // stopped by rock or a full hopper: try again
		messages.Show(took > 0 ? std::to_string(took) + " blocks into your pack." : room <= 0 ? "Your pack is full." : "Nothing in it.", 1.5f);
		return true;
	}
	return false;
}

std::string UndertimeModule::MachinePrompt(const Placed& p) const
{
	char text[200];
	const bool broken = p.wear >= cfg::kWearHours * 3600.0;
	const int load = Total(p.hopper);
	if (DrillKind(p.item) >= 0)
	{
		const int kind = DrillKind(p.item);
		const char* state = broken ? "broken (E: parts)" : !p.on ? "stopped (rock, or the hopper's full)" : kind == 0 ? "works while you're beside it" :
			kind == 1 ? (p.fuel <= 0.0f ? "out of coal" : "working") : (era < cfg::kElectricFromEra ? "no power line yet" : "working");
		std::snprintf(text, sizeof(text), "%s: %s, %.1f m bored, hopper %d / %d%s   E  %s", econ::NameOf(p.item).c_str(), state, p.progress, load, cfg::kHopperCapacity,
			kind == 1 ? ("  coal " + std::to_string(static_cast<int>(p.fuel))).c_str() : "", kind == 1 && purse.Count("coal") > 0 && p.fuel < cfg::kFuelTank - 1 ? "Add coal" : "Empty the hopper");
		return text;
	}
	if (p.item == "belt")
	{
		std::snprintf(text, sizeof(text), "Belt: %d on it, %s", load, EngineNear(p.at) ? "running" : "no engine in reach (a steam engine with coal)");
		return text;
	}
	if (p.item == "steam_engine")
	{
		std::snprintf(text, sizeof(text), "Steam engine: %s, coal %d   E  Add coal", broken ? "broken (E: parts)" : p.fuel > 0.0f ? "driving belts within 30 m" : "cold", static_cast<int>(p.fuel));
		return text;
	}
	if (p.item == "stamp_mill")
	{
		std::snprintf(text, sizeof(text), "Stamp mill: %s, bin %d stone, %d crushed, ore waiting %d   E  Tip in stone, take the ore",
			broken ? "broken (E: parts)" : p.hopper[static_cast<int>(Block::Stone)] > 0 ? "stamping" : "idle", p.hopper[static_cast<int>(Block::Stone)], static_cast<int>(p.progress),
			p.hopper[static_cast<int>(Block::Copper)] + p.hopper[static_cast<int>(Block::Silver)]);
		return text;
	}
	if (p.item == "hand_cart")
	{
		const bool held = heldCart >= 0 && heldCart < static_cast<int>(placed.size()) && &placed[heldCart] == &p;
		std::snprintf(text, sizeof(text), "Hand cart: %d / %d   %s", load, cfg::kCartCapacity, held ? "E  Let go      Q  Tip it out" : "E  Push it (it rolls downhill on its own)");
		return text;
	}
	if (p.item == "rails") return "Rails";
	return {};
}

bool UndertimeModule::CartTakesScoops(XMFLOAT3& into, int& cart) const
{
	cart = -1;
	float best = (cfg::kBarrowReach + 1.0f) * (cfg::kBarrowReach + 1.0f);
	for (size_t i = 0; i < placed.size(); ++i)
	{
		const Placed& p = placed[i];
		if (p.item != "hand_cart" || static_cast<int>(i) == heldCart || Total(p.hopper) >= cfg::kCartCapacity) continue;
		const float dx = p.at.x - player.position.x, dz = p.at.z - player.position.z;
		if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; cart = static_cast<int>(i); into = { p.at.x, p.at.y + 0.6f, p.at.z }; }
	}
	return cart >= 0;
}
