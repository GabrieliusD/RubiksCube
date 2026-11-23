// Tests for talking to people: friendship from the player log (once a day, fading while you're
// away, gossip, gifts, turning time back), the dialog files and conversations, and the save game.
#include "../Npcs.h"
#include "../NpcLog.h"
#include "../Dialog.h"
#include "../SaveGame.h"
#include "../Config.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
	int gDialogFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gDialogFailures;
	}

	int Index(const char* id) { return npc::CastIndex(id); }

	int Find(const std::vector<std::string>& options, const std::string& text)
	{
		for (size_t i = 0; i < options.size(); ++i)
			if (options[i] == text) return static_cast<int>(i);
		return -1;
	}
}

int DialogTests()
{
	using namespace npc;
	const int agnes = Index("agnes"), tom = Index("tom"), mae = Index("mae"), bram = Index("bram"), clockmaker = Index("clockmaker");
	Check(agnes >= 0 && tom >= 0 && mae >= 0 && bram >= 0 && clockmaker >= 0, "talk: the first cast is all there");

	// ---- Friendship --------------------------------------------------------------------------------
	{
		PlayerLog log;
		Check(log.StandingOf(agnes, 0.0).tier == Tier::Stranger, "friendship: someone you've never talked to is a Stranger");
		const float first = log.Talk(agnes, 1.2);
		const float again = log.Talk(agnes, 1.6);
		const float nextDay = log.Talk(agnes, 2.1);
		Check(first > 0.0f && again == 0.0f && nextDay > 0.0f, "friendship: only the first conversation of an outside day counts");
		const Standing met = log.StandingOf(agnes, 2.1);
		Check(met.met && met.talks == 3 && met.tier == Tier::Acquaintance, "friendship: after talking you're an Acquaintance");

		log.Add({ 2.1, agnes, EventKind::Regard, 30.0f, "test" });
		const float now = log.StandingOf(agnes, 2.1).regard;
		const float inGrace = log.StandingOf(agnes, 2.1 + cfg::kRegardGraceDays - 0.1).regard;
		const double half = cfg::kRegardHalfLifeDays * (0.5 + Cast()[agnes].traits.patience) * (1.0 + now / 50.0);
		const float later = log.StandingOf(agnes, 2.1 + cfg::kRegardGraceDays + half).regard;
		const float years = log.StandingOf(agnes, 2.1 + 3650.0).regard;
		std::printf("        regard %.1f, %.1f after the grace days, %.1f a half-life later, %.2f after ten years\n", now, inGrace, later, years);
		Check(std::fabs(inGrace - now) < 1e-3f, "friendship: a week away costs nothing");
		Check(later < now * 0.62f && later > now * 0.38f, "friendship: after the grace days, about half fades per half-life");
		Check(years < 1.0f && years >= 0.0f, "friendship: ten years away and it's all but gone (a deep dive costs friends)");
		const float back = log.Talk(agnes, 2.1 + 3650.0);
		Check(back > 0.0f && log.StandingOf(agnes, 2.1 + 3650.0).Away(2.1 + 3650.0) == 0.0, "friendship: coming back and talking starts it again");
	}
	{
		// Gossip: a big change with Tom reaches Mae (his wife) two days later, not Bram (no tie).
		PlayerLog log;
		log.Add({ 1.0, tom, EventKind::Regard, 10.0f, "test" });
		const float before = log.StandingOf(mae, 1.0 + cfg::kGossipDays - 0.01).regard;
		const float after = log.StandingOf(mae, 1.0 + cfg::kGossipDays + 0.001).regard;
		const float bramHeard = log.StandingOf(bram, 10.0).regard;
		Check(before == 0.0f && std::fabs(after - 10.0f * cfg::kGossipShare) < 0.05f && bramHeard == 0.0f,
			"friendship: gossip passes a share of a big change to the people they're tied to, a couple of days later");
		log.Add({ 20.0, tom, EventKind::Regard, 2.0f, "small" });
		Check(log.StandingOf(mae, 30.0).regard < after, "friendship: small things aren't gossiped about");
	}
	{
		PlayerLog log;
		const float liked = log.Give(tom, 1.1, "Gold");
		const float twice = log.Give(tom, 1.5, "Gold");
		const float neutral = log.Give(bram, 1.1, "Gold");
		const float disliked = log.Give(mae, 1.1, "Time crystal");
		Check(liked > neutral && neutral > 0.0f && disliked < 0.0f && twice == 0.0f, "friendship: gifts they like count most, ones they dislike cost, one a day counts");
	}
	{
		// Turning time back: people met after that day forget you (you remember them: déjà vu);
		// the clockmaker remembers everything.
		PlayerLog log;
		log.Talk(agnes, 2.0);
		log.Talk(tom, 100.0);
		log.Add({ 100.0, tom, EventKind::Regard, 25.0f, "test" });
		log.Talk(clockmaker, 100.0);
		log.Add({ 100.0, clockmaker, EventKind::Regard, 25.0f, "test" });
		const float clockBefore = log.StandingOf(clockmaker, 100.0).regard;
		log.TurnBackTo(50.0);
		const Standing t = log.StandingOf(tom, 50.0), a = log.StandingOf(agnes, 50.0), c = log.StandingOf(clockmaker, 50.0);
		Check(!t.met && t.lostRegard > 20.0f && a.met && a.lostRegard == 0.0f,
			"rewind: someone met after the day you go back to forgets you, and you remember them (déjà vu); earlier friends stay");
		Check(c.met && std::fabs(c.regard - clockBefore) < 0.5f, "rewind: the clockmaker remembers everything");

		// Saving keeps all of it.
		PlayerLog loaded;
		std::string error;
		const bool ok = loaded.FromJson(log.ToJson(), error);
		bool same = ok;
		for (int n = 0; n < static_cast<int>(Cast().size()); ++n)
			for (double day : { 10.0, 50.0, 400.0 })
			{
				const Standing x = log.StandingOf(n, day), y = loaded.StandingOf(n, day);
				same &= x.met == y.met && std::fabs(x.regard - y.regard) < 1e-3f && x.lostRegard == y.lostRegard;
			}
		Check(same, "friendship: the log saves and loads to the same standing with everyone");
	}

	// ---- Dialog files --------------------------------------------------------------------------------
	DialogLibrary library;
	library.LoadFolder(std::string(UNDERTIME_CONTENT_DIR) + "/Dialog");
	for (const std::string& e : library.Errors()) std::printf("        %s\n", e.c_str());
	Check(library.Errors().empty(), "dialog: every file in Content/Dialog loads without problems");
	bool everyone = library.Shared() != nullptr;
	for (int n = 0; n < static_cast<int>(Cast().size()); ++n) everyone &= library.For(n) != nullptr;
	Check(everyone, "dialog: everyone in the cast has a dialog file, and there's Shared.dialog");
	{
		DialogLibrary bad;
		bad.LoadText("bad.dialog", R"({ "npc": "agnes", "topics": [ { "id": "x", "title": "X", "stepz": [] } ] })");
		bad.LoadText("nobody.dialog", R"({ "npc": "nobody", "greetings": [ { "say": "Hi" } ] })");
		bad.LoadText("tier.dialog", R"({ "npc": "tom", "greetings": [ { "say": "Hi", "when": { "tier": ">=Bestie" } } ] })");
		bad.LoadText("broken.dialog", "{ not json");
		Check(bad.Errors().size() >= 4 && bad.FileCount() == 0, "dialog: typos (unknown keys, people, tiers, broken JSON) are reported, and the file isn't used");
	}

	{
		std::vector<std::string> errors;
		const Condition c = ParseCondition(nlohmann::json::parse(R"({ "tier": ">=Friendly", "away": ">=30", "hour": [18, 23], "dead": "bram", "has": { "Copper": 15 } })"), "agnes", "test", errors);
		std::printf("        %s\n", Describe(c).c_str());
		Check(errors.empty() && Describe(c) == "tier >= Friendly, away >= 30 days, hours 18-23, has 15 Copper, bram dead" && Describe(Condition{}).empty(),
			"dialog: conditions read back in words (the editor's dialog view)");
	}

	// ---- A conversation ------------------------------------------------------------------------------
	{
		PlayerLog log;
		Conversation talk;
		Conversation::Situation situation;
		situation.days = 3.4;
		situation.hour = 9.6f;
		talk.Start(library, log, agnes, situation);
		const std::string greeting = talk.Text();
		std::printf("        Agnes: \"%s\"\n", greeting.c_str());
		Check(talk.GetMode() == Conversation::Mode::Line && greeting.find("new face") != std::string::npos, "talk: a first meeting gets the first-meeting greeting");
		talk.Continue();
		std::vector<std::string> options = talk.Options(true);
		const int debt = Find(options, "You came here owing money?");
		Check(talk.GetMode() == Conversation::Mode::Topics && Find(options, "About the trading post") >= 0 && debt >= 0
			&& options[options.size() - 2] == "Give something" && options.back() == "Goodbye", "talk: then topics, with Give something and Goodbye last");
		const float before = log.StandingOf(agnes, situation.days).regard;
		talk.Choose(debt, true);
		talk.Continue();
		Check(talk.GetMode() == Conversation::Mode::Choices && talk.Options(true).size() == 2, "talk: a topic can ask you to choose");
		talk.Choose(0, true);   // "That took courage."
		Check(log.StandingOf(agnes, situation.days).regard > before + 5.0f, "talk: choices change their friendship (through the log)");
		talk.Continue();
		options = talk.Options(true);
		Check(talk.GetMode() == Conversation::Mode::Topics && Find(options, "You came here owing money?") < 0, "talk: a once-topic isn't offered again");
		const float beforeGift = log.StandingOf(agnes, situation.days).regard;
		Check(talk.Choose(Find(options, "Give something"), true), "talk: Give something asks the game what to give");
		talk.Give("Gold");
		std::printf("        Agnes (given gold): \"%s\"\n", talk.Text().c_str());
		Check(log.StandingOf(agnes, situation.days).regard > beforeGift && talk.Text().find("real metal") != std::string::npos, "talk: they react to a gift they like");
		talk.Continue();
		talk.Choose(static_cast<int>(talk.Options(true).size()) - 1, true);   // Goodbye
		if (talk.GetMode() == Conversation::Mode::Line) talk.Continue();
		Check(talk.GetMode() == Conversation::Mode::Over, "talk: Goodbye ends it");

		// A year later she greets you differently.
		Conversation later;
		situation.days += 400.0;
		later.Start(library, log, agnes, situation);
		std::printf("        Agnes, a year later: \"%s\"\n", later.Text().c_str());
		Check(later.Text().find("1 year") != std::string::npos, "talk: after a long absence they say how long you've been gone");
		DialogContext c = later.Context();
		Check(FillText("{tie:husband} and {first}", c) == "Bram and Agnes", "talk: {tie:...} and {first} fill in names");
	}
	{
		// Barks: everyone has something to say to a stranger.
		bool all = true;
		for (int n = 0; n < static_cast<int>(Cast().size()); ++n)
		{
			DialogContext c;
			c.npc = n;
			c.days = 1.0;
			c.hour = 10.0f;
			all &= !library.Pick(&DialogFile::barks, c, 1u).empty() && !library.Pick(&DialogFile::greetings, c, 1u).empty();
		}
		Check(all, "talk: everyone greets and barks at a stranger");
	}

	// ---- The save game -------------------------------------------------------------------------------
	{
		SaveGame save;
		save.seed = 1234;
		save.days = 812.25;
		save.personal = 3600.5;
		save.era = 2;
		save.feet = { 1.5f, 2.0f, -30.25f };
		save.yaw = 1.25f;
		save.backpack = { { "Copper", 12 }, { "Gold", 3 } };
		save.pickLevel = 2;
		save.date = "06:00, 1 May 1873";
		save.log.Talk(tom, 800.0);
		save.log.Add({ 801.0, -1, EventKind::Flag, 0.0f, "tom.partners" });
		const std::string path = (std::filesystem::temp_directory_path() / "undertime-test-save.json").string();
		std::string error;
		SaveGame loaded;
		const bool ok = save.Write(path, error) && loaded.Read(path, error);
		std::filesystem::remove(path);
		Check(ok && loaded.seed == 1234 && loaded.days == 812.25 && loaded.era == 2 && loaded.feet.z == -30.25f && loaded.backpack["Gold"] == 3
			&& loaded.pickLevel == 2 && loaded.date == save.date && loaded.log.StandingOf(tom, 810.0).met && loaded.log.Flag("tom.partners", 810.0),
			"save: everything comes back (clock, place, backpack, gear, the people's log)");
		SaveGame junk;
		Check(!junk.FromJson(nlohmann::json::parse(R"({ "hello": 1 })"), error), "save: something that isn't a save is refused");
	}
	return gDialogFailures;
}
