#include "DiceUpgrades.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

using nlohmann::json;

namespace dice
{
	const StatInfo* FindStat(const std::string& name)
	{
		for (const StatInfo& stat : kStats)
			if (name == stat.name) return &stat;
		return nullptr;
	}

	const UpgradeNode* UpgradeTree::Find(const std::string& id) const
	{
		for (const UpgradeNode& node : nodes)
			if (node.id == id) return &node;
		return nullptr;
	}

	bool LoadUpgradeTree(const std::string& text, UpgradeTree& tree, std::vector<std::string>& problems)
	{
		tree = {};
		const size_t problemsBefore = problems.size();
		json root;
		try
		{
			root = json::parse(text);
		}
		catch (const std::exception& error)
		{
			problems.push_back(std::string("not JSON: ") + error.what());
			return false;
		}
		if (!root.is_object())
		{
			problems.push_back("the file isn't a JSON object");
			return false;
		}
		tree.coinsPerPoint = root.value("coinsPerPoint", 0.1);
		if (root.contains("start") && root["start"].is_object())
		{
			for (auto& [name, value] : root["start"].items())
			{
				if (!FindStat(name)) problems.push_back("start: unknown stat \"" + name + "\"");
				else if (!value.is_number()) problems.push_back("start: \"" + name + "\" isn't a number");
				else tree.start[name] = value.get<double>();
			}
		}
		if (!root.contains("nodes") || !root["nodes"].is_array())
		{
			problems.push_back("no \"nodes\" list");
			return false;
		}
		std::set<std::string> ids;
		for (const json& item : root["nodes"])
		{
			UpgradeNode node;
			node.id = item.value("id", std::string());
			node.name = item.value("name", node.id);
			node.description = item.value("description", std::string());
			node.x = item.value("x", 0.0f);
			node.y = item.value("y", 0.0f);
			node.levels = std::max(1, item.value("levels", 1));
			node.cost = item.value("cost", 10.0);
			node.costGrowth = item.value("costGrowth", 1.5);
			const std::string where = "node \"" + node.id + "\"";
			if (node.id.empty()) problems.push_back("a node without an \"id\"");
			else if (!ids.insert(node.id).second) problems.push_back(where + ": the id is used twice");
			if (node.cost < 0 || node.costGrowth < 1) problems.push_back(where + ": cost must be >= 0 and costGrowth >= 1");
			if (item.contains("effects"))
			{
				for (const json& effect : item["effects"])
				{
					UpgradeEffect e{ effect.value("stat", std::string()), effect.value("add", 0.0) };
					if (!FindStat(e.stat)) problems.push_back(where + ": unknown stat \"" + e.stat + "\"");
					node.effects.push_back(e);
				}
			}
			if (item.contains("requires"))
			{
				for (const json& requirement : item["requires"])
					node.requirements.push_back({ requirement.value("id", std::string()), std::max(1, requirement.value("level", 1)) });
			}
			tree.nodes.push_back(std::move(node));
		}
		// Requirements name nodes that exist, at levels they have, without cycles.
		for (const UpgradeNode& node : tree.nodes)
		{
			for (const UpgradeRequirement& requirement : node.requirements)
			{
				const UpgradeNode* other = tree.Find(requirement.id);
				if (!other) problems.push_back("node \"" + node.id + "\" requires \"" + requirement.id + "\", which doesn't exist");
				else if (requirement.level > other->levels)
					problems.push_back("node \"" + node.id + "\" requires level " + std::to_string(requirement.level) + " of \"" + requirement.id + "\", which has " + std::to_string(other->levels));
			}
		}
		std::map<std::string, int> mark;   // 1 visiting, 2 done
		std::function<bool(const UpgradeNode&)> visit = [&](const UpgradeNode& node) -> bool
		{
			int& state = mark[node.id];
			if (state == 1) return false;
			if (state == 2) return true;
			state = 1;
			for (const UpgradeRequirement& requirement : node.requirements)
				if (const UpgradeNode* other = tree.Find(requirement.id); other && !visit(*other)) return false;
			state = 2;
			return true;
		};
		for (const UpgradeNode& node : tree.nodes)
			if (!visit(node)) { problems.push_back("node \"" + node.id + "\" is part of a requirement cycle"); break; }
		return problems.size() == problemsBefore;
	}

