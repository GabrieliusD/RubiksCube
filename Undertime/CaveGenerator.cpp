#include "CaveGenerator.h"
#include "Noise.h"
#include <algorithm>
#include <cmath>
#include <random>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr int V = cfg::kVoxelsPerMeter;
	constexpr float S = cfg::kVoxelSize;
	constexpr float kWidth = static_cast<float>(cfg::kGridX);
	constexpr float kHeight = static_cast<float>(cfg::kGridY);
	constexpr float kShell = 1.0f;   // bedrock thickness (m)
	constexpr float kRoomFloorShare = 0.85f;   // fraction of a room's radius with a flat floor
	constexpr float kSeparateLevels = 3.0f;    // overlapping rooms closer than this in height share a floor

	struct Generator
	{
		VoxelWorld& world;
		std::vector<Block>& voxels;
		std::mt19937_64 rng;
		Perlin3 perlin;
		NoiseLattice wallNoise;
		NoiseLattice cheeseNoise;
		std::vector<uint8_t> visited;
		std::vector<uint8_t> floorGuard;   // rock under room/tunnel floors that noise caverns must not remove
		std::vector<int> stack;

		Generator(VoxelWorld& w, uint64_t seed)
			: world(w), voxels(w.Voxels()), rng(seed), perlin(seed * 0x9E3779B97F4A7C15ull + 1)
		{
		}

		float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
		int RandInt(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

		// Ellipsoid cavity standing on floorY (nothing below the floor is carved), with
		// noisy walls, limited to the depth range [zMin, zMax).
		void CarveEllipsoid(const XMFLOAT3& c, float rx, float ry, float floorY, float zMin, float zMax)
		{
			const float pad = cfg::kWallNoiseAmount;
			const int x0 = std::max(static_cast<int>(std::floor((c.x - rx - pad) * V)), 0);
			const int x1 = std::min(static_cast<int>(std::floor((c.x + rx + pad) * V)), cfg::kVoxX - 1);
			const int y0 = std::max(static_cast<int>(std::floor(floorY * V)), 0);
			const int y1 = std::min(static_cast<int>(std::floor((c.y + ry + pad) * V)), cfg::kVoxY - 1);
			const int z0 = std::max(static_cast<int>(std::floor((c.z - rx - pad) * V)), static_cast<int>(std::ceil(zMin * V)));
			const int z1 = std::min(static_cast<int>(std::floor((c.z + rx + pad) * V)), static_cast<int>(std::ceil(zMax * V)) - 1);
			const float minRadius = std::min(rx, ry);
			for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
			for (int x = x0; x <= x1; ++x)
			{
				Block& b = voxels[VoxelWorld::Index(x, y, z)];
				if (b != Block::Stone) continue;
				const float px = (x + 0.5f) * S, py = (y + 0.5f) * S, pz = (z + 0.5f) * S;
				const float dx = (px - c.x) / rx, dy = (py - c.y) / ry, dz = (pz - c.z) / rx;
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				const float threshold = 1.0f + wallNoise.Sample(px, py, pz) * cfg::kWallNoiseAmount / minRadius;
				if (d < threshold)
				{
					b = Block::Air;
					// Keep a solid floor under walkable spaces, whatever the noise caverns do.
					if (y == y0 && !floorGuard.empty())
					{
						for (int k = 1; k <= 3 && y - k >= 0; ++k) floorGuard[VoxelWorld::Index(x, y - k, z)] = 1;
					}
				}
			}
		}

		void CarveRoom(const CaveRoom& room, float zMin, float zMax)
		{
			// Centre a little above the floor: a flat floor about 0.9 x radius across, ~1.4 x height tall.
			const XMFLOAT3 c{ room.center.x, room.center.y + room.height * 0.4f, room.center.z };
			CarveEllipsoid(c, room.radius, room.height, room.center.y, zMin, zMax);
		}

		// A Perlin worm from room a to room b: heads for b with noisy wandering that fades
		// out near the end, floor height interpolated so the slope stays walkable.
		void CarveTunnel(const CaveRoom& a, const CaveRoom& b, int tunnelId, const CaveZone& zone)
		{
			const float step = 0.5f;
			float x = a.center.x, z = a.center.z;
			const float total = std::max(std::hypot(b.center.x - x, b.center.z - z), 0.01f);
			const int maxSteps = static_cast<int>(total / step) * 3 + 40;
			for (int i = 0; i < maxSteps; ++i)
			{
				const float remaining = std::hypot(b.center.x - x, b.center.z - z);
				if (remaining < 0.6f) break;
				const float progress = std::clamp(1.0f - remaining / total, 0.0f, 1.0f);
				const float toward = std::atan2(b.center.x - x, b.center.z - z);
				const float wander = perlin.Noise(tunnelId * 7.31f, i * 0.06f, 3.7f) * cfg::kTunnelWander * (1.0f - progress * progress);
				const float heading = toward + wander;
				x += std::sin(heading) * step;
				z += std::cos(heading) * step;

				const float radius = cfg::kTunnelRadiusMin + (cfg::kTunnelRadiusMax - cfg::kTunnelRadiusMin) * (0.5f + 0.5f * perlin.Noise(tunnelId * 3.17f, i * 0.09f, 11.1f));
				x = std::clamp(x, kShell + radius, kWidth - kShell - radius);
				z = std::clamp(z, zone.z0 + 0.3f, zone.z1 - 0.3f);
				// Floor stays level with each room until the tunnel clears it, and climbs in
				// between, so there's never a ledge where a tunnel leaves a room.
				const float fromA = std::max(0.0f, std::hypot(x - a.center.x, z - a.center.z) - kRoomFloorShare * a.radius);
				const float fromB = std::max(0.0f, std::hypot(x - b.center.x, z - b.center.z) - kRoomFloorShare * b.radius);
				const float t = (fromA + fromB) > 0.001f ? fromA / (fromA + fromB) : 0.5f;
				const float floorY = a.center.y + (b.center.y - a.center.y) * t;
				const float ry = std::max(radius, cfg::kTunnelMinHeight);
				CarveEllipsoid({ x, floorY + ry * 0.5f, z }, radius, ry, floorY, zone.z0, zone.z1);
			}
		}

		// Air reachable from `start` within voxel depth range [zMin, zMax), marked in `visited`.
		void FloodAir(int start, int zMin, int zMax)
		{
			stack.clear();
			if (voxels[start] != Block::Air || visited[start]) return;
			visited[start] = 1;
			stack.push_back(start);
			static const int kSteps[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
			while (!stack.empty())
			{
				const int i = stack.back();
				stack.pop_back();
				const int x = i % cfg::kVoxX, y = (i / cfg::kVoxX) % cfg::kVoxY, z = i / (cfg::kVoxX * cfg::kVoxY);
				for (const auto& s : kSteps)
				{
					const int nx = x + s[0], ny = y + s[1], nz = z + s[2];
					if (!VoxelWorld::InGrid(nx, ny, nz) || nz < zMin || nz >= zMax) continue;
					const int n = VoxelWorld::Index(nx, ny, nz);
					if (visited[n] || voxels[n] != Block::Air) continue;
					visited[n] = 1;
					stack.push_back(n);
				}
			}
		}
	};

	int VoxelIndexAt(float x, float y, float z)
	{
		const int vx = std::clamp(static_cast<int>(std::floor(x * V)), 0, cfg::kVoxX - 1);
		const int vy = std::clamp(static_cast<int>(std::floor(y * V)), 0, cfg::kVoxY - 1);
		const int vz = std::clamp(static_cast<int>(std::floor(z * V)), 0, cfg::kVoxZ - 1);
		return VoxelWorld::Index(vx, vy, vz);
	}
}

XMFLOAT3 RoomStandPoint(const CaveRoom& room)
{
	return { room.center.x, room.center.y + 0.02f, room.center.z };
}

CaveLayout GenerateCaves(VoxelWorld& world, uint64_t seed)
{
	world.GenerateBasic();   // bedrock shell, solid stone, entrance passage (re-carved below)
	Generator g(world, seed);
	CaveLayout layout;
	layout.seed = seed;

	// Undo the basic chamber; keep the shell and stone.
	for (Block& b : g.voxels)
	{
		if (b == Block::Air) b = Block::Stone;
	}

	g.floorGuard.assign(g.voxels.size(), 0);
	g.wallNoise.Build(kWidth, kHeight, static_cast<float>(cfg::kGridZ), 1.0f, [&](float x, float y, float z)
	{
		return g.perlin.Fbm(x * cfg::kWallNoiseScale, y * cfg::kWallNoiseScale, z * cfg::kWallNoiseScale + 100.0f, 2);
	});
	g.cheeseNoise.Build(kWidth, kHeight, static_cast<float>(cfg::kGridZ), 1.0f, [&](float x, float y, float z)
	{
		// Squashed vertically: caverns come out wider than they are tall.
		return g.perlin.Fbm(x * cfg::kCheeseScale + 50.0f, y * cfg::kCheeseScale * 1.7f, z * cfg::kCheeseScale, 3);
	});

	// ---- Zones and seal bands ---------------------------------------------------
	const float zStart = cfg::kEntranceDepth;
	const float zEnd = cfg::kGridZ - kShell;
	std::vector<float> sealThickness(cfg::kZoneCount - 1);
	float sealTotal = 0.0f;
	for (float& t : sealThickness) { t = g.Rand(cfg::kSealMin, cfg::kSealMax); sealTotal += t; }
	std::vector<float> weights(cfg::kZoneCount);
	float weightTotal = 0.0f;
	for (float& w : weights) { w = g.Rand(0.75f, 1.25f); weightTotal += w; }
	const float zoneSpace = zEnd - zStart - sealTotal;
	float cursor = zStart;
	for (int i = 0; i < cfg::kZoneCount; ++i)
	{
		CaveZone zone;
		zone.z0 = cursor;
		zone.z1 = cursor + zoneSpace * weights[i] / weightTotal;
		cursor = zone.z1;
		if (i + 1 < cfg::kZoneCount)
		{
			layout.seals.emplace_back(cursor, cursor + sealThickness[i]);
			cursor += sealThickness[i];
		}
		layout.zones.push_back(zone);
	}
	layout.zones.back().z1 = zEnd;

	// ---- Rooms --------------------------------------------------------------------
	auto randomRoom = [&](const CaveZone& zone, bool upperAllowed)
	{
		CaveRoom room;
		const float zoneLength = zone.z1 - zone.z0;
		room.radius = g.Rand(cfg::kRoomRadiusMin, std::min(cfg::kRoomRadiusMax, zoneLength * 0.4f));
		room.height = g.Rand(cfg::kRoomHeightMin, cfg::kRoomHeightMax);
		room.center.x = g.Rand(kShell + room.radius + 1.0f, kWidth - kShell - room.radius - 1.0f);
		room.center.z = g.Rand(zone.z0 + room.radius, std::max(zone.z0 + room.radius, zone.z1 - room.radius));
		const float maxFloor = kHeight - kShell - room.height * 1.4f - 1.0f;
		room.center.y = (upperAllowed && g.Rand(0.0f, 1.0f) < cfg::kHighRoomChance) ? g.Rand(kShell + 3.0f, std::max(kShell + 3.0f, maxFloor)) : kShell;
		return room;
	};

	for (int zi = 0; zi < cfg::kZoneCount; ++zi)
	{
		CaveZone& zone = layout.zones[zi];

		CaveRoom entry = randomRoom(zone, false);
		entry.radius = std::min(entry.radius, 4.5f);
		entry.center.z = zone.z0 + entry.radius * 0.8f;
		if (zi == 0) entry.center.x = cfg::kCenterX + 0.5f;   // the crack opens into it
		CaveRoom exit = randomRoom(zone, zi > 0);
		exit.radius = std::min(exit.radius, 4.5f);
		exit.center.z = zone.z1 - exit.radius * 0.8f;          // pressed against the next seal
		zone.rooms = { entry, exit };

		const int extra = g.RandInt(cfg::kExtraRoomsMin, cfg::kExtraRoomsMax);
		for (int i = 0, attempts = 0; i < extra && attempts < extra * 15; ++attempts)
		{
			CaveRoom room = randomRoom(zone, true);
			bool crowded = false;
			for (const CaveRoom& other : zone.rooms)
			{
				const float d = std::hypot(room.center.x - other.center.x, room.center.z - other.center.z);
				if (d < (room.radius + other.radius) * 0.7f && std::fabs(room.center.y - other.center.y) < 3.0f) crowded = true;
			}
			if (crowded) continue;
			zone.rooms.push_back(room);
			++i;
		}

		// Minimum spanning tree over the rooms (Prim), then floors relaxed along it so
		// every tunnel's slope stays walkable.
		const int n = static_cast<int>(zone.rooms.size());
		auto horizontal = [&](int a, int b)
		{
			return std::hypot(zone.rooms[a].center.x - zone.rooms[b].center.x, zone.rooms[a].center.z - zone.rooms[b].center.z);
		};
		auto allowedRise = [&](int a, int b)
		{
			const float span = std::max(1.0f, horizontal(a, b) - kRoomFloorShare * (zone.rooms[a].radius + zone.rooms[b].radius));
			return cfg::kTunnelMaxSlope * 0.8f * span;
		};
		// A tunnel shouldn't run through a third room: if their floors differ, the tunnel
		// pokes out of that room's wall as a ledge you can't reach.
		auto crossesRoom = [&](int a, int b)
		{
			const XMFLOAT3& pa = zone.rooms[a].center;
			const XMFLOAT3& pb = zone.rooms[b].center;
			const float abx = pb.x - pa.x, abz = pb.z - pa.z;
			const float lengthSq = std::max(abx * abx + abz * abz, 1e-4f);
			for (int c = 0; c < n; ++c)
			{
				if (c == a || c == b) continue;
				const XMFLOAT3& pc = zone.rooms[c].center;
				const float t = std::clamp(((pc.x - pa.x) * abx + (pc.z - pa.z) * abz) / lengthSq, 0.0f, 1.0f);
				const float dx = pa.x + abx * t - pc.x, dz = pa.z + abz * t - pc.z;
				if (std::sqrt(dx * dx + dz * dz) < zone.rooms[c].radius + cfg::kTunnelRadiusMax) return true;
			}
			return false;
		};

		std::vector<int> parent(n, -1);
		std::vector<uint8_t> inTree(n, 0);
		inTree[0] = 1;
		std::vector<int> order = { 0 };
		for (int added = 1; added < n; ++added)
		{
			// Cheapest edge into the tree that doesn't cross another room; crossing ones only
			// if there's no other way to connect.
			float best = 1e30f;
			int bestFrom = -1, bestTo = -1;
			for (int pass = 0; pass < 2 && bestTo < 0; ++pass)
			{
				for (int a = 0; a < n; ++a)
				{
					if (!inTree[a]) continue;
					for (int b = 0; b < n; ++b)
					{
						if (inTree[b]) continue;
						if (pass == 0 && crossesRoom(a, b)) continue;
						const float cost = horizontal(a, b) + 2.0f * std::fabs(zone.rooms[a].center.y - zone.rooms[b].center.y);
						if (cost < best) { best = cost; bestFrom = a; bestTo = b; }
					}
				}
			}
			inTree[bestTo] = 1;
			parent[bestTo] = bestFrom;
			order.push_back(bestTo);
			zone.tunnels.emplace_back(bestFrom, bestTo);
		}
		// Two rules for floors, applied together a few times so they settle:
		//  - along each tunnel the climb stays within the walkable slope;
		//  - rooms that overlap on nearly the same level share one floor (otherwise their
		//    union leaves knee-high ledges).
		for (int pass = 0; pass < 4; ++pass)
		{
			for (int k = 1; k < n; ++k)
			{
				const int child = order[k];
				const int p = parent[child];
				const float rise = allowedRise(p, child);
				float& floorY = zone.rooms[child].center.y;
				floorY = std::clamp(floorY, std::max(kShell, zone.rooms[p].center.y - rise), zone.rooms[p].center.y + rise);
			}
			for (int a = 0; a < n; ++a)
			{
				for (int b = a + 1; b < n; ++b)
				{
					CaveRoom& ra = zone.rooms[a];
					CaveRoom& rb = zone.rooms[b];
					const bool overlap = horizontal(a, b) < ra.radius + rb.radius;
					if (overlap && std::fabs(ra.center.y - rb.center.y) < kSeparateLevels)
					{
						ra.center.y = rb.center.y = std::min(ra.center.y, rb.center.y);
					}
				}
			}
		}
		for (int loop = 0, attempts = 0; loop < cfg::kExtraLoops && attempts < 20 && n > 2; ++attempts)
		{
			const int a = g.RandInt(0, n - 1), b = g.RandInt(0, n - 1);
			if (a == b || parent[a] == b || parent[b] == a || crossesRoom(a, b)) continue;
			if (std::fabs(zone.rooms[a].center.y - zone.rooms[b].center.y) > allowedRise(a, b)) continue;
			const bool duplicate = std::any_of(zone.tunnels.begin(), zone.tunnels.end(), [&](const std::pair<int, int>& t)
			{
				return (t.first == a && t.second == b) || (t.first == b && t.second == a);
			});
			if (duplicate) continue;
			zone.tunnels.emplace_back(a, b);
			++loop;
		}

		for (const CaveRoom& room : zone.rooms) g.CarveRoom(room, zone.z0, zone.z1);
		for (size_t t = 0; t < zone.tunnels.size(); ++t)
		{
			g.CarveTunnel(zone.rooms[zone.tunnels[t].first], zone.rooms[zone.tunnels[t].second], zi * 100 + static_cast<int>(t), zone);
		}
	}

	// ---- Entrance crack into zone 0 -------------------------------------------------
	const int cx = cfg::kCenterX * V;
	for (int z = 0; z < static_cast<int>(zStart * V) + V; ++z)
		for (int y = V; y < V + cfg::kEntranceHeight * V; ++y)
			for (int x = cx; x < cx + V; ++x)
				g.voxels[VoxelWorld::Index(x, y, z)] = Block::Air;

	// ---- Noise caverns (kept only where they join a zone's network) -----------------
	for (const CaveZone& zone : layout.zones)
	{
		const int z0 = static_cast<int>(std::ceil((zone.z0 + 1.0f) * V));
		const int z1 = static_cast<int>(std::floor((zone.z1 - 1.0f) * V));
		for (int z = z0; z < z1; ++z)
		for (int y = static_cast<int>((kShell + 0.5f) * V); y < static_cast<int>((kHeight - kShell - 1.0f) * V); ++y)
		for (int x = V + 2; x < cfg::kVoxX - V - 2; ++x)
		{
			const int index = VoxelWorld::Index(x, y, z);
			Block& b = g.voxels[index];
			if (b != Block::Stone || g.floorGuard[index]) continue;
			if (g.cheeseNoise.Sample((x + 0.5f) * S, (y + 0.5f) * S, (z + 0.5f) * S) > cfg::kCheeseThreshold) b = Block::Air;
		}
	}

	// Each zone keeps only the air connected to its own network (the entrance counts as
	// zone 0's). Anything else is refilled, so zones never leak into each other.
	g.visited.assign(g.voxels.size(), 0);
	for (size_t zi = 0; zi < layout.zones.size(); ++zi)
	{
		const CaveZone& zone = layout.zones[zi];
		const int zMin = zi == 0 ? 0 : static_cast<int>(std::floor(zone.z0 * V));
		const int zMax = static_cast<int>(std::ceil(zone.z1 * V));
		const XMFLOAT3 p = RoomStandPoint(zone.rooms[0]);
		g.FloodAir(VoxelIndexAt(p.x, p.y + 0.5f, p.z), zMin, zMax);
		if (zi == 0) g.FloodAir(VoxelIndexAt(cfg::kCenterX + 0.5f, 1.5f, 0.1f), zMin, zMax);
	}
	for (size_t i = 0; i < g.voxels.size(); ++i)
	{
		if (g.voxels[i] == Block::Air && !g.visited[i]) g.voxels[i] = Block::Stone;
	}

	// ---- Sealed side pockets -------------------------------------------------------
	for (int zi = 0; zi < cfg::kZoneCount; ++zi)
	{
		const CaveZone& zone = layout.zones[zi];
		const int count = g.RandInt(cfg::kPocketsMin, cfg::kPocketsMax);
		for (int made = 0, attempts = 0; made < count && attempts < 60; ++attempts)
		{
			CaveRoom shape;
			shape.radius = g.Rand(cfg::kPocketRadiusMin, cfg::kPocketRadiusMax);
			shape.height = std::max(shape.radius * 0.75f, 1.2f);
			const float wall = cfg::kPocketWallMin + cfg::kWallNoiseAmount;
			const float reach = shape.radius + wall;
			if (zone.z1 - zone.z0 < 2.0f * reach) break;
			shape.center.x = g.Rand(kShell + reach, kWidth - kShell - reach);
			shape.center.z = g.Rand(zone.z0 + reach, zone.z1 - reach);
			shape.center.y = g.Rand(kShell, std::max(kShell, kHeight - kShell - shape.height * 1.4f - wall));

			// Every voxel within the pocket grown by the wall thickness must still be rock.
			const XMFLOAT3 c{ shape.center.x, shape.center.y + shape.height * 0.4f, shape.center.z };
			bool sealed = true;
			const float gx = shape.radius + wall, gy = shape.height + wall;
			for (int z = static_cast<int>((c.z - gx) * V); z <= static_cast<int>((c.z + gx) * V) && sealed; ++z)
			for (int y = std::max(static_cast<int>((shape.center.y - wall) * V), 0); y <= static_cast<int>((c.y + gy) * V) && sealed; ++y)
			for (int x = static_cast<int>((c.x - gx) * V); x <= static_cast<int>((c.x + gx) * V) && sealed; ++x)
			{
				if (!VoxelWorld::InGrid(x, y, z)) continue;
				const float dx = ((x + 0.5f) * S - c.x) / gx, dy = ((y + 0.5f) * S - c.y) / gy, dz = ((z + 0.5f) * S - c.z) / gx;
				if (dx * dx + dy * dy + dz * dz <= 1.0f && g.voxels[VoxelWorld::Index(x, y, z)] == Block::Air) sealed = false;
			}
			if (!sealed) continue;

			g.CarveRoom(shape, zone.z0, zone.z1);
			layout.pockets.push_back({ shape, zi });
			++made;
		}
	}

	// ---- Remove rock that isn't attached to the bedrock shell -------------------------
	// (Same rule as the in-game support check, so nothing collapses at the start.)
	g.visited.assign(g.voxels.size(), 0);
	g.stack.clear();
	const int startIndex = VoxelWorld::Index(0, 0, 0);   // a bedrock corner; the shell is one piece
	g.visited[startIndex] = 1;
	g.stack.push_back(startIndex);
	static const int kSteps[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
	while (!g.stack.empty())
	{
		const int i = g.stack.back();
		g.stack.pop_back();
		const int x = i % cfg::kVoxX, y = (i / cfg::kVoxX) % cfg::kVoxY, z = i / (cfg::kVoxX * cfg::kVoxY);
		for (const auto& s : kSteps)
		{
			const int nx = x + s[0], ny = y + s[1], nz = z + s[2];
			if (!VoxelWorld::InGrid(nx, ny, nz)) continue;
			const int n = VoxelWorld::Index(nx, ny, nz);
			if (g.visited[n] || g.voxels[n] == Block::Air) continue;
			g.visited[n] = 1;
			g.stack.push_back(n);
		}
	}
	for (size_t i = 0; i < g.voxels.size(); ++i)
	{
		if (g.voxels[i] != Block::Air && !g.visited[i])
		{
			g.voxels[i] = Block::Air;
			++layout.floatingVoxelsRemoved;
		}
		if (g.voxels[i] == Block::Air) ++layout.airVoxels;
	}
	return layout;
}
