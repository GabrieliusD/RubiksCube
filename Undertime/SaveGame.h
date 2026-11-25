#pragma once
// The save game (Docs/NPCs.md §9): <project>/Saved/SaveGame.json. It keeps the outside clock,
// where you stand, your backpack and gear, and the player log (everything about the people).
// Not yet: the mined cave and loose blocks (the mountain comes back from its seed, unmined).
// Pure C++ (the round trip is tested in UndertimeTests).
#include "NpcLog.h"
#include <DirectXMath.h>
#include <cstdint>
#include <map>
#include <string>

struct SaveGame
{
	static constexpr int kVersion = 1;
	uint32_t seed = 0;
	double days = 0.0;               // the outside clock
	double personal = 0.0;           // your own time (seconds)
	int era = 1;
	DirectX::XMFLOAT3 feet{ 0, 0, 0 };
	float yaw = 0.0f, pitch = 0.0f;
	std::map<std::string, int> backpack;   // by block name
	int pickLevel = 0, lanternLevel = 0;
	npc::PlayerLog log;
	// Money, goods, the barrow, the claim, crops, machines... (Hauling.cpp writes and reads it).
	nlohmann::json hauling;
	std::string date;                // the outside date when saved ("14:05, 3 June 1871"), for the title screen

	nlohmann::json ToJson() const;
	bool FromJson(const nlohmann::json& json, std::string& error);
	// Writes through a temporary file, so a crash mid-write never leaves a broken save.
	bool Write(const std::string& path, std::string& error) const;
	bool Read(const std::string& path, std::string& error);
};
