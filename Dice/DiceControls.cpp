#include "DiceControls.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dice
{
	const std::vector<Field>& Fields()
	{
		using C = DiceConfig;
		static const std::vector<Field> fields = {
			{ "layout", nullptr, nullptr, nullptr, 1 },
			{ "boardShape", nullptr, nullptr, nullptr, 2 },
			{ "boardSize", nullptr, &C::boardSize },
			{ "scatterSpacing", &C::scatterSpacing },
			{ "density", &C::density },
			{ "dieSize", &C::dieSize },
			{ "slotJitter", &C::slotJitter },
			{ "hoverRadius", &C::hoverRadius },
			{ "rollDistMin", &C::rollDistMin },
			{ "rollDistMax", &C::rollDistMax },
			{ "rollDuration", &C::rollDuration },
			{ "durationJitter", &C::durationJitter },
			{ "hopHeight", &C::hopHeight },
			{ "tumblePerCell", &C::tumblePerCell },
			{ "squash", &C::squash },
			{ "cooldown", &C::cooldown },
			{ "readyGlowTime", &C::readyGlowTime },
			{ "luck", &C::luck },
			{ "collisionMode", nullptr, nullptr, nullptr, 3 },
			{ "stackMax", nullptr, &C::stackMax },
			{ "stackChance", &C::stackChance },
			{ "stackScorePerLevel", &C::stackScorePerLevel },
			{ "toppleBonus", &C::toppleBonus },
			{ "toppleSpreadDeg", &C::toppleSpreadDeg },
			{ "toppleDistMin", &C::toppleDistMin },
			{ "toppleDistMax", &C::toppleDistMax },
			{ "bumpChainLimit", nullptr, &C::bumpChainLimit },
			{ "bumpsPerSecond", &C::bumpsPerSecond },
			{ "bumpDistMin", &C::bumpDistMin },
			{ "bumpDistMax", &C::bumpDistMax },
			{ "bumpSpreadDeg", &C::bumpSpreadDeg },
			{ "bumpScoreMult", &C::bumpScoreMult },
			{ "bumpContact", &C::bumpContact },
			{ "bumpNormalWeight", &C::bumpNormalWeight },
			{ "bumpHopMult", &C::bumpHopMult },
			{ "bumpDurationMult", &C::bumpDurationMult },
			{ "bumpRecoil", &C::bumpRecoil },
			{ "bounceChance", &C::bounceChance },
			{ "bounceDistMin", &C::bounceDistMin },
			{ "bounceDistMax", &C::bounceDistMax },
			{ "bounceSpreadDeg", &C::bounceSpreadDeg },
			{ "bounceTime", &C::bounceTime },
			{ "bounceHopMult", &C::bounceHopMult },
			{ "hitWobble", &C::hitWobble },
			{ "clashEnabled", nullptr, nullptr, &C::clashEnabled },
			{ "clashDistance", &C::clashDistance },
			{ "clashDeflect", &C::clashDeflect },
			{ "clashBonus", &C::clashBonus },
			{ "midAirCollide", nullptr, nullptr, &C::midAirCollide },
			{ "midAirContact", &C::midAirContact },
			{ "midAirDeflect", &C::midAirDeflect },
			{ "midAirMaxHits", nullptr, &C::midAirMaxHits },
			{ "pointsPerPip", &C::pointsPerPip },
			{ "sessionSeconds", &C::sessionSeconds },
			{ "volume", &C::volume },
			{ "landTicks", nullptr, nullptr, &C::landTicks },
		};
		return fields;
	}

	const Field* FindField(const char* key)
	{
		for (const Field& field : Fields())
			if (std::strcmp(field.key, key) == 0) return &field;
		return nullptr;
	}

	const char* EnumName(int kind, int value)
	{
		static const char* layouts[] = { "square", "hex", "scatter" };
		static const char* shapes[] = { "full", "circle", "ring", "islands" };
		static const char* modes[] = { "nudge", "bump", "stack" };
		if (value < 0 || value >= EnumCount(kind)) return "";
		return kind == 1 ? layouts[value] : kind == 2 ? shapes[value] : modes[value];
	}

	int EnumCount(int kind) { return kind == 1 ? 3 : kind == 2 ? 4 : kind == 3 ? 3 : 0; }

	int GetEnum(const DiceConfig& c, int kind)
	{
		return kind == 1 ? static_cast<int>(c.layout) : kind == 2 ? static_cast<int>(c.boardShape) : static_cast<int>(c.collisionMode);
	}

	void SetEnum(DiceConfig& c, int kind, int value)
	{
		if (kind == 1) c.layout = static_cast<Layout>(value);
		else if (kind == 2) c.boardShape = static_cast<BoardShape>(value);
		else c.collisionMode = static_cast<CollisionMode>(value);
	}

	std::string ConfigToJson(const DiceConfig& config)
	{
		nlohmann::ordered_json json;
		for (const Field& field : Fields())
		{
			if (field.kind) json[field.key] = EnumName(field.kind, GetEnum(config, field.kind));
			else if (field.f) json[field.key] = config.*field.f;
			else if (field.i) json[field.key] = config.*field.i;
			else json[field.key] = config.*field.b;
		}
		return json.dump(2);
	}

	bool ConfigFromJson(const std::string& text, DiceConfig& config, std::vector<std::string>& problems)
	{
		const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
		if (json.is_discarded() || !json.is_object()) { problems.push_back("not a JSON object"); return false; }
		DiceConfig result = config;
		for (auto it = json.begin(); it != json.end(); ++it)
		{
			const Field* field = FindField(it.key().c_str());
			if (!field) { problems.push_back("unknown setting '" + it.key() + "'"); continue; }
			const nlohmann::json& value = it.value();
			if (field->kind)
			{
				int found = -1;
				if (value.is_string())
					for (int v = 0; v < EnumCount(field->kind); ++v)
						if (value.get<std::string>() == EnumName(field->kind, v)) found = v;
				if (found < 0) { problems.push_back("'" + it.key() + "': not one of its names"); continue; }
				SetEnum(result, field->kind, found);
			}
			else if (field->b)
			{
				if (!value.is_boolean()) { problems.push_back("'" + it.key() + "': not true / false"); continue; }
				result.*field->b = value.get<bool>();
			}
			else
			{
				if (!value.is_number()) { problems.push_back("'" + it.key() + "': not a number"); continue; }
				if (field->f) result.*field->f = value.get<float>();
				else result.*field->i = static_cast<int>(std::lround(value.get<double>()));
			}
		}
		config = result;
		return true;
	}

	const std::vector<Control>& Controls()
	{
		using K = Control;
		static const std::vector<Control> controls = {
			{ K::Header, nullptr, "Board" },
			{ K::Select, "layout", "Layout", 0, 0, 0, true, { "Square grid", "Hex grid", "Scatter (random, evenly spaced)" } },
			{ K::Select, "boardShape", "Board shape", 0, 0, 0, true, { "Full square", "Circle", "Ring", "Islands" } },
			{ K::Range, "boardSize", "Board size (N x N)", 6, static_cast<float>(kMaxSide), 1, true },
			{ K::Range, "scatterSpacing", "Scatter spacing (cells)", 0.7f, 2, 0.05f, true },
			{ K::Range, "density", "Dice density", 0.05f, 1, 0.05f, true },
			{ K::Header, nullptr, "Hover & roll" },
			{ K::Range, "hoverRadius", "Hover radius (cells)", 0.3f, 6, 0.1f },
			{ K::Range, "rollDistMin", "Roll distance min", 0, 8, 0.1f },
			{ K::Range, "rollDistMax", "Roll distance max", 0, 8, 0.1f },
			{ K::Range, "rollDuration", "Roll duration (s)", 0.15f, 1.5f, 0.05f },
			{ K::Range, "hopHeight", "Hop height", 0, 3, 0.05f },
			{ K::Range, "tumblePerCell", "Tumble (\xC2\xBC turns / cell)", 0, 3, 0.25f },
			{ K::Range, "squash", "Landing squash", 0, 0.6f, 0.02f },
			{ K::Range, "cooldown", "Cooldown (s)", 0, 3, 0.05f },
			{ K::Range, "luck", "Luck (bias to high faces)", 0, 1, 0.05f },
			{ K::Header, nullptr, "Collisions" },
			{ K::Select, "collisionMode", "Collision mode", 0, 0, 0, false, { "Nudge (nearest free slot)", "Bump (push occupant)", "Stack (land on top)" } },
			{ K::Range, "stackMax", "Stack: max height (dice)", 2, 6, 1 },
			{ K::Range, "stackChance", "Stack: chance to stack", 0, 1, 0.05f },
			{ K::Range, "bounceChance", "Bump: bounce-off chance", 0, 1, 0.05f },
			{ K::Range, "bounceDistMax", "Bounce-off max distance", 0.5f, 4, 0.1f },
			{ K::Range, "bounceTime", "Bounce-off time (s)", 0.1f, 0.8f, 0.05f },
			{ K::Range, "bumpChainLimit", "Bump chain limit", 0, 10, 1 },
			{ K::Range, "bumpsPerSecond", "Bump rate limit (/s)", 1, 200, 1 },
			{ K::Range, "bumpNormalWeight", "Bump: push along contact", 0, 1, 0.05f },
			{ K::Range, "bumpSpreadDeg", "Bump spread (\xC2\xB0)", 0, 90, 1 },
			{ K::Range, "bumpHopMult", "Bump: pushed hop height", 0, 1.5f, 0.05f },
			{ K::Range, "bumpDistMax", "Bump: max push distance", 0.5f, 5, 0.1f },
			{ K::Check, "clashEnabled", "Mid-air clash" },
			{ K::Range, "clashDistance", "Clash distance (cells)", 0.1f, 1.5f, 0.05f },
			{ K::Check, "midAirCollide", "Airborne dice glance off each other" },
			{ K::Range, "midAirDeflect", "Glance deflection (cells)", 0, 1.2f, 0.05f },
			{ K::Header, nullptr, "Audio" },
			{ K::Range, "volume", "Volume", 0, 1, 0.05f },
			{ K::Check, "landTicks", "Landing ticks" },
		};
		return controls;
	}

	double SnapToStep(double value, double min, double max, double step)
	{
		if (step > 0) value = min + std::round((value - min) / step) * step;
		return std::clamp(value, min, max);
	}
}
