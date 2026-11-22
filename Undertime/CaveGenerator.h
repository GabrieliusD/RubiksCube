#pragma once
#include "VoxelWorld.h"
#include <DirectXMath.h>
#include <cstdint>
#include <utility>
#include <vector>

// Procedural mountain interior (Minecraft-style caves, organised for progression):
//
//   entrance crack -> zone 0 | seal | zone 1 | seal | ... | zone N-1 | far wall
//
// Each zone is one connected cave network (caverns joined by wandering tunnels, plus
// noise caverns that touch them). Zones are separated by solid seal bands you have to
// dig through, and each zone hides a few sealed side pockets.

struct CaveRoom
{
	DirectX::XMFLOAT3 center{ 0, 0, 0 };   // horizontal centre, y = floor height (m)
	float radius = 4.0f;                   // horizontal radius (m)
	float height = 3.0f;                   // vertical radius (m)
};

struct CaveZone
{
	float z0 = 0.0f, z1 = 0.0f;            // depth range (m)
	std::vector<CaveRoom> rooms;           // [0] = entry cavern, [1] = exit cavern
	std::vector<std::pair<int, int>> tunnels;
};

// A natural shaft (cfg::kShafts): where it opens at the grid's top and where it reaches its
// cavern.
struct CaveShaft
{
	DirectX::XMFLOAT3 top{ 0, 0, 0 };
	DirectX::XMFLOAT3 bottom{ 0, 0, 0 };
	int zone = 0;
	int room = 0;                          // its cavern in zones[zone].rooms
};

struct CavePocket
{
	CaveRoom shape;
	int zone = 0;
};

struct CaveLayout
{
	uint64_t seed = 0;
	std::vector<CaveZone> zones;
	std::vector<std::pair<float, float>> seals;   // depth ranges (m) of the solid bands between zones
	std::vector<CavePocket> pockets;
	std::vector<CaveShaft> shafts;                   // cfg::kShafts, in order
	int floatingVoxelsRemoved = 0;
	int airVoxels = 0;
	int veins = 0;
	std::vector<DirectX::XMFLOAT3> watches;            // relic positions (m)
	int oreVoxels[static_cast<int>(cfg::Block::Count)] = {};
};

// Fills `world` with a new mountain for `seed`. Same seed, same mountain.
CaveLayout GenerateCaves(VoxelWorld& world, uint64_t seed);

// Feet position for standing in a room (teleports, tests).
DirectX::XMFLOAT3 RoomStandPoint(const CaveRoom& room);
