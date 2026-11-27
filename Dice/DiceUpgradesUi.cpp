// The upgrades in the game (DiceUpgrades.h has the rules): the tree and the save, coins banked
// when a session ends, the stats applied on every deal, and the upgrade screen
// (Content/UI/Upgrades.rml, its "upgrades" data model).
#include "DiceModule.h"
#include <Core/Log.h>
#include <RmlUi/Core.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
	constexpr const char* kUpgradesDocument = "UI/Upgrades.rml";
	// A node's box in the tree, in dp (Upgrades.rcss's .node): links join their edges.
	constexpr float kNodeWidth = 150.0f, kNodeHeight = 64.0f;
	constexpr double kFlashSeconds = 0.45;

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		return file ? text.str() : std::string();
	}

	const char* EnvText(const char* name)
	{
		const char* value = std::getenv(name);
		return value && *value ? value : nullptr;
	}

	// A stat as the player reads it.
	std::string StatLabel(const std::string& stat)
	{
		if (stat == "luck") return "Luck";
		if (stat == "density") return "Dice dealt";
		if (stat == "sessionSeconds") return "Session";
		if (stat == "goldShare") return "Golden dice";
		if (stat == "luckyShare") return "Lucky dice";
		if (stat == "goldMultiplier") return "Golden dice score";
		if (stat == "luckyLuck") return "Lucky dice luck";
		return stat;
	}

	std::string StatValue(const std::string& stat, double value)
	{
		char text[32];
		if (stat == "density" || stat == "goldShare" || stat == "luckyShare") std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
		else if (stat == "sessionSeconds") std::snprintf(text, sizeof(text), "%.0f s", value);
		else if (stat == "goldMultiplier") std::snprintf(text, sizeof(text), "\xC3\x97%.0f", value);
		else std::snprintf(text, sizeof(text), "%.2f", value);
		return text;
	}

	std::string Dp(float value)
	{
		char text[32];
		std::snprintf(text, sizeof(text), "%.1fdp", value);
		return text;
	}
}

// ---- Tree, save, stats ----------------------------------------------------------------------

void DiceModule::LoadUpgrades(Engine& engine)
{
	std::vector<std::string> problems;
	const std::string tree = ReadFile(std::filesystem::path(engine.ContentPath(L"Upgrades.json")));
	if (tree.empty()) CHERRY_WARN("Dice: no Content/Upgrades.json (no upgrades)");
	else if (!dice::LoadUpgradeTree(tree, mUpgrades, problems))
		for (const std::string& problem : problems) CHERRY_WARN("Dice: Upgrades.json: %s", problem.c_str());

	// Development: DICE_DEV_COINS=N starts with N coins and DICE_DEV_LEVELS="id=n id=n" with those
	// levels, without reading or writing the save; DICE_DEV_PROGRESSION=0|1 switches upgrades off
	// or on (off by default with the prototype's dev switches: density, preset, profile).
	mProgressSaves = true;
	mProgress = {};
	if (const char* coins = EnvText("DICE_DEV_COINS"))
	{
		mProgressSaves = false;
		mProgress.coins = std::atoll(coins);
		if (const char* levels = EnvText("DICE_DEV_LEVELS"))
		{
			std::istringstream words(levels);
			std::string word;
			while (words >> word)
				if (const size_t equals = word.find('='); equals != std::string::npos)
					mProgress.levels[word.substr(0, equals)] = std::atoi(word.c_str() + equals + 1);
		}
	}
	else if (const std::string saved = ReadFile(engine.SavedDir() / "Progress.json"); !saved.empty())
	{
		if (!dice::LoadProgress(saved, mProgress)) CHERRY_WARN("Dice: Saved/Progress.json isn't valid (starting over)");
	}
	mProgression = !(EnvText("DICE_DEV_DENSITY") || EnvText("DICE_DEV_PRESET") || EnvText("DICE_DEV_PROFILE"));
	if (const char* progression = EnvText("DICE_DEV_PROGRESSION")) mProgression = std::atoi(progression) != 0;
	mUpgradeBoughtAt.assign(mUpgrades.nodes.size(), -10.0);
	mSessionBanked = true;
}

void DiceModule::SaveUpgradeProgress(Engine& engine)
{
	if (!mProgressSaves) return;
	std::ofstream file(engine.SavedDir() / "Progress.json", std::ios::binary);
	file << dice::SaveProgress(mProgress);
	if (!file) CHERRY_WARN("Dice: couldn't write Saved/Progress.json");
}

