// Tests for the people (Npcs.h, NpcTown.h): the cast's data holds together, every place
// exists in Mine.scene, their routes stay on dry land (or a footbridge) and fit their day, and
// where someone is follows from the clock alone: the same date gives the same answer, and
// nobody jumps, in the Camp and the Settlement, through Founders' Day and a funeral.
#include "../Npcs.h"
#include "../NpcTown.h"
#include "../Life.h"
#include "../Config.h"
#include "../DayNight.h"
#include "../Clock.h"
#include <Scene/Scene.h>
#include <Scene/SceneSerializer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace DirectX;

namespace
{
	int gNpcFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gNpcFailures;
	}

	bool ReadFile(const std::string& path, std::string& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		std::stringstream text;
		text << in.rdbuf();
		out = text.str();
		return true;
	}

	// Every day someone has: their first eras' and their later ones, with the eras they're for.
	std::vector<std::pair<int, const std::vector<npc::ScheduleBlock>*>> Days(const npc::NpcDef& n)
	{
		std::vector<std::pair<int, const std::vector<npc::ScheduleBlock>*>> out;
		if (!n.day.empty()) out.push_back({ std::max(1, n.eraFrom), &n.day });
		for (const npc::EraDay& d : n.eraDays) out.push_back({ d.eraFrom, &d.blocks });
		return out;
	}
}

