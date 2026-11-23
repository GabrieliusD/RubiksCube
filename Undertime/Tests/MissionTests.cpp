// Tests for missions (Missions.h): every file loads, offers come through conversations, each
// kind of objective can be finished, deadlines fail them, the board is the same each week,
// and turning time back undoes them.
#include "../Missions.h"
#include "../Npcs.h"
#include "../Config.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
	int gMissionFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gMissionFailures;
	}

	int Find(const std::vector<std::string>& options, const std::string& start)
	{
		for (size_t i = 0; i < options.size(); ++i)
			if (options[i].rfind(start, 0) == 0) return static_cast<int>(i);
		return -1;
	}
}

int MissionTests()
{
	using namespace npc;
	MissionBook book;
	book.LoadFolder(std::string(UNDERTIME_CONTENT_DIR) + "/Missions");
	for (const std::string& e : book.Errors()) std::printf("        %s\n", e.c_str());
	Check(book.Errors().empty() && book.Count() >= 5 && book.JobTemplates() >= 2, "missions: every file in Content/Missions loads without problems");
	{
		MissionBook bad;
		bad.LoadText("bad.mission", R"({ "missions": [ { "id": "x", "giver": "nobody", "title": "X", "offer": "?", "objective": { "type": "deliver", "item": "Cheese", "count": 1 } } ] })");
		bad.LoadText("bad2.mission", R"({ "missions": [ { "id": "y", "giver": "tom", "title": "Y", "offer": "?", "objective": { "type": "teleport" } } ] })");
		Check(bad.Errors().size() >= 3 && bad.Count() == 0, "missions: unknown people, items and objective types are reported");
	}

	DialogLibrary library;
	library.LoadFolder(std::string(UNDERTIME_CONTENT_DIR) + "/Dialog");
	std::map<std::string, int> bag;
	PlayerLog log;
	Conversation talk;
	Conversation::Hooks hooks;
	hooks.topics = [&](const DialogContext& c, std::deque<Topic>& out) { book.Topics(c, out); };
	hooks.count = [&](const std::string& item) { return bag[item]; };
	hooks.effect = [&](const Effect& e)
	{
		// What the game does (UndertimeModule::MissionEffect).
		if (e.kind == Effect::Kind::Accept) book.Accept(log, e.name, 0.0);
		if (e.kind == Effect::Kind::Take) bag[e.name] -= static_cast<int>(e.amount);
	};
	talk.SetHooks(hooks);
	const int agnes = CastIndex("agnes");
	Conversation::Situation at;

	// ---- Deliver, through a conversation ------------------------------------------------------------
	{
		at.days = 2.3;
		// Accept and Complete come through the hooks with the conversation's date.
		hooks.effect = [&](const Effect& e)
		{
			if (e.kind == Effect::Kind::Accept) book.Accept(log, e.name, at.days);
			if (e.kind == Effect::Kind::Complete) for (const auto& [item, n] : book.Complete(log, e.name, at.days)) bag[item] += n;
			if (e.kind == Effect::Kind::Take) bag[e.name] -= static_cast<int>(e.amount);
		};
		talk.SetHooks(hooks);
		talk.Start(library, log, agnes, at);
		talk.Continue();
		std::vector<std::string> options = talk.Options(false);
		const int offer = Find(options, "Need a hand?");
		Check(offer == 0, "missions: someone with work offers it first among the topics");
		talk.Choose(offer, false);
		talk.Continue();
		talk.Choose(0, false);   // I'll do it.
		const MissionStatus s = book.Status(log, "agnes_roof", at.days);
		Check(s.state == MissionState::Active && std::fabs(s.due - (2.3 + 10.0)) < 1e-6, "missions: accepting starts it, with its deadline on the outside clock");
		talk.Continue();
		options = talk.Options(false);
		Check(Find(options, "Need a hand?") < 0 && Find(options, "About Copper for the roof") >= 0, "missions: then it's no longer offered; you can ask about it");

		bag["Copper"] = 20;
		at.days = 5.0;
		Conversation again;
		again.SetHooks(hooks);
		again.Start(library, log, agnes, at);
		again.Continue();
		options = again.Options(false);
		const int hand = Find(options, "Hand over 15 Copper");
		Check(hand >= 0, "missions: with enough in the backpack you can hand it over");
		const float before = log.StandingOf(agnes, at.days).regard;
		again.Choose(hand, false);
		Check(book.Status(log, "agnes_roof", at.days).state == MissionState::Done && bag["Copper"] == 5 && bag["Silver"] == 3
			&& log.StandingOf(agnes, at.days).regard > before + 9.0f, "missions: handing over takes the items, gives the reward and the friendship");
	}

	// ---- Deadlines ---------------------------------------------------------------------------------
	{
		PlayerLog late;
		book.Accept(late, "tom_pan", 10.0);
		Check(book.Expire(late, 23.9).empty(), "missions: nothing fails before its deadline");
		const float before = late.StandingOf(CastIndex("tom"), 23.0).regard;
		const std::vector<std::string> failed = book.Expire(late, 30.0);
		const MissionStatus s = book.Status(late, "tom_pan", 30.0);
		Check(failed.size() == 1 && s.state == MissionState::Failed && std::fabs(s.ended - 24.0) < 1e-6 && late.StandingOf(CastIndex("tom"), 24.0).regard < before,
			"missions: past the deadline it fails (dated on the deadline) and costs friendship");
		std::printf("        14 days: %s\n", DeadlineText(14.0).c_str());
		Check(DeadlineText(14.0).find("zone 3") != std::string::npos && DeadlineText(14.0).find("zone 5") == std::string::npos && DeadlineText(-1.0) == "too late", "missions: deadlines in real time at each depth");
	}

	// ---- Retrieve, courier, meet, wait ---------------------------------------------------------------
	{
		PlayerLog l;
		book.Accept(l, "tom_pan", 1.0);
		book.Found(l, "tom_pan", 2.0);
		Check(book.Status(l, "tom_pan", 2.0).found && !book.Status(l, "tom_pan", 1.5).found, "missions: retrieve: finding the thing is logged");

		book.Accept(l, "mae_list", 1.0);
		DialogContext atAgnes;
		atAgnes.npc = agnes;
		atAgnes.days = 2.0;
		atAgnes.log = &l;
		std::deque<Topic> topics;
		book.Topics(atAgnes, topics);
		bool give = false;
		for (const Topic& t : topics) give |= t.title.rfind("Give them Mae's list", 0) == 0;
		Check(give, "missions: courier: the one it's for can be given it");

		book.Accept(l, "mae_supper", 3.4);   // the evening after next: day 5, 18:00 to 21:00
		auto there = [](const std::string& place) { return place == "fire.e"; };
		auto away = [](const std::string&) { return false; };
		Check(book.Arrived(l, 5.0 + 19.0 / 24.0, away).empty() && book.Arrived(l, 4.0 + 19.0 / 24.0, there).empty()
			&& book.Arrived(l, 5.0 + 19.0 / 24.0, there).size() == 1, "missions: meet: only at the place, in its window");
		Check(book.Expire(l, 5.0 + 22.0 / 24.0).size() == 1, "missions: meet: missing the window fails it");

		book.Accept(l, "clock_year", 10.0);
		DialogContext atClock;
		atClock.npc = CastIndex("clockmaker");
		atClock.log = &l;
		auto ready = [&](double days)
		{
			atClock.days = days;
			std::deque<Topic> t;
			book.Topics(atClock, t);
			for (const Topic& x : t) if (x.title == "I'm back, as you asked") return true;
			return false;
		};
		Check(!ready(300.0) && ready(376.0), "missions: wait: done only once that much outside time has passed");
	}

	// ---- The notice board ----------------------------------------------------------------------------
	{
		const std::vector<std::string> a = book.BoardJobs(12, 1), b = book.BoardJobs(12, 1), c = book.BoardJobs(13, 1);
		Check(!a.empty() && a == b && a.size() <= static_cast<size_t>(cfg::kBoardJobsPerWeek), "board: the same week always shows the same notices");
		bool posted = true;
		for (const std::string& id : a)
		{
			const MissionDef* m = book.Find(id);
			posted &= m && m->job && CastIndex(m->giver) >= 0 && (m->type != ObjectiveType::Deliver || m->count >= 1);
		}
		Check(posted, "board: every notice is posted by someone in the cast");
		bool differs = false;
		for (long long week = 13; week < 40 && !differs; ++week) differs = book.BoardJobs(week, 1) != a;
		Check(differs && book.BoardJobs(12, 4).empty(), "board: notices change from week to week, and only people living there post them");
	}

	// ---- Turning time back ---------------------------------------------------------------------------
	{
		PlayerLog l;
		book.Accept(l, "agnes_roof", 100.0);
		l.TurnBackTo(50.0);
		Check(book.Status(l, "agnes_roof", 50.0).state == MissionState::Open, "missions: turning time back to before you accepted undoes it");
	}
	return gMissionFailures;
}