void DiceModule::ApplyUpgradeStats()
{
	if (!mProgression)
	{
		mSim.kinds = {};
		return;
	}
	std::map<std::string, double> stats = dice::StatsFor(mUpgrades, mProgress);
	mSim.config.luck = static_cast<float>(stats["luck"]);
	mSim.config.density = static_cast<float>(stats["density"]);
	mSim.config.sessionSeconds = static_cast<float>(stats["sessionSeconds"]);
	mSim.kinds.goldShare = stats["goldShare"];
	mSim.kinds.luckyShare = stats["luckyShare"];
	mSim.kinds.goldMultiplier = stats["goldMultiplier"];
	mSim.kinds.luckyLuck = stats["luckyLuck"];
}

// A session that just ended banks its coins (once).
void DiceModule::BankSessionCoins(Engine& engine)
{
	if (mSessionBanked || mSim.phase != dice::Phase::Ended) return;
	mSessionBanked = true;
	const long long coins = dice::CoinsFor(mUpgrades, mSim.score);
	mProgress.coins += coins;
	mProgress.earned += coins;
	char text[48];
	std::snprintf(text, sizeof(text), "+%lld coins", coins);
	mCoinsEarned = text;
	SaveUpgradeProgress(engine);
}

void DiceModule::BuyUpgrade(Engine& engine, int index)
{
	if (index < 0 || index >= static_cast<int>(mUpgrades.nodes.size())) return;
	const dice::UpgradeNode& node = mUpgrades.nodes[index];
	const std::map<std::string, double> before = dice::StatsFor(mUpgrades, mProgress);
	if (!dice::Buy(mUpgrades, mProgress, node.id)) return;
	mUpgradeBoughtAt[index] = mSim.now;
	SaveUpgradeProgress(engine);
	mAudio.Start();
	mAudio.Play(dice::Sound::Clack, mSim.config.volume, mSim.config.landTicks);
	// More dice or new die types need a new deal (not mid-session: the screen is closed then).
	const std::map<std::string, double> after = dice::StatsFor(mUpgrades, mProgress);
	bool redeal = false;
	for (const char* stat : { "density", "goldShare", "luckyShare" })
		redeal |= before.at(stat) != after.at(stat);
	if (redeal && CanOpenUpgrades()) BuildBoard(engine);
	else ApplyUpgradeStats();
}

void DiceModule::SetUpgradesOpen(bool open)
{
	if (open && !CanOpenUpgrades()) return;
	if (open == mUpgradesOpen) return;
	mUpgradesOpen = open;
	mUpgradeHover = -1;
	if (mUpgradesModel) mUpgradesModel.DirtyVariable("open");
}

// ---- The screen -----------------------------------------------------------------------------