int NpcTests()
{
	using namespace npc;
	const std::vector<NpcDef>& cast = Cast();

	// ---- The data --------------------------------------------------------------------------
	{
		bool placesKnown = true, ordered = true, ties = true, lives = true;
		for (const NpcDef& n : cast)
		{
			for (const auto& [era, day] : Days(n))
				for (size_t i = 0; i < day->size(); ++i)
				{
					const ScheduleBlock& b = (*day)[i];
					if (!FindPlace(b.place) || (b.rainPlace && !FindPlace(b.rainPlace)))
					{
						std::printf("        %s: unknown place %s\n", n.id, FindPlace(b.place) ? b.rainPlace : b.place);
						placesKnown = false;
					}
					if (b.activity == Activity::Walk || b.rainActivity == Activity::Walk) placesKnown = false;
					if (i == 0 ? b.hour != 0.0f : b.hour <= (*day)[i - 1].hour || b.hour >= 24.0f) ordered = false;
				}
			for (const Tie& tie : n.ties)
				if (CastIndex(tie.id) < 0) ties = false;
			if (n.spouse && (CastIndex(n.spouse) < 0 || Cast()[CastIndex(n.spouse)].marries != n.marries)) lives = false;
			for (const char* parent : n.parents) lives &= CastIndex(parent) >= 0 && Cast()[CastIndex(parent)].born < n.born;
			lives &= n.born <= n.arrives || n.ageless;
		}
		Check(placesKnown, "people: every place in a schedule exists (and nobody is scheduled to 'walk')");
		Check(ordered, "people: each day starts at 0:00 and its blocks are in hour order");
		Check(ties, "people: every tie names someone in the cast");
		Check(lives, "people: lives hold together (both spouses agree, parents are older, nobody arrives before they're born)");
		bool unique = true;
		for (size_t i = 0; i < Places().size(); ++i)
			for (size_t j = i + 1; j < Places().size(); ++j) unique &= std::string(Places()[i].id) != Places()[j].id;
		Check(unique, "people: place names are unique");
	}

	// ---- The town, with the places on Mine.scene's props ----------------------------------------
	Scene scene;
	std::string text, error;
	const bool loaded = ReadFile(std::string(UNDERTIME_CONTENT_DIR) + "/Scenes/Mine.scene", text) && cherry::SceneFromJson(scene, text, error);
	Check(loaded, "people: Mine.scene loads");
	Valley valley;
	Town town;
	auto lookup = [&](const char* entity, XMFLOAT4X4& world)
	{
		bool found = false;
		scene.Each<EntityInfo>([&](Entity e, EntityInfo& info)
		{
			if (found || info.name != entity || !scene.Has<Transform>(e)) return;
			XMStoreFloat4x4(&world, scene.WorldMatrix(e));
			found = true;
		});
		return found;
	};
	town.Build(valley, lookup);
	{
		bool resolved = true;
		for (size_t i = 0; i < town.PlaceCount(); ++i)
			if (!town.PlaceAt(i).resolved)
			{
				std::printf("        place %s doesn't resolve\n", town.PlaceName(static_cast<int>(i)).c_str());
				resolved = false;
			}
		Check(resolved, "people: every place is on a prop in Mine.scene (by name), on a dry river bank, or round a gathering");
		bool banks = true;
		for (size_t i = 0; i < Places().size(); ++i)
		{
			if (!Places()[i].riverBank || !town.PlaceAt(i).resolved) continue;
			const Place& p = town.PlaceAt(i);
			const float d = valley.RiverDistance(p.at.x, p.at.z);
			banks &= d > cfg::kRiverHalfWidth * 0.5f && d < cfg::kRiverHalfWidth + 4.0f && valley.WaterLevel(p.at.x, p.at.z) <= town.GroundY(p.at.x, p.at.z);
		}
		Check(banks, "people: river bank places are on dry ground right by the water");
	}

	// ---- Routes -----------------------------------------------------------------------------------
	{
		bool dry = true, sensible = true, fits = true;
		float longest = 0.0f;
		auto checkRoute = [&](const char* who, const char* from, const char* to, double hours, bool timed)
		{
			const int a = town.PlaceIndex(from), b = town.PlaceIndex(to);
			if (a < 0 || b < 0 || a == b || !town.PlaceAt(a).resolved || !town.PlaceAt(b).resolved) return;
			const Town::Route& route = town.RouteBetween(a, b);
			for (size_t k = 1; k < route.points.size(); ++k)
			{
				const RoutePoint& p = route.points[k - 1];
				const RoutePoint& q = route.points[k];
				if ((p.ground || q.ground) && !town.Dry(p.x, p.z, q.x, q.z))
				{
					std::printf("        %s: %s -> %s crosses water\n", who, from, to);
					dry = false;
				}
			}
			const float straight = std::hypot(town.PlaceAt(b).at.x - town.PlaceAt(a).at.x, town.PlaceAt(b).at.z - town.PlaceAt(a).at.z);
			if (route.length > straight * 2.5f + 20.0f)
			{
				std::printf("        %s: %s -> %s is %.0f m for %.0f m as the crow flies\n", who, from, to, route.length, straight);
				sensible = false;
			}
			const double walk = Town::WalkDays(route.length) * 24.0;
			if (timed && walk > hours)
			{
				std::printf("        %s: %s -> %s takes %.1f h, the block can be %.1f h\n", who, from, to, walk, hours);
				fits = false;
			}
			longest = std::max(longest, route.length);
		};
		for (const NpcDef& n : cast)
			for (const auto& [era, dayPtr] : Days(n))
			{
				const std::vector<ScheduleBlock>& day = *dayPtr;
				for (size_t i = 0; i < day.size(); ++i)
				{
					const ScheduleBlock& prev = day[(i + day.size() - 1) % day.size()];
					const ScheduleBlock& cur = day[i];
					const float length = (i + 1 < day.size() ? day[i + 1].hour : 24.0f) - cur.hour;
					const int next = static_cast<int>(i + 1 < day.size() ? i + 1 : 0);
					// The walk must end before the next block begins, even on a day this one
					// starts as late and the next as early as they can.
					const float shortest = length - Town::JitterLimit(day, static_cast<int>(i)) - Town::JitterLimit(day, next);
					for (const char* from : { prev.place, prev.rainPlace })
						for (const char* to : { cur.place, cur.rainPlace })
							if (from && to) checkRoute(n.id, from, to, shortest, true);
				}
				// To and from the gatherings of that era (no timing: they set off early).
				const char* gather = era == 1 ? "gather.fire#0" : "gather.square#0";
				for (const ScheduleBlock& b : day)
				{
					checkRoute(n.id, b.place, gather, 0.0, false);
					if (era >= 2) checkRoute(n.id, b.place, "gather.graves#0", 0.0, false);
				}
			}
		std::printf("        longest walk %.0f m (%.1f outside hours)\n", longest, Town::WalkDays(longest) * 24.0);
		Check(dry, "people: no route goes through the river or the lake (the footbridge is a bridge)");
		Check(sensible, "people: routes aren't wild detours (under 2.5x the straight distance + 20 m)");
		Check(fits, "people: every walk is over before the next block of their day begins");
	}

	// ---- Where they are, from the clock alone -------------------------------------------------------
	{
		const double minute = 1.0 / 1440.0;
		const float step = cfg::kNpcWalkSpeed * 60.0f / cfg::kSurfaceRate * 1.25f + 0.02f;
		bool smooth = true, walked = false, gathered = false;
		// Founders' Day in year 5, and Bram's funeral (he dies at 8.5 years without the remedy).
		const double festival = std::fmod(cfg::kFestivalDayOfYear - cfg::kArrivalDayOfYear + 365.0 * 10.0, 365.0) + 5.0 * 365.0;
		const double funeral = std::floor(Cast()[CastIndex("bram")].dies * 365.0) + cfg::kFuneralDelayDays;
		const struct { double from, to; int era; } spans[] = { { 0.0, 7.0, 1 }, { 5.0 * 365.0, 5.0 * 365.0 + 3.0, 2 }, { festival - 0.5, festival + 1.5, 2 }, { funeral - 0.5, funeral + 1.5, 2 } };
		for (const auto& span : spans)
			for (size_t n = 0; n < cast.size(); ++n)
			{
				Pose last = town.Evaluate(n, span.from, span.era);
				for (double t = span.from + minute; t < span.to; t += minute)
				{
					const Pose pose = town.Evaluate(n, t, span.era);
					const float jump = std::hypot(pose.position.x - last.position.x, pose.position.z - last.position.z);
					if (pose.present && last.present && jump > step && smooth)
					{
						std::printf("        %s jumps %.2f m at day %.4f (%s -> %s)\n", cast[n].id, jump, t, ActivityName(last.activity), ActivityName(pose.activity));
						smooth = false;
					}
					walked |= pose.activity == Activity::Walk;
					gathered |= pose.gathering && pose.activity != Activity::Walk;
					last = pose;
				}
			}
		Check(smooth, "people: nobody jumps; minute to minute they move at most a minute's walk (Camp, Settlement, Founders' Day, a funeral)");
		Check(walked && gathered, "people: they walk between places, and gather at Founders' Day and funerals");

		bool slept = true;
		for (size_t n = 0; n < cast.size(); ++n)
			for (const auto& [day, era] : { std::make_pair(3.0, 1), std::make_pair(5.0 * 365.0 + 1.0, 2) })
			{
				const Pose night = town.Evaluate(n, day + 3.0 / 24.0, era);   // 3 am
				if (night.present && night.activity != Activity::Sleep) { std::printf("        %s is up at 3 am (%s)\n", cast[n].id, ActivityName(night.activity)); slept = false; }
			}
		Check(slept, "people: everyone living there is asleep at 3 am");

		bool same = true;
		Town again;
		again.Build(valley, lookup);
		for (double t : { 0.3, 12.71, 400.5, 3650.25, 1.2e5 })
			for (size_t n = 0; n < cast.size(); ++n)
			{
				const int era = OutsideClock::EraAt(t / 365.0) >= 2 ? 2 : 1;
				const Pose a = town.Evaluate(n, t, era), b = again.Evaluate(n, t, era);
				town.Evaluate(n, t + 33.3, era);
				const Pose c = town.Evaluate(n, t, era);
				same &= a.present == b.present && a.place == b.place && a.position.x == b.position.x && a.position.z == b.position.z
					&& a.position.x == c.position.x && a.position.z == c.position.z && a.activity == c.activity;
			}
		Check(same, "people: the same date always gives the same answer (a dive of years, or turning time back, just asks again)");

		bool apart = true;
		for (const auto& span : spans)
			for (double t = span.from; t < span.to && apart; t += 10.0 / 1440.0)
				for (size_t a = 0; a < cast.size(); ++a)
					for (size_t b = a + 1; b < cast.size(); ++b)
					{
						const Pose pa = town.Evaluate(a, t, span.era), pb = town.Evaluate(b, t, span.era);
						if (pa.present && pb.present && pa.activity != Activity::Walk && pb.activity != Activity::Walk && pa.place == pb.place)
						{
							std::printf("        %s and %s both at %s on day %.3f\n", cast[a].id, cast[b].id, town.PlaceName(pa.place).c_str(), t);
							apart = false;
						}
					}
		Check(apart, "people: two people never stand in the same spot");

		// Rain sends people to their rain places.
		bool rained = false, sheltered = true;
		for (double t = 0.0; t < 400.0 && !rained; t += 1.0 / 24.0)
		{
			if (WeatherAt(t).rain <= cfg::kNpcRainThreshold) continue;
			for (size_t n = 0; n < cast.size(); ++n)
			{
				Town::Block current, previous;
				if (!town.BlocksAt(n, t, 1, nullptr, current, previous) || current.index < 0) continue;
				const ScheduleBlock& def = DayFor(cast[n], 1)[current.index];
				if (!def.rainPlace || WeatherAt(current.start).rain <= cfg::kNpcRainThreshold) continue;
				rained = true;
				const Pose pose = town.Evaluate(n, t, 1);
				sheltered &= !pose.present || (pose.rain && pose.place == town.PlaceIndex(def.rainPlace));
			}
		}
		Check(rained && sheltered, "people: when it rains as a block starts, they go to its rain place");

		// Who lives there: their life (arrived, alive) and a day in that era's town.
		bool eras = true;
		for (size_t n = 0; n < cast.size(); ++n)
			for (int era = 1; era <= 6; ++era)
				for (double t : { 100.3, 5.0 * 365.0 + 0.4 })
					eras &= town.Evaluate(n, t, era).present == (!DayFor(cast[n], era).empty() && LifeAt(static_cast<int>(n), t, nullptr).present);
		const auto clockmaker = std::find_if(cast.begin(), cast.end(), [](const NpcDef& n) { return n.ageless; });
		Check(eras && clockmaker != cast.end() && !DayFor(*clockmaker, 6).empty(), "people: each lives in the valley while alive, arrived and with a day in that era's town; the clockmaker always");
	}
	return gNpcFailures;
}
