#include "Life.h"
#include "Npcs.h"
#include "Clock.h"
#include "Config.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace npc
{
	namespace
	{
		constexpr double kYear = 365.0;

		double HookedDay(const NpcDef& n, double years, bool dies, const PlayerLog* log)
		{
			if (years >= 1e8) return 1e30;
			const double base = years * kYear;
			if (log)
				for (const LifeHook& h : n.hooks)
				{
					const double moved = dies ? h.dies : h.leaves;
					if (moved >= 0.0 && log->Flag(h.flag, base)) return moved >= 1e8 ? 1e30 : moved * kYear;
				}
			return base;
		}

		int EraOfDay(double days) { return OutsideClock::EraAt(days / kYear); }

		const char* Name(int npc) { return npc >= 0 && npc < static_cast<int>(Cast().size()) ? Cast()[npc].name : "someone"; }

		std::string Lower(std::string s)
		{
			if (!s.empty()) s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
			return s;
		}
	}

	double DeathDay(int npc, const PlayerLog* log) { return HookedDay(Cast()[npc], Cast()[npc].dies, true, log); }
	double LeaveDay(int npc, const PlayerLog* log) { return HookedDay(Cast()[npc], Cast()[npc].leaves, false, log); }

	LifeState LifeAt(int npc, double days, const PlayerLog* log)
	{
		LifeState s;
		if (npc < 0 || npc >= static_cast<int>(Cast().size())) return s;
		const NpcDef& n = Cast()[npc];
		const double years = days / kYear;
		s.born = n.ageless || years >= n.born;
		s.diesDay = n.ageless ? 1e30 : DeathDay(npc, log);
		s.leavesDay = n.ageless ? 1e30 : LeaveDay(npc, log);
		s.alive = s.born && days < s.diesDay;
		s.present = s.alive && years >= n.arrives && days < s.leavesDay;
		s.age = n.ageless ? 0.0 : std::max(0.0, years - n.born);
		s.role = n.role;
		for (const RoleChange& r : n.roles)
			if (years >= r.year) s.role = r.role;
		return s;
	}

	std::vector<TownEvent> EventsBetween(double from, double to, const PlayerLog* log)
	{
		std::vector<TownEvent> out;
		auto add = [&](TownEvent e) { if (e.day > from && e.day <= to) out.push_back(std::move(e)); };
		const std::vector<NpcDef>& cast = Cast();
		for (int i = 0; i < static_cast<int>(cast.size()); ++i)
		{
			const NpcDef& n = cast[i];
			if (n.ageless) continue;
			if (!n.parents.empty() && n.born >= 0.0)
			{
				TownEvent e;
				e.kind = TownEventKind::Birth;
				e.id = std::string("birth:") + n.id;
				e.day = n.born * kYear;
				e.npc = i;
				e.other = CastIndex(n.parents[0]);
				add(e);
			}
			else if (n.arrives > 0.0)
			{
				TownEvent e;
				e.kind = TownEventKind::Arrival;
				e.id = std::string("arrival:") + n.id;
				e.day = n.arrives * kYear;
				e.npc = i;
				add(e);
			}
			for (const RoleChange& r : n.roles)
			{
				TownEvent e;
				e.kind = TownEventKind::Role;
				e.id = std::string("role:") + n.id + ":" + std::to_string(static_cast<int>(r.year * 10));
				e.day = r.year * kYear;
				e.npc = i;
				add(e);
			}
			const int spouse = n.spouse ? CastIndex(n.spouse) : -1;
			if (spouse > i && n.marries >= 0.0)
			{
				// The wedding: the afternoon of that day, at the fire in the Camp, in the square later.
				TownEvent e;
				e.kind = TownEventKind::Wedding;
				e.id = std::string("wedding:") + n.id;
				e.day = std::floor(n.marries * kYear) + cfg::kWeddingHours[0] / 24.0;
				e.until = std::floor(n.marries * kYear) + cfg::kWeddingHours[1] / 24.0;
				e.npc = i;
				e.other = spouse;
				e.gather = EraOfDay(e.day) == 1 ? "fire" : "square";
				add(e);
			}
			const double death = DeathDay(i, log);
			if (death < 1e29 && death >= 0.0)
			{
				TownEvent e;
				e.kind = TownEventKind::Death;
				e.id = std::string("death:") + n.id;
				e.day = death;
				e.npc = i;
				add(e);
				TownEvent f;
				f.kind = TownEventKind::Funeral;
				f.id = std::string("funeral:") + n.id;
				f.day = std::floor(death) + cfg::kFuneralDelayDays + cfg::kFuneralHours[0] / 24.0;
				f.until = std::floor(death) + cfg::kFuneralDelayDays + cfg::kFuneralHours[1] / 24.0;
				f.npc = i;
				f.gather = EraOfDay(f.day) >= 2 ? "graves" : "fire";
				add(f);
			}
			const double leave = LeaveDay(i, log);
			if (leave < 1e29 && leave >= 0.0 && leave < death)
			{
				TownEvent e;
				e.kind = TownEventKind::Departure;
				e.id = std::string("departure:") + n.id;
				e.day = leave;
				e.npc = i;
				add(e);
			}
		}
		// Founders' Day: the same day every year, in the town's eras.
		const double offset = std::fmod(static_cast<double>(cfg::kFestivalDayOfYear - cfg::kArrivalDayOfYear) + kYear * 10.0, kYear);
		const long long first = static_cast<long long>(std::floor((std::max(from, 0.0) - offset) / kYear)) - 1;
		for (long long k = std::max(0LL, first); offset + k * kYear <= to; ++k)
		{
			const double day = offset + k * kYear;
			const int era = EraOfDay(day);
			if (era < cfg::kFestivalEras[0] || era > cfg::kFestivalEras[1]) continue;
			TownEvent e;
			e.kind = TownEventKind::Festival;
			OutsideClock c;
			c.SetDays(day);
			e.id = "festival:" + std::to_string(c.Year());
			e.day = day + cfg::kFestivalHours[0] / 24.0;
			e.until = day + cfg::kFestivalHours[1] / 24.0;
			e.gather = "square";
			add(e);
		}
		std::stable_sort(out.begin(), out.end(), [](const TownEvent& a, const TownEvent& b) { return a.day < b.day; });
		return out;
	}

	std::vector<TownEvent> GatheringsOverlapping(double from, double to, const PlayerLog* log)
	{
		std::vector<TownEvent> out;
		for (TownEvent& e : EventsBetween(from - 1.0, to, log))
			if (e.Gathering() && e.until >= from && e.day <= to) out.push_back(std::move(e));
		return out;
	}

	bool Attends(const TownEvent& g, int npc, const PlayerLog* log)
	{
		if (!g.Gathering() || (g.kind == TownEventKind::Funeral && npc == g.npc)) return false;
		if (!LifeAt(npc, g.day, log).present) return false;
		return !DayFor(Cast()[npc], EraOfDay(g.day)).empty();
	}

	std::vector<int> Hosts(const TownEvent& g)
	{
		std::vector<int> hosts;
		if (g.kind == TownEventKind::Wedding) hosts = { g.npc, g.other };
		else if (g.kind == TownEventKind::Funeral && g.npc >= 0)
		{
			const NpcDef& dead = Cast()[g.npc];
			for (const Tie& t : dead.ties)
				if (CastIndex(t.id) >= 0) hosts.push_back(CastIndex(t.id));
			for (int i = 0; i < static_cast<int>(Cast().size()); ++i)
				for (const Tie& t : Cast()[i].ties)
					if (std::string(t.id) == dead.id && std::find(hosts.begin(), hosts.end(), i) == hosts.end()) hosts.push_back(i);
		}
		return hosts;
	}

	std::string NewsLine(const TownEvent& e)
	{
		switch (e.kind)
		{
		case TownEventKind::Arrival: return std::string(Name(e.npc)) + " has come to the valley (" + Lower(Cast()[e.npc].role) + ").";
		case TownEventKind::Birth:
		{
			const NpcDef& child = Cast()[e.npc];
			std::string parents;
			for (size_t p = 0; p < child.parents.size(); ++p)
				parents += (p == 0 ? "" : (p + 1 == child.parents.size() ? " and " : ", ")) + std::string(Name(CastIndex(child.parents[p])));
			return std::string(child.name) + " was born to " + parents + ".";
		}
		case TownEventKind::Wedding: return std::string(Name(e.npc)) + " and " + Name(e.other) + " were married.";
		case TownEventKind::Role:
		{
			LifeState s = LifeAt(e.npc, e.day + 0.01, nullptr);
			return std::string(Name(e.npc)) + " is now the " + Lower(s.role) + ".";
		}
		case TownEventKind::Departure: return std::string(Name(e.npc)) + " has left the valley.";
		case TownEventKind::Death: return std::string(Name(e.npc)) + " has died.";
		default: return {};
		}
	}

	std::vector<std::string> News(double from, double to, const PlayerLog* log, size_t max)
	{
		static const TownEventKind kOrder[] = { TownEventKind::Death, TownEventKind::Wedding, TownEventKind::Birth, TownEventKind::Departure, TownEventKind::Arrival, TownEventKind::Role };
		const std::vector<TownEvent> events = EventsBetween(from, to, log);
		std::vector<std::string> out;
		for (TownEventKind kind : kOrder)
			for (const TownEvent& e : events)
				if (e.kind == kind && out.size() < max) out.push_back(NewsLine(e));
		return out;
	}

	std::vector<TownEvent> SettleGatherings(PlayerLog& log, double days, const std::function<bool(const TownEvent&)>& there)
	{
		std::vector<TownEvent> joined;
		for (const TownEvent& g : GatheringsOverlapping(0.0, days, &log))
		{
			const std::string attended = "attended:" + g.id, missed = "missed:" + g.id;
			if (log.Flag(attended, days) || log.Flag(missed, days)) continue;
			if (days >= g.day && days <= g.until && there(g))
			{
				log.Add({ days, -1, EventKind::Flag, 0.0f, attended });
				if (g.kind == TownEventKind::Festival)
				{
					for (int i = 0; i < static_cast<int>(Cast().size()); ++i)
						if (Attends(g, i, &log)) log.Add({ days, i, EventKind::Regard, cfg::kFestivalRegard, g.id });
				}
				else
					for (int h : Hosts(g)) log.Add({ days, h, EventKind::Regard, cfg::kGatheringRegard, g.id });
				joined.push_back(g);
			}
			else if (days > g.until)
			{
				// Missed: the close friends among its hosts mind.
				log.Add({ g.until, -1, EventKind::Flag, 0.0f, missed });
				for (int h : Hosts(g))
					if (LifeAt(h, g.until, &log).alive && log.StandingOf(h, g.until).tier >= Tier::Friend)
						log.Add({ g.until, h, EventKind::Regard, -cfg::kGatheringMissed, g.id });
			}
		}
		return joined;
	}
}
