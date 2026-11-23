#pragma once
// Lives across the eras (Docs/NPCs.md §2.1 and §7): who is born, arrives, marries, takes on a
// new role, leaves or dies, and when; the town's gatherings (weddings, funerals, Founders' Day)
// and the news when you come up. All of it from the cast's written lives (Npcs.cpp) and the
// player log (story flags that change a life: a remedy in time), so turning time back replays
// it. Pure C++ (tested in UndertimeTests).
#include "NpcLog.h"
#include <string>
#include <vector>

namespace npc
{
	struct LifeState
	{
		bool born = false;          // (the ageless always are)
		bool alive = false;
		bool present = false;       // alive, arrived and not left: living in the valley
		double age = 0.0;           // years
		double diesDay = 1e30;      // outside days (with story hooks), 1e30: not within the story
		double leavesDay = 1e30;
		const char* role = "";
	};
	// `log` may be null (no story flags).
	LifeState LifeAt(int npc, double days, const PlayerLog* log);
	double DeathDay(int npc, const PlayerLog* log);
	double LeaveDay(int npc, const PlayerLog* log);

	enum class TownEventKind { Arrival, Birth, Wedding, Role, Departure, Death, Funeral, Festival };

	struct TownEvent
	{
		TownEventKind kind = TownEventKind::Arrival;
		std::string id;             // "funeral:bram", "festival:1874", ...
		double day = 0.0;           // when it happens (outside days); a gathering: when it starts
		double until = 0.0;         // a gathering: when it ends
		int npc = -1, other = -1;   // who it's about (a wedding: both; a birth: the child, `other` a parent)
		std::string gather;         // a gathering's place: "fire", "square" or "graves"
		bool Gathering() const { return kind == TownEventKind::Wedding || kind == TownEventKind::Funeral || kind == TownEventKind::Festival; }
	};

	// Everything that happens after `from` up to `to` (outside days), in date order.
	std::vector<TownEvent> EventsBetween(double from, double to, const PlayerLog* log);
	// Gatherings whose window overlaps [from, to].
	std::vector<TownEvent> GatheringsOverlapping(double from, double to, const PlayerLog* log);
	// Whether cast member `npc` goes to a gathering (everyone living there, not the dead).
	bool Attends(const TownEvent& gathering, int npc, const PlayerLog* log);
	// The people a gathering means most to (the couple, the dead one's family and friends).
	std::vector<int> Hosts(const TownEvent& gathering);
	// One line of news: "Agnes Rook and Bram Kettle were married."
	std::string NewsLine(const TownEvent& event);
	// The news between two dates (coming up from a dive), most important first, at most `max`.
	std::vector<std::string> News(double from, double to, const PlayerLog* log, size_t max);

	// Being at gatherings: `there(gathering)` says whether you're there now. Logs who you
	// were there for (friendship) and, for those that ended without you, a loss with the close
	// friends hosting them. Returns the gatherings you just joined.
	std::vector<TownEvent> SettleGatherings(PlayerLog& log, double days, const std::function<bool(const TownEvent&)>& there);
}
