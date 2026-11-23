// Tests for lives across the eras (Life.h, Letters.h): deaths that a mission can put off,
// the town's events and news, gatherings you join or miss, children who inherit friendship,
// letters while you're away, and turning time back replaying it all.
#include "../Life.h"
#include "../Letters.h"
#include "../Missions.h"
#include "../Npcs.h"
#include "../Config.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
	int gLifeFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gLifeFailures;
	}

	bool Has(const std::vector<std::string>& lines, const std::string& text)
	{
		for (const std::string& l : lines) if (l == text) return true;
		return false;
	}
}

int LifeTests()
{
	using namespace npc;
	const int agnes = CastIndex("agnes"), bram = CastIndex("bram"), tom = CastIndex("tom"), mae = CastIndex("mae"), ella = CastIndex("ella"), hattie = CastIndex("hattie");
	const double year = 365.0;

	// ---- Lives ---------------------------------------------------------------------------------
	{
		PlayerLog log;
		Check(LifeAt(bram, 8.4 * year, &log).alive && !LifeAt(bram, 8.6 * year, &log).alive, "life: Bram dies at 8.5 years");
		log.Add({ 8.0 * year, -1, EventKind::Flag, 0.0f, "bram.saved" });
		Check(LifeAt(bram, 20.0 * year, &log).alive, "life: the remedy in time (a story flag before his death) gives him years");
		log.TurnBackTo(7.5 * year);
		Check(!LifeAt(bram, 8.6 * year, &log).alive, "life: turning time back past the remedy, he dies on the old day again");
		Check(!LifeAt(ella, 2.9 * year, nullptr).born && LifeAt(ella, 3.1 * year, nullptr).present, "life: Ella is born in year 3");
		Check(std::string(LifeAt(agnes, 1.0 * year, nullptr).role) == "Keeper of the trading post" && std::string(LifeAt(agnes, 2.5 * year, nullptr).role) == "Keeper of the general store",
			"life: roles change hands (Agnes keeps the general store from the Settlement)");
		Check(!LifeAt(CastIndex("silas"), 1.0 * year, nullptr).present && LifeAt(CastIndex("silas"), 2.5 * year, nullptr).present, "life: the Settlement's people arrive with it");
	}

	// ---- Events and news -----------------------------------------------------------------------------
	{
		const std::vector<std::string> news = News(0.0, 10.0 * year, nullptr, 20);
		for (const std::string& line : news) std::printf("        %s\n", line.c_str());
		Check(Has(news, "Agnes Rook and Bram Kettle were married.") && Has(news, "Ella Harlan was born to Tom Harlan and Mae Harlan.")
			&& Has(news, "Bram Kettle has died.") && Has(news, "Silas Merrow has come to the valley (assayer)."), "news: weddings, births, deaths and arrivals, in words");
		Check(News(0.0, 1.0, nullptr, 20).empty(), "news: nothing on the first day");
		const std::vector<TownEvent> gatherings = GatheringsOverlapping(1.4 * year, 1.6 * year, nullptr);
		bool wedding = false;
		for (const TownEvent& g : gatherings) wedding |= g.kind == TownEventKind::Wedding && g.gather == "fire" && g.npc == agnes;
		Check(wedding, "events: Agnes and Bram's wedding is a gathering at the campfire (the Camp)");
		int festivals = 0;
		for (const TownEvent& e : EventsBetween(2.0 * year, 10.0 * year, nullptr)) festivals += e.kind == TownEventKind::Festival ? 1 : 0;
		Check(festivals == 8, "events: Founders' Day comes once a year in the Settlement's eight years");
	}

	// ---- Gatherings ----------------------------------------------------------------------------------
	{
		TownEvent wedding;
		for (const TownEvent& g : GatheringsOverlapping(1.4 * year, 1.6 * year, nullptr)) if (g.kind == TownEventKind::Wedding) wedding = g;
		PlayerLog went;
		const float before = went.StandingOf(agnes, wedding.day).regard;
		const auto joined = SettleGatherings(went, wedding.day + 0.05, [&](const TownEvent& g) { return g.id == wedding.id; });
		Check(joined.size() == 1 && went.StandingOf(agnes, wedding.day + 0.06).regard > before, "gatherings: being at a wedding pleases the couple");

		PlayerLog missed;
		missed.Talk(agnes, wedding.day - 2.0);
		missed.Add({ wedding.day - 2.0, agnes, EventKind::Regard, 60.0f, "test" });   // a Friend
		SettleGatherings(missed, wedding.until + 1.0, [](const TownEvent&) { return false; });
		const float after = missed.StandingOf(agnes, wedding.until + 1.0).regard;
		PlayerLog control;
		control.Talk(agnes, wedding.day - 2.0);
		control.Add({ wedding.day - 2.0, agnes, EventKind::Regard, 60.0f, "test" });
		Check(after < control.StandingOf(agnes, wedding.until + 1.0).regard - 3.0f, "gatherings: a close friend minds when you miss their wedding");
	}

	// ---- Children inherit friendship -----------------------------------------------------------------------
	{
		PlayerLog log;
		log.Talk(tom, 2.9 * year);
		log.Add({ 2.9 * year, tom, EventKind::Regard, 50.0f, "test" });
		const Standing child = log.StandingOf(ella, 3.0 * year + 1.0);
		std::printf("        Ella starts at %.1f (Tom was at %.1f)\n", child.regard, log.StandingOf(tom, 3.0 * year).regard);
		Check(child.regard > 20.0f && !child.met, "family: a child starts with a share of their parents' friendship, without having met you");
	}

	// ---- Letters ---------------------------------------------------------------------------------------
	{
		DialogLibrary library;
		library.LoadFolder(std::string(UNDERTIME_CONTENT_DIR) + "/Dialog");
		PlayerLog log;
		log.Talk(mae, 1.0);
		log.Talk(agnes, 1.0);
		const std::vector<Letter> letters = LettersUpTo(library, log, 200.0);
		bool maeAway = false;
		for (const Letter& l : letters)
		{
			std::printf("        day %.0f, from %s: %s\n", l.day, Cast()[l.from].name, l.text.substr(0, 60).c_str());
			maeAway |= l.from == mae && std::fabs(l.day - 61.0) < 0.01;
		}
		Check(maeAway, "letters: a friend writes when you've been away too long");
		Check(LettersUpTo(library, PlayerLog(), 3000.0).empty(), "letters: strangers don't write");
		log.Talk(agnes, 1.4 * year);
		const std::vector<Letter> later = LettersUpTo(library, log, 1.6 * year);
		bool wedding = false;
		for (const Letter& l : later) wedding |= l.from == agnes && l.text.find("married") != std::string::npos;
		Check(wedding, "letters: Agnes writes about her wedding if you weren't there");
		const size_t unread = UnreadLetters(library, log, 200.0).size();
		std::deque<Topic> topics;
		DialogContext atPost;
		atPost.npc = agnes;
		atPost.days = 200.0;
		atPost.era = 1;
		atPost.log = &log;
		LetterTopics(library, atPost, topics);
		Check(unread > 0 && topics.size() == 1 && topics[0].steps.size() == unread + 1, "letters: they wait at the post (Agnes in the Camp), one line each");
		Check(PostKeeper(1, 10.0, nullptr) == agnes && PostKeeper(2, 4.0 * year, nullptr) == hattie, "letters: the trading post in the Camp, the post office in the Settlement");
	}

	// ---- The remedy mission saves Bram ------------------------------------------------------------------------
	{
		MissionBook book;
		book.LoadFolder(std::string(UNDERTIME_CONTENT_DIR) + "/Missions");
		PlayerLog log;
		book.Accept(log, "doc_bram", 7.5 * year);
		book.Complete(log, "doc_bram", 7.5 * year + 5.0);
		Check(LifeAt(bram, 10.0 * year, &log).alive, "life: Dr Calloway's remedy (a mission) keeps Bram alive past 8.5 years");
	}
	return gLifeFailures;
}