void DiceModule::CreateUpgradesUi(Engine& engine)
{
	Rml::Context* context = engine.GetUi().GetContext();
	if (!context) return;
	Rml::DataModelConstructor model = context->CreateDataModel("upgrades");
	if (!model) return;
	if (Rml::StructHandle<UpgradeNodeView> node = model.RegisterStruct<UpgradeNodeView>())
	{
		node.RegisterMember("name", &UpgradeNodeView::name);
		node.RegisterMember("level", &UpgradeNodeView::level);
		node.RegisterMember("cost", &UpgradeNodeView::cost);
		node.RegisterMember("left", &UpgradeNodeView::left);
		node.RegisterMember("top", &UpgradeNodeView::top);
		node.RegisterMember("locked", &UpgradeNodeView::locked);
		node.RegisterMember("expensive", &UpgradeNodeView::expensive);
		node.RegisterMember("buyable", &UpgradeNodeView::buyable);
		node.RegisterMember("maxed", &UpgradeNodeView::maxed);
		node.RegisterMember("flash", &UpgradeNodeView::flash);
	}
	model.RegisterArray<std::vector<UpgradeNodeView>>();
	if (Rml::StructHandle<UpgradeLinkView> link = model.RegisterStruct<UpgradeLinkView>())
	{
		link.RegisterMember("left", &UpgradeLinkView::left);
		link.RegisterMember("top", &UpgradeLinkView::top);
		link.RegisterMember("width", &UpgradeLinkView::width);
		link.RegisterMember("transform", &UpgradeLinkView::transform);
		link.RegisterMember("met", &UpgradeLinkView::met);
	}
	model.RegisterArray<std::vector<UpgradeLinkView>>();
	model.Bind("nodes", &mUpgradeNodes);
	model.Bind("links", &mUpgradeLinks);
	model.Bind("open", &mUpgradesOpen);
	model.Bind("coins", &mCoins);
	model.Bind("detailName", &mDetailName);
	model.Bind("detailText", &mDetailText);
	model.Bind("detailNext", &mDetailNext);
	model.BindEventCallback("buy", [this, &engine](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
	{
		if (!arguments.empty()) BuyUpgrade(engine, arguments[0].Get<int>(-1));
	});
	model.BindEventCallback("hover", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
	{
		mUpgradeHover = arguments.empty() ? -1 : arguments[0].Get<int>(-1);
	});
	model.BindEventCallback("close", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { SetUpgradesOpen(false); });
	mUpgradesModel = model.GetModelHandle();

	// The links: from the right edge of each requirement to the left edge of the node needing it.
	mUpgradeLinks.clear();
	mUpgradeNodes.assign(mUpgrades.nodes.size(), UpgradeNodeView{});
	for (const dice::UpgradeNode& node : mUpgrades.nodes)
	{
		for (const dice::UpgradeRequirement& requirement : node.requirements)
		{
			const dice::UpgradeNode* from = mUpgrades.Find(requirement.id);
			if (!from) continue;
			const float x0 = from->x + kNodeWidth, y0 = from->y + kNodeHeight * 0.5f;
			const float x1 = node.x, y1 = node.y + kNodeHeight * 0.5f;
			UpgradeLinkView link;
			link.left = Dp(x0);
			link.top = Dp(y0);
			link.width = Dp(std::hypot(x1 - x0, y1 - y0));
			char rotate[48];
			std::snprintf(rotate, sizeof(rotate), "rotate(%.4frad)", std::atan2(y1 - y0, x1 - x0));
			link.transform = rotate;
			mUpgradeLinks.push_back(link);
		}
	}
	engine.GetUi().LoadDocument(kUpgradesDocument);
	if (EnvText("DICE_DEV_UPGRADES")) SetUpgradesOpen(true);
	UpdateUpgradesUi(engine);
}

void DiceModule::UpdateUpgradesUi(Engine& engine)
{
	(void)engine;
	if (!mUpgradesModel) return;
	auto set = [&](std::string& field, const std::string& value, const char* name)
	{
		if (field == value) return;
		field = value;
		mUpgradesModel.DirtyVariable(name);
	};
	char text[160];
	std::snprintf(text, sizeof(text), "%lld", mProgress.coins);
	set(mCoins, text, "coins");
	if (!CanOpenUpgrades()) SetUpgradesOpen(false);   // a session started, or progression went off
	if (!mUpgradesOpen) return;

	// Nodes.
	bool changed = mUpgradeNodes.size() != mUpgrades.nodes.size();
	if (changed) mUpgradeNodes.resize(mUpgrades.nodes.size());
	for (size_t i = 0; i < mUpgrades.nodes.size(); ++i)
	{
		const dice::UpgradeNode& node = mUpgrades.nodes[i];
		const int owned = dice::LevelOf(mProgress, node.id);
		const dice::UpgradeState state = dice::StateOf(mUpgrades, mProgress, node);
		UpgradeNodeView view;
		view.name = node.name;
		std::snprintf(text, sizeof(text), "%d / %d", owned, node.levels);
		view.level = text;
		if (state == dice::UpgradeState::Maxed) view.cost = "max";
		else if (state == dice::UpgradeState::Locked) view.cost = "locked";
		else { std::snprintf(text, sizeof(text), "%lld", dice::NextCost(node, owned)); view.cost = text; }
		view.left = Dp(node.x);
		view.top = Dp(node.y);
		view.locked = state == dice::UpgradeState::Locked;
		view.expensive = state == dice::UpgradeState::TooExpensive;
		view.buyable = state == dice::UpgradeState::Buyable;
		view.maxed = state == dice::UpgradeState::Maxed;
		view.flash = mSim.now - mUpgradeBoughtAt[i] < kFlashSeconds;
		UpgradeNodeView& shown = mUpgradeNodes[i];
		if (shown.name != view.name || shown.level != view.level || shown.cost != view.cost || shown.left != view.left || shown.top != view.top
			|| shown.locked != view.locked || shown.expensive != view.expensive || shown.buyable != view.buyable || shown.maxed != view.maxed
			|| shown.flash != view.flash)
		{
			shown = view;
			changed = true;
		}
	}
	if (changed) mUpgradesModel.DirtyVariable("nodes");

	// Links: lit once their requirement is met.
	size_t k = 0;
	bool linksChanged = false;
	for (const dice::UpgradeNode& node : mUpgrades.nodes)
		for (const dice::UpgradeRequirement& requirement : node.requirements)
		{
			if (!mUpgrades.Find(requirement.id) || k >= mUpgradeLinks.size()) continue;
			const bool met = dice::LevelOf(mProgress, requirement.id) >= requirement.level;
			if (mUpgradeLinks[k].met != met) { mUpgradeLinks[k].met = met; linksChanged = true; }
			++k;
		}
	if (linksChanged) mUpgradesModel.DirtyVariable("links");

	// The details of the node under the mouse.
	if (mUpgradeHover < 0 || mUpgradeHover >= static_cast<int>(mUpgrades.nodes.size()))
	{
		set(mDetailName, "Upgrades", "detailName");
		std::snprintf(text, sizeof(text), "Finished sessions bank coins: %g per point. Click an upgrade to buy its next level.", mUpgrades.coinsPerPoint);
		set(mDetailText, text, "detailText");
		set(mDetailNext, "", "detailNext");
		return;
	}
	const dice::UpgradeNode& node = mUpgrades.nodes[mUpgradeHover];
	const int owned = dice::LevelOf(mProgress, node.id);
	set(mDetailName, node.name, "detailName");
	set(mDetailText, node.description, "detailText");
	std::string next;
	if (owned >= node.levels) next = "All levels owned.";
	else
	{
		dice::Progress after = mProgress;
		after.levels[node.id] = owned + 1;
		const std::map<std::string, double> now = dice::StatsFor(mUpgrades, mProgress), then = dice::StatsFor(mUpgrades, after);
		std::snprintf(text, sizeof(text), "Level %d for %lld coins:", owned + 1, dice::NextCost(node, owned));
		next = text;
		for (const dice::UpgradeEffect& effect : node.effects)
			next += "  " + StatLabel(effect.stat) + " " + StatValue(effect.stat, now.at(effect.stat)) + " \xE2\x86\x92 " + StatValue(effect.stat, then.at(effect.stat));
		if (dice::StateOf(mUpgrades, mProgress, node) == dice::UpgradeState::Locked)
		{
			next += "  (needs";
			for (const dice::UpgradeRequirement& requirement : node.requirements)
				if (const dice::UpgradeNode* other = mUpgrades.Find(requirement.id))
					next += " " + other->name + " " + std::to_string(requirement.level);
			next += ")";
		}
	}
	set(mDetailNext, next, "detailNext");
}

// ---- The debug panel's section ----------------------------------------------------------------

void DiceModule::DrawUpgradesPanel(Engine& engine)
{
	ImGui::SeparatorText("Upgrades");
	bool progression = mProgression;
	if (ImGui::Checkbox("Upgrades set luck, dice, time, die types", &progression))
	{
		mProgression = progression;
		if (!mProgression)
		{
			// Back to the prototype's values for what the upgrades were setting.
			const DiceConfig defaults;
			mSim.config.luck = defaults.luck;
			mSim.config.density = defaults.density;
			mSim.config.sessionSeconds = defaults.sessionSeconds;
		}
		if (mSim.phase != dice::Phase::Running && mSim.phase != dice::Phase::Finishing) BuildBoard(engine);
	}
	ImGui::Text("%lld coins (%lld earned)%s", mProgress.coins, mProgress.earned, mProgressSaves ? "" : "  [dev: not saved]");
	if (ImGui::Button("+100 coins")) { mProgress.coins += 100; SaveUpgradeProgress(engine); }
	ImGui::SameLine();
	if (ImGui::Button("+1000 coins")) { mProgress.coins += 1000; SaveUpgradeProgress(engine); }
	ImGui::SameLine();
	if (ImGui::Button("Reset progress"))
	{
		mProgress = {};
		SaveUpgradeProgress(engine);
		if (CanOpenUpgrades()) BuildBoard(engine);
	}
	if (ImGui::Button(mUpgradesOpen ? "Close upgrades (U)" : "Open upgrades (U)")) SetUpgradesOpen(!mUpgradesOpen);
}
