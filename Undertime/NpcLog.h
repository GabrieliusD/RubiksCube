#pragma once
// The player log (Docs/NPCs.md §1): everything you did with the people, stamped with the
// outside date. It's the only thing about them that changes: friendship, who you've met, when
// you last saw them and story flags are all worked out from it, so turning time back is
// dropping the entries after the day you go back to. Pure C++ (tested in UndertimeTests).
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace npc
{
	enum class EventKind : uint8_t
	{
		Talk,      // a conversation (you were seen); `amount`: the friendship it gave
		Regard,    // friendship gained or lost (dialog choices, gifts); `text`: why
		Flag,      // a story flag set (`text`); global when `npc` is -1
		Unflag,    // ... cleared
		Gift,      // you gave them something (`text`: what); `amount`: the friendship it gave
		Accepted,  // a mission taken on (`text`: its id; `npc`: who gave it)
		Progress,  // ... a step of it done (`text`: "id:what", e.g. "tom_pan:found")
		Completed, // ... finished
		Failed,    // ... too late (logged on the deadline)
	};

	struct Event
	{
		double day = 0.0;          // outside days since you arrived
		int npc = -1;              // index into Cast(), or -1 (global)
		EventKind kind = EventKind::Talk;
		float amount = 0.0f;
		std::string text;
	};

	enum class Tier { Hostile, Wary, Stranger, Acquaintance, Friendly, Friend, Close };
	const char* TierName(Tier tier);

	// What someone thinks of you at a moment.
	struct Standing
	{
		bool met = false;          // you've talked
		double metDay = 0.0;
		double lastSeen = -1e30;   // your last conversation (outside days)
		double lastGift = -1e30;
		float regard = 0.0f;       // -100..100 (after fading, with gossip)
		int talks = 0;
		Tier tier = Tier::Stranger;
		float lostRegard = 0.0f;   // déjà vu: the best friendship of a timeline you undid (0: none)
		// Outside days since you last talked (a large number if never).
		double Away(double days) const { return met ? days - lastSeen : 1e9; }
	};

	class PlayerLog
	{
	public:
		// Events must come in date order (a later one never has an earlier day).
		void Add(const Event& event);
		const std::vector<Event>& Events() const { return mEvents; }
		void Clear();

		// What cast member `npc` thinks of you at `days`: their own events, gossip from the
		// people they're tied to, and fading while you're away.
		Standing StandingOf(int npc, double days) const;
		// A story flag at `days` (global when `npc` is -1).
		bool Flag(const std::string& name, double days, int npc = -1) const;

		// Logging a conversation and a gift: the friendship they give (by warmth and taste; only
		// the first of each a day counts), added to the log. Returns the amount.
		float Talk(int npc, double days);
		float Give(int npc, double days, const std::string& item);

		// Turning time back to `day`: everything after it is forgotten, except by the ageless
		// (the clockmaker). You remember the people you knew who no longer know you (déjà vu).
		void TurnBackTo(double day);

		nlohmann::json ToJson() const;
		bool FromJson(const nlohmann::json& json, std::string& error);

	private:
		std::vector<Event> mEvents;
		std::vector<float> mLost;   // per cast member: best regard of a timeline you undid
	};

	Tier TierOf(float regard, bool met);
}
