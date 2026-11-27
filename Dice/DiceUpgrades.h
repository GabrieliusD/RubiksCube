#pragma once
// The upgrade tree (not in the reference): coins earned from session scores buy levels of
// upgrades that change the game's stats (luck, how many dice are dealt, session length, die
// types). Pure C++: the tree is data (Content/Upgrades.json, edited by hand or in the editor),
// the player's progress is saved by the module (Saved/Progress.json), and the screen is an
// RmlUi document (Content/UI/Upgrades.rml, DiceUpgradesUi.cpp).
//
// Upgrades.json:
//   {
//     "coinsPerPoint": 0.1,                 // coins banked per point of a finished session's score
//     "start": { "density": 0.3 },          // stats before any upgrade (the rest: kStats' defaults)
//     "nodes": [
//       { "id": "luck", "name": "Loaded dice", "description": "...",
//         "x": 0, "y": 0,                   // place in the tree, in dp from its top-left
//         "levels": 5, "cost": 60, "costGrowth": 1.8,   // level n+1 costs round(cost x growth^n)
//         "effects": [ { "stat": "luck", "add": 0.12 } ],    // per level owned
//         "requires": [ { "id": "dice", "level": 2 } ] }      // to buy its first level
//     ]
//   }
#include <map>
#include <string>
#include <vector>

namespace dice
{
	// The stats upgrades can change, with their values before any upgrade (Upgrades.json's
	// "start" overrides these).
	struct StatInfo
	{
		const char* name;
		double base;
		double min, max;
		const char* description;
	};
	inline constexpr StatInfo kStats[] = {
		{ "luck", 0.0, 0.0, 3.0, "bias of every roll towards six (the debug panel's luck)" },
		{ "density", 0.8, 0.0, 1.0, "chance each slot is dealt a die" },
		{ "sessionSeconds", 30.0, 5.0, 300.0, "length of a session" },
		{ "goldShare", 0.0, 0.0, 1.0, "share of the dealt dice that are golden" },
		{ "luckyShare", 0.0, 0.0, 1.0, "share of the dealt dice that are lucky" },
		{ "goldMultiplier", 2.0, 1.0, 20.0, "points a golden die scores, times a normal one's" },
		{ "luckyLuck", 1.5, 0.0, 3.0, "luck a lucky die adds to its own rolls" },
	};
	const StatInfo* FindStat(const std::string& name);

	struct UpgradeEffect
	{
		std::string stat;
		double add = 0;   // per level
	};

	struct UpgradeRequirement
	{
		std::string id;
		int level = 1;
	};

	struct UpgradeNode
	{
		std::string id, name, description;
		float x = 0, y = 0;
		int levels = 1;
		double cost = 10;
		double costGrowth = 1.5;
		std::vector<UpgradeEffect> effects;
		std::vector<UpgradeRequirement> requirements;   // "requires" in the file
	};

	struct UpgradeTree
	{
		double coinsPerPoint = 0.1;
		std::map<std::string, double> start;
		std::vector<UpgradeNode> nodes;

		const UpgradeNode* Find(const std::string& id) const;
	};

	// Reads Upgrades.json. Problems (bad JSON, unknown stats, requirements naming missing nodes
	// or levels a node doesn't have, duplicate ids, requirement cycles) are listed; the tree is
	// still filled as far as it could be. True if there were none.
	bool LoadUpgradeTree(const std::string& json, UpgradeTree& tree, std::vector<std::string>& problems);
	std::string SaveUpgradeTree(const UpgradeTree& tree);

	// What the player owns.
	struct Progress
	{
		long long coins = 0;
		long long earned = 0;             // every coin banked so far
		std::map<std::string, int> levels;
	};
	int LevelOf(const Progress& progress, const std::string& id);
	std::string SaveProgress(const Progress& progress);
	bool LoadProgress(const std::string& json, Progress& progress);

	// The price of the node's next level (0 if it's maxed).
	long long NextCost(const UpgradeNode& node, int ownedLevels);
	enum class UpgradeState { Locked, TooExpensive, Buyable, Maxed };
	UpgradeState StateOf(const UpgradeTree& tree, const Progress& progress, const UpgradeNode& node);
	// Buys the node's next level if it's Buyable (spends the coins).
	bool Buy(const UpgradeTree& tree, Progress& progress, const std::string& id);
	// Coins a finished session banks.
	long long CoinsFor(const UpgradeTree& tree, long long score);

	// Every stat: its start value plus each owned level's effects, clamped to the stat's range.
	std::map<std::string, double> StatsFor(const UpgradeTree& tree, const Progress& progress);
}
