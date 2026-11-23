#include "NpcLog.h"
#include "Npcs.h"
#include "Config.h"
#include <algorithm>
#include <cmath>

namespace npc
{
	namespace
	{
		const char* const kKindNames[] = { "talk", "regard", "flag", "unflag", "gift", "accepted", "progress", "completed", "failed" };

		bool Tied(int a, int b)
		{
			const std::vector<NpcDef>& cast = Cast();
			for (const Tie& tie : cast[a].ties)
				if (std::string(tie.id) == cast[b].id) return true;
			for (const Tie& tie : cast[b].ties)
				if (std::string(tie.id) == cast[a].id) return true;
			return false;
		}

		// Friendship fading towards nothing between `from` and `to`, counting only the time
		// after the grace period that follows your last visit.
		float Fade(float regard, double from, double to, double lastSeen, float patience)
		{
			const double start = std::max(from, lastSeen + cfg::kRegardGraceDays);
			if (to <= start || regard == 0.0f) return regard;
			const double halfLife = cfg::kRegardHalfLifeDays * (0.5 + patience) * (1.0 + std::max(regard, 0.0f) / 50.0);
			return static_cast<float>(regard * std::pow(0.5, (to - start) / halfLife));
		}
	}

	const char* TierName(Tier tier)
	{
		static const char* const kNames[] = { "Hostile", "Wary", "Stranger", "Acquaintance", "Friendly", "Friend", "Close" };
		return kNames[static_cast<int>(tier)];
	}

	Tier TierOf(float regard, bool met)
	{
		if (regard < cfg::kTierHostile) return Tier::Hostile;
		if (regard < cfg::kTierWary) return Tier::Wary;
		if (!met) return Tier::Stranger;
		if (regard < cfg::kTierFriendly) return Tier::Acquaintance;
		if (regard < cfg::kTierFriend) return Tier::Friendly;
		if (regard < cfg::kTierClose) return Tier::Friend;
		return Tier::Close;
	}

	void PlayerLog::Add(const Event& event)
	{
		// Kept in date order (after turning time back, the clockmaker's memories sit at the day you went back to).
		const auto at = std::upper_bound(mEvents.begin(), mEvents.end(), event.day, [](double d, const Event& e) { return d < e.day; });
		mEvents.insert(at, event);
	}

	void PlayerLog::Clear()
	{
		mEvents.clear();
		mLost.clear();
	}