	std::string SaveUpgradeTree(const UpgradeTree& tree)
	{
		json root;
		root["coinsPerPoint"] = tree.coinsPerPoint;
		root["start"] = json::object();
		for (const auto& [name, value] : tree.start) root["start"][name] = value;
		root["nodes"] = json::array();
		for (const UpgradeNode& node : tree.nodes)
		{
			json item;
			item["id"] = node.id;
			item["name"] = node.name;
			item["description"] = node.description;
			item["x"] = node.x;
			item["y"] = node.y;
			item["levels"] = node.levels;
			item["cost"] = node.cost;
			item["costGrowth"] = node.costGrowth;
			item["effects"] = json::array();
			for (const UpgradeEffect& effect : node.effects) item["effects"].push_back({ { "stat", effect.stat }, { "add", effect.add } });
			item["requires"] = json::array();
			for (const UpgradeRequirement& requirement : node.requirements) item["requires"].push_back({ { "id", requirement.id }, { "level", requirement.level } });
			root["nodes"].push_back(std::move(item));
		}
		return root.dump(1, '\t') + "\n";
	}

	int LevelOf(const Progress& progress, const std::string& id)
	{
		auto found = progress.levels.find(id);
		return found == progress.levels.end() ? 0 : found->second;
	}

	std::string SaveProgress(const Progress& progress)
	{
		json root;
		root["coins"] = progress.coins;
		root["earned"] = progress.earned;
		root["levels"] = json::object();
		for (const auto& [id, level] : progress.levels)
			if (level > 0) root["levels"][id] = level;
		return root.dump(1, '\t') + "\n";
	}

	bool LoadProgress(const std::string& text, Progress& progress)
	{
		progress = {};
		try
		{
			const json root = json::parse(text);
			progress.coins = root.value("coins", 0LL);
			progress.earned = root.value("earned", progress.coins);
			if (root.contains("levels"))
				for (auto& [id, level] : root["levels"].items())
					if (level.is_number_integer() && level.get<int>() > 0) progress.levels[id] = level.get<int>();
			return true;
		}
		catch (const std::exception&)
		{
			progress = {};
			return false;
		}
	}

	long long NextCost(const UpgradeNode& node, int ownedLevels)
	{
		if (ownedLevels >= node.levels) return 0;
		return std::llround(node.cost * std::pow(node.costGrowth, ownedLevels));
	}

	UpgradeState StateOf(const UpgradeTree& tree, const Progress& progress, const UpgradeNode& node)
	{
		const int owned = LevelOf(progress, node.id);
		if (owned >= node.levels) return UpgradeState::Maxed;
		if (owned == 0)
		{
			for (const UpgradeRequirement& requirement : node.requirements)
				if (!tree.Find(requirement.id) || LevelOf(progress, requirement.id) < requirement.level) return UpgradeState::Locked;
		}
		return progress.coins >= NextCost(node, owned) ? UpgradeState::Buyable : UpgradeState::TooExpensive;
	}

	bool Buy(const UpgradeTree& tree, Progress& progress, const std::string& id)
	{
		const UpgradeNode* node = tree.Find(id);
		if (!node || StateOf(tree, progress, *node) != UpgradeState::Buyable) return false;
		progress.coins -= NextCost(*node, LevelOf(progress, id));
		progress.levels[id]++;
		return true;
	}

	long long CoinsFor(const UpgradeTree& tree, long long score)
	{
		return std::max(0LL, static_cast<long long>(std::floor(static_cast<double>(score) * tree.coinsPerPoint)));
	}

	std::map<std::string, double> StatsFor(const UpgradeTree& tree, const Progress& progress)
	{
		std::map<std::string, double> stats;
		for (const StatInfo& stat : kStats) stats[stat.name] = stat.base;
		for (const auto& [name, value] : tree.start) stats[name] = value;
		for (const UpgradeNode& node : tree.nodes)
		{
			const int owned = std::min(LevelOf(progress, node.id), node.levels);
			for (const UpgradeEffect& effect : node.effects)
				if (FindStat(effect.stat)) stats[effect.stat] += effect.add * owned;
		}
		for (const StatInfo& stat : kStats) stats[stat.name] = std::clamp(stats[stat.name], stat.min, stat.max);
		return stats;
	}
}
