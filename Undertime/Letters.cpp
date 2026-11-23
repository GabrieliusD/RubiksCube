#include "Letters.h"
#include "Life.h"
#include "Npcs.h"
#include "Clock.h"
#include "Config.h"
#include <algorithm>
#include <cmath>

namespace npc
{
	namespace
	{
		uint32_t Hash(uint32_t a, uint32_t b)
		{
			uint32_t h = a * 2246822519u + b * 3266489917u + 0x27d4eb2fu;
			h = (h ^ (h >> 15)) * 668265263u;
			return h ^ (h >> 13);
		}

		TownEventKind KindOf(const std::string& on, bool& known)
		{
			known = true;
			if (on == "wedding") return TownEventKind::Wedding;
			if (on == "birth") return TownEventKind::Birth;
			if (on == "death") return TownEventKind::Death;
			if (on == "arrival") return TownEventKind::Arrival;
			if (on == "role") return TownEventKind::Role;
			if (on == "departure") return TownEventKind::Departure;
			known = false;
			return TownEventKind::Arrival;
		}
	}

	std::vector<Letter> LettersUpTo(const DialogLibrary& library, const PlayerLog& log, double days)
	{
		std::vector<Letter> out;
		const std::vector<TownEvent> events = EventsBetween(0.0, days, &log);
		for (int w = 0; w < static_cast<int>(Cast().size()); ++w)
		{
			const DialogFile* file = library.For(w);
			if (!file || file->letters.empty()) continue;
			// Your conversations with them, in order.
			std::vector<double> talks;
			for (const Event& e : log.Events())
				if (e.npc == w && e.kind == EventKind::Talk && e.day <= days) talks.push_back(e.day);

			for (size_t k = 0; k < file->letters.size(); ++k)
			{
				const LetterDef& def = file->letters[k];
				auto write = [&](double date, int about)
				{
					if (date > days || date < 0.0) return;
					DialogContext c;
					c.npc = w;
					c.days = date;
					c.hour = 12.0f;
					c.era = OutsideClock::EraAt(date / 365.0);
					c.log = &log;
					c.about = about;
					c.standing = log.StandingOf(w, date);
					// Only people who know you, and who are living in the valley, write.
					if (!LifeAt(w, date, &log).present || !c.standing.met || c.standing.tier < Tier::Acquaintance || !Matches(def.when, c)) return;
					Letter letter;
					letter.from = w;
					letter.day = date;
					letter.id = std::string(Cast()[w].id) + ":" + std::to_string(k) + ":" + std::to_string(static_cast<long long>(std::floor(date)));
					letter.text = FillText(def.say[Hash(static_cast<uint32_t>(w), static_cast<uint32_t>(date)) % def.say.size()], c);
					out.push_back(std::move(letter));
				};
				if (def.on == "away")
				{
					// Each long silence: a letter `days` after you last talked.
					for (size_t t = 0; t < talks.size(); ++t)
					{
						const double next = t + 1 < talks.size() ? talks[t + 1] : days;
						if (next - talks[t] >= def.days) write(talks[t] + def.days, -1);
					}
					continue;
				}
				bool known = false;
				const TownEventKind kind = KindOf(def.on, known);
				if (!known) continue;
				for (const TownEvent& e : events)
				{
					if (e.kind != kind) continue;
					const int about = e.npc;
					if (!def.about.empty() && CastIndex(def.about) != e.npc && CastIndex(def.about) != e.other) continue;
					// Only if you haven't talked to them since: you'd have heard.
					const double date = e.day + cfg::kLetterDelayDays;
					if (log.StandingOf(w, date).lastSeen >= e.day) continue;
					write(date, about);
				}
			}
		}
		std::stable_sort(out.begin(), out.end(), [](const Letter& a, const Letter& b) { return a.day < b.day; });
		return out;
	}

	std::vector<Letter> UnreadLetters(const DialogLibrary& library, const PlayerLog& log, double days)
	{
		std::vector<Letter> out;
		for (Letter& l : LettersUpTo(library, log, days))
			if (!log.Flag("read:" + l.id, days)) out.push_back(std::move(l));
		return out;
	}

	int PostKeeper(int era, double days, const PlayerLog* log)
	{
		const int keeper = CastIndex(era <= 1 ? "agnes" : "hattie");
		return keeper >= 0 && LifeAt(keeper, days, log).present ? keeper : -1;
	}

	void LetterTopics(const DialogLibrary& library, const DialogContext& context, std::deque<Topic>& out)
	{
		if (!context.log || context.npc < 0 || context.npc != PostKeeper(context.era, context.days, context.log)) return;
		const std::vector<Letter> unread = UnreadLetters(library, *context.log, context.days);
		if (unread.empty()) return;
		out.emplace_back();
		Topic& t = out.back();
		t.id = "letters";
		t.title = "Any letters for me?  (" + std::to_string(unread.size()) + ")";
		t.priority = 10;
		Step intro;
		intro.say = unread.size() == 1 ? "One came for you." : std::to_string(unread.size()) + " of them, tied with string. Here.";
		t.steps.push_back(intro);
		for (const Letter& l : unread)
		{
			OutsideClock when;
			when.SetDays(l.day);
			std::string date = when.DateText();
			const size_t comma = date.find(", ");
			if (comma != std::string::npos) date = date.substr(comma + 2);   // the day, not the hour
			Step step;
			step.say = "From " + std::string(Cast()[l.from].name) + ", " + date + ":\n\"" + l.text + "\"";
			Effect read;
			read.kind = Effect::Kind::Flag;
			read.name = "read:" + l.id;
			step.effects.push_back(read);
			t.steps.push_back(step);
		}
	}
}