	Standing PlayerLog::StandingOf(int npc, double days) const
	{
		Standing s;
		const std::vector<NpcDef>& cast = Cast();
		if (npc < 0 || npc >= static_cast<int>(cast.size())) return s;
		const float patience = cast[npc].traits.patience;
		if (npc < static_cast<int>(mLost.size())) s.lostRegard = mLost[npc];

		// Their own events, and gossip: a share of big changes with the people they're tied to,
		// heard a couple of days later.
		struct Change { double day; float amount; bool seen; bool gift; };
		std::vector<Change> changes;
		for (const Event& e : mEvents)
		{
			if (e.npc < 0 || e.npc >= static_cast<int>(cast.size())) continue;
			if (e.npc == npc)
			{
				if (e.day > days) continue;
				if (e.kind == EventKind::Talk) changes.push_back({ e.day, e.amount, true, false });
				else if (e.kind == EventKind::Regard) changes.push_back({ e.day, e.amount, false, false });
				else if (e.kind == EventKind::Gift) changes.push_back({ e.day, e.amount, false, true });
			}
			else if ((e.kind == EventKind::Regard || e.kind == EventKind::Gift) && std::fabs(e.amount) >= cfg::kGossipMin
				&& e.day + cfg::kGossipDays <= days && Tied(npc, e.npc))
			{
				changes.push_back({ e.day + cfg::kGossipDays, e.amount * cfg::kGossipShare, false, false });
			}
		}
		// A child starts with a share of their parents' friendship when they're born ("my
		// grandmother spoke of a courier who never grew old").
		const NpcDef& self = cast[npc];
		if (!self.parents.empty() && self.born >= 0.0 && self.born * 365.0 <= days)
		{
			float best = 0.0f;
			for (const char* parent : self.parents)
			{
				const int p = CastIndex(parent);
				if (p >= 0 && p != npc) best = std::max(best, StandingOf(p, self.born * 365.0).regard);
			}
			if (best > 0.0f) changes.push_back({ self.born * 365.0, best * cfg::kInheritShare, false, false });
		}
		std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.day < b.day; });

		double t = changes.empty() ? days : changes.front().day;
		for (const Change& c : changes)
		{
			s.regard = Fade(s.regard, t, c.day, s.lastSeen, patience);
			t = c.day;
			s.regard = std::clamp(s.regard + c.amount, -100.0f, 100.0f);
			if (c.seen)
			{
				if (!s.met) s.metDay = c.day;
				s.met = true;
				s.lastSeen = c.day;
				++s.talks;
			}
			if (c.gift) s.lastGift = c.day;
		}
		s.regard = Fade(s.regard, t, days, s.lastSeen, patience);
		s.tier = TierOf(s.regard, s.met);
		return s;
	}

	bool PlayerLog::Flag(const std::string& name, double days, int npc) const
	{
		bool set = false;
		for (const Event& e : mEvents)
		{
			if (e.day > days) break;
			if (e.npc == npc && e.text == name && (e.kind == EventKind::Flag || e.kind == EventKind::Unflag)) set = e.kind == EventKind::Flag;
		}
		return set;
	}

	float PlayerLog::Talk(int npc, double days)
	{
		const Standing s = StandingOf(npc, days);
		const bool sameDay = s.met && std::floor(s.lastSeen) == std::floor(days);
		const float amount = sameDay ? 0.0f : cfg::kRegardTalk * (0.5f + Cast()[npc].traits.warmth);
		Add({ days, npc, EventKind::Talk, amount, "" });
		return amount;
	}

	float PlayerLog::Give(int npc, double days, const std::string& item)
	{
		const NpcDef& who = Cast()[npc];
		const Standing s = StandingOf(npc, days);
		float amount = 0.0f;
		if (std::floor(s.lastGift) != std::floor(days))
		{
			const auto has = [&](const std::vector<const char*>& list) { return std::any_of(list.begin(), list.end(), [&](const char* x) { return item == x; }); };
			amount = has(who.likes) ? cfg::kRegardGiftLiked : has(who.dislikes) ? cfg::kRegardGiftDisliked : cfg::kRegardGiftNeutral;
			if (amount > 0.0f) amount *= 0.5f + who.traits.warmth;
		}
		Add({ days, npc, EventKind::Gift, amount, item });
		return amount;
	}

	void PlayerLog::TurnBackTo(double day)
	{
		const std::vector<NpcDef>& cast = Cast();
		mLost.resize(cast.size(), 0.0f);
		const double latest = mEvents.empty() ? day : std::max(day, mEvents.back().day);
		for (size_t n = 0; n < cast.size(); ++n)
		{
			if (cast[n].ageless) continue;
			const Standing s = StandingOf(static_cast<int>(n), latest);
			if (s.met && s.metDay > day) mLost[n] = std::max(mLost[n], s.regard);
		}
		// The ageless keep what happened: their memories move to the day you went back to.
		std::vector<Event> kept;
		for (Event e : mEvents)
		{
			const bool ageless = e.npc >= 0 && e.npc < static_cast<int>(cast.size()) && cast[e.npc].ageless;
			if (e.day <= day) kept.push_back(e);
			else if (ageless)
			{
				e.day = day;
				kept.push_back(e);
			}
		}
		mEvents = std::move(kept);
	}

	nlohmann::json PlayerLog::ToJson() const
	{
		nlohmann::json events = nlohmann::json::array();
		const std::vector<NpcDef>& cast = Cast();
		for (const Event& e : mEvents)
		{
			nlohmann::json j;
			j["day"] = e.day;
			j["npc"] = e.npc >= 0 && e.npc < static_cast<int>(cast.size()) ? cast[e.npc].id : "";
			j["kind"] = kKindNames[static_cast<int>(e.kind)];
			if (e.amount != 0.0f) j["amount"] = e.amount;
			if (!e.text.empty()) j["text"] = e.text;
			events.push_back(j);
		}
		nlohmann::json lost = nlohmann::json::object();
		for (size_t n = 0; n < mLost.size() && n < cast.size(); ++n)
			if (mLost[n] != 0.0f) lost[cast[n].id] = mLost[n];
		return { { "events", events }, { "lost", lost } };
	}

	bool PlayerLog::FromJson(const nlohmann::json& json, std::string& error)
	{
		Clear();
		if (!json.is_object() || !json.contains("events") || !json["events"].is_array())
		{
			error = "the log has no events";
			return false;
		}
		for (const nlohmann::json& j : json["events"])
		{
			Event e;
			e.day = j.value("day", 0.0);
			const std::string id = j.value("npc", std::string());
			e.npc = id.empty() ? -1 : CastIndex(id);
			if (!id.empty() && e.npc < 0) continue;   // someone no longer in the cast
			const std::string kind = j.value("kind", std::string());
			const auto found = std::find_if(std::begin(kKindNames), std::end(kKindNames), [&](const char* k) { return kind == k; });
			if (found == std::end(kKindNames))
			{
				error = "unknown event kind '" + kind + "'";
				return false;
			}
			e.kind = static_cast<EventKind>(found - std::begin(kKindNames));
			e.amount = j.value("amount", 0.0f);
			e.text = j.value("text", std::string());
			Add(e);
		}
		mLost.assign(Cast().size(), 0.0f);
		if (json.contains("lost") && json["lost"].is_object())
		{
			for (auto it = json["lost"].begin(); it != json["lost"].end(); ++it)
			{
				const int n = CastIndex(it.key());
				if (n >= 0 && it.value().is_number()) mLost[n] = it.value().get<float>();
			}
		}
		return true;
	}
}
