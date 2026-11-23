#include "Missions.h"
#include "Npcs.h"
#include "Config.h"
#include "Clock.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using nlohmann::json;

namespace npc
{
	namespace
	{
		uint32_t Hash(uint32_t a, uint32_t b)
		{
			uint32_t h = a * 374761393u + b * 668265263u + 0x5bd1e995u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return h ^ (h >> 16);
		}

		bool IsBlockName(const std::string& item)
		{
			for (const cfg::BlockInfo& b : cfg::kBlocks)
				if (item == b.name) return true;
			return false;
		}

		std::string NameOf(const std::string& id)
		{
			const int n = CastIndex(id);
			return n >= 0 ? Cast()[n].name : id;
		}

		Effect MakeEffect(Effect::Kind kind, const std::string& name, float amount = 0.0f)
		{
			Effect e;
			e.kind = kind;
			e.name = name;
			e.amount = amount;
			return e;
		}

		Step Say(const std::string& text)
		{
			Step s;
			s.say = text;
			return s;
		}
	}

	// ---- Loading ---------------------------------------------------------------------------------

	bool MissionBook::Parse(const std::string& where, const json& j, MissionDef& m, bool job)
	{
		const size_t before = mErrors.size();
		auto error = [&](const std::string& what) { mErrors.push_back(where + ": " + what); };
		static const std::set<std::string> kKeys = { "id", "giver", "title", "brief", "when", "offer", "accept", "status", "done", "late",
			"objective", "deadline", "reward", "penalty", "era" };
		if (!j.is_object()) { error("expected an object"); return false; }
		for (auto it = j.begin(); it != j.end(); ++it)
			if (!kKeys.count(it.key()) || (it.key() == "era" && !job)) error("unknown key '" + it.key() + "'");
		try
		{
			m.id = j.value("id", std::string());
			m.giver = j.value("giver", std::string());
			m.title = j.value("title", std::string());
			m.brief = j.value("brief", std::string());
			m.offer = j.value("offer", std::string());
			m.accept = j.value("accept", std::string());
			m.status = j.value("status", std::string());
			m.done = j.value("done", std::string());
			m.late = j.value("late", std::string());
			m.deadline = j.value("deadline", 0.0);
			m.penalty = j.value("penalty", 0.0f);
			m.job = job;
			if (m.id.empty() || m.id.find('@') != std::string::npos || m.id.find(':') != std::string::npos) error("needs an id (without @ or :)");
			if (CastIndex(m.giver) < 0) error("giver: no one called '" + m.giver + "' in the cast");
			if (m.title.empty()) error("needs a title");
			if (m.offer.empty()) error("needs an offer (what they ask)");
			if (j.contains("when")) m.when = ParseCondition(j["when"], m.giver, where + ".when", mErrors);

			const json& o = j.value("objective", json::object());
			const std::string type = o.value("type", std::string());
			auto count = [&]() -> int
			{
				if (!o.contains("count")) return 1;
				if (o["count"].is_array()) return o["count"][0].get<int>();   // board jobs pick in the range
				return o["count"].get<int>();
			};
			if (type == "deliver")
			{
				m.type = ObjectiveType::Deliver;
				m.item = o.value("item", std::string());
				m.count = count();
				if (!IsBlockName(m.item)) error("deliver: '" + m.item + "' isn't a backpack item (Config.h, kBlocks)");
				if (m.count < 1) error("deliver: count must be at least 1");
			}
			else if (type == "retrieve" && !job)
			{
				m.type = ObjectiveType::Retrieve;
				m.thing = o.value("thing", std::string());
				m.zone = o.value("zone", 1);
				if (m.thing.empty()) error("retrieve: what's the thing?");
				if (m.zone < 1 || m.zone > 5) error("retrieve: zone 1..5");
			}
			else if (type == "courier")
			{
				m.type = ObjectiveType::Courier;
				m.thing = o.value("thing", std::string("a letter"));
				if (o.contains("to") && o["to"].is_array()) m.to = o["to"][0].get<std::string>();
				else m.to = o.value("to", std::string());
				if (CastIndex(m.to) < 0) error("courier: no one called '" + m.to + "'");
				if (o.contains("to") && o["to"].is_array())
					for (const json& t : o["to"]) if (CastIndex(t.get<std::string>()) < 0) error("courier: no one called '" + t.get<std::string>() + "'");
			}
			else if (type == "meet" && !job)
			{
				m.type = ObjectiveType::Meet;
				m.place = o.value("place", std::string());
				m.inDays = o.value("inDays", 1.0);
				if (o.contains("hours") && o["hours"].is_array() && o["hours"].size() == 2) { m.hourFrom = o["hours"][0].get<float>(); m.hourTo = o["hours"][1].get<float>(); }
				if (!FindPlace(m.place)) error("meet: no place called '" + m.place + "' (Npcs.cpp)");
				if (m.hourTo <= m.hourFrom) error("meet: hours [from, to] on one day");
			}
			else if (type == "wait" && !job)
			{
				m.type = ObjectiveType::Wait;
				m.waitDays = o.value("days", 0.0);
				if (m.waitDays <= 0.0) error("wait: how many days?");
			}
			else error("objective: type deliver, retrieve, courier, meet or wait" + std::string(job ? " (board jobs: deliver or courier)" : ""));

			const json& r = j.value("reward", json::object());
			for (auto it = r.begin(); it != r.end(); ++it)
				if (it.key() != "regard" && it.key() != "items" && it.key() != "flag") error("reward: unknown key '" + it.key() + "'");
			m.rewardRegard = r.value("regard", 0.0f);
			m.rewardFlag = r.value("flag", std::string());
			if (r.contains("items"))
				for (auto it = r["items"].begin(); it != r["items"].end(); ++it)
				{
					if (!IsBlockName(it.key())) error("reward: '" + it.key() + "' isn't a backpack item");
					m.rewardItems[it.key()] = it.value().get<int>();
				}
		}
		catch (const std::exception& e)
		{
			error(std::string("wrong type of value: ") + e.what());
		}
		return mErrors.size() == before;
	}

	bool MissionBook::LoadText(const std::string& name, const std::string& text)
	{
		const size_t before = mErrors.size();
		json j;
		try { j = json::parse(text); }
		catch (const std::exception& e)
		{
			mErrors.push_back(name + ": not valid JSON: " + e.what());
			return false;
		}
		if (!j.is_object() || (!j.contains("missions") && !j.contains("jobs")))
		{
			mErrors.push_back(name + ": needs \"missions\" (or \"jobs\" for the notice board)");
			return false;
		}
		for (auto it = j.begin(); it != j.end(); ++it)
			if (it.key() != "missions" && it.key() != "jobs") mErrors.push_back(name + ": unknown key '" + it.key() + "'");
		const json& missions = j.value("missions", json::array());
		for (size_t i = 0; i < missions.size(); ++i)
		{
			MissionDef m;
			const std::string where = name + ": missions[" + std::to_string(i) + "]";
			if (!Parse(where, missions[i], m, false)) continue;
			if (Find(m.id)) { mErrors.push_back(where + ": a second mission called '" + m.id + "'"); continue; }
			mMissions.push_back(std::move(m));
		}
		const json& jobs = j.value("jobs", json::array());
		for (size_t i = 0; i < jobs.size(); ++i)
		{
			JobTemplate t;
			const std::string where = name + ": jobs[" + std::to_string(i) + "]";
			if (!Parse(where, jobs[i], t.def, true)) continue;
			t.source = jobs[i];
			if (jobs[i].contains("era"))
			{
				const json& e = jobs[i]["era"];
				if (e.is_array() && e.size() == 2) { t.eraFrom = e[0].get<int>(); t.eraTo = e[1].get<int>(); }
				else if (e.is_number_integer()) t.eraFrom = t.eraTo = e.get<int>();
			}
			mJobs.push_back(std::move(t));
		}
		return mErrors.size() == before;
	}

	void MissionBook::LoadFolder(const std::string& folder)
	{
		mMissions.clear();
		mJobs.clear();
		mMadeJobs.clear();
		mErrors.clear();
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(folder, error))
		{
			if (entry.path().extension() != ".mission") continue;
			std::ifstream in(entry.path(), std::ios::binary);
			std::stringstream text;
			text << in.rdbuf();
			LoadText(entry.path().filename().string(), text.str());
		}
		if (error) mErrors.push_back(folder + ": " + error.message());
	}

	// ---- Board jobs ------------------------------------------------------------------------------

	MissionDef MissionBook::MakeJob(const MissionDef& tmpl, const json& source, long long week) const
	{
		MissionDef m = tmpl;
		m.id = tmpl.id + "@" + std::to_string(week);
		const uint32_t h = Hash(static_cast<uint32_t>(week + 1000000), static_cast<uint32_t>(std::hash<std::string>{}(tmpl.id)));
		const json& o = source.value("objective", json::object());
		if (o.contains("count") && o["count"].is_array() && o["count"].size() == 2)
		{
			const int lo = o["count"][0].get<int>(), hi = o["count"][1].get<int>();
			m.count = lo + static_cast<int>(h % static_cast<uint32_t>(std::max(1, hi - lo + 1)));
		}
		if (o.contains("to") && o["to"].is_array() && !o["to"].empty())
			m.to = o["to"][(h >> 8) % o["to"].size()].get<std::string>();
		m.job = true;
		return m;
	}

	std::vector<std::string> MissionBook::BoardJobs(long long week, int era) const
	{
		std::vector<int> eligible;
		for (size_t i = 0; i < mJobs.size(); ++i)
		{
			const JobTemplate& t = mJobs[i];
			const int giver = CastIndex(t.def.giver);
			const int to = t.def.type == ObjectiveType::Courier ? CastIndex(t.def.to) : giver;
			if (era < t.eraFrom || era > t.eraTo || giver < 0 || era < Cast()[giver].eraFrom || era > Cast()[giver].eraTo) continue;
			if (to < 0 || era < Cast()[to].eraFrom || era > Cast()[to].eraTo) continue;
			eligible.push_back(static_cast<int>(i));
		}
		std::vector<std::string> out;
		for (uint32_t k = 0; k < static_cast<uint32_t>(cfg::kBoardJobsPerWeek) && !eligible.empty(); ++k)
		{
			const size_t pick = Hash(static_cast<uint32_t>(week + 1000000), k + 17u) % eligible.size();
			out.push_back(mJobs[eligible[pick]].def.id + "@" + std::to_string(week));
			eligible.erase(eligible.begin() + pick);
		}
		return out;
	}

	const MissionDef* MissionBook::Find(const std::string& id) const
	{
		for (const MissionDef& m : mMissions)
			if (m.id == id) return &m;
		const size_t at = id.find('@');
		if (at == std::string::npos) return nullptr;
		const auto made = mMadeJobs.find(id);
		if (made != mMadeJobs.end()) return &made->second;
		const std::string base = id.substr(0, at);
		long long week = 0;
		try { week = std::stoll(id.substr(at + 1)); }
		catch (...) { return nullptr; }
		for (const JobTemplate& t : mJobs)
			if (t.def.id == base) return &mMadeJobs.emplace(id, MakeJob(t.def, t.source, week)).first->second;
		return nullptr;
	}

	// ---- Where they stand ------------------------------------------------------------------------

	MissionStatus MissionBook::Status(const PlayerLog& log, const std::string& id, double days) const
	{
		MissionStatus s;
		const std::string found = id + ":found";
		for (const Event& e : log.Events())
		{
			if (e.day > days) break;
			if (e.kind == EventKind::Accepted && e.text == id) { s.state = MissionState::Active; s.accepted = e.day; s.found = false; }
			else if (e.kind == EventKind::Completed && e.text == id) { s.state = MissionState::Done; s.ended = e.day; }
			else if (e.kind == EventKind::Failed && e.text == id) { s.state = MissionState::Failed; s.ended = e.day; }
			else if (e.kind == EventKind::Progress && e.text == found) s.found = true;
		}
		if (s.state == MissionState::Open) return s;
		if (const MissionDef* m = Find(id))
		{
			if (m->deadline > 0.0) s.due = s.accepted + m->deadline;
			if (m->type == ObjectiveType::Meet)
			{
				const double day = std::floor(s.accepted) + m->inDays;
				s.windowFrom = day + m->hourFrom / 24.0;
				s.windowTo = day + m->hourTo / 24.0;
				s.due = std::min(s.due, s.windowTo);
			}
		}
		return s;
	}

	std::vector<std::pair<const MissionDef*, MissionStatus>> MissionBook::Taken(const PlayerLog& log, double days) const
	{
		std::vector<std::pair<const MissionDef*, MissionStatus>> out;
		std::set<std::string> seen;
		for (const Event& e : log.Events())
		{
			if (e.day > days) break;
			if (e.kind != EventKind::Accepted || !seen.insert(e.text).second) continue;
			if (const MissionDef* m = Find(e.text)) out.push_back({ m, Status(log, e.text, days) });
		}
		return out;
	}

	void MissionBook::Accept(PlayerLog& log, const std::string& id, double days) const
	{
		const MissionDef* m = Find(id);
		if (!m || Status(log, id, days).state != MissionState::Open) return;
		log.Add({ days, CastIndex(m->giver), EventKind::Accepted, 0.0f, id });
	}

	std::map<std::string, int> MissionBook::Complete(PlayerLog& log, const std::string& id, double days) const
	{
		const MissionDef* m = Find(id);
		if (!m || Status(log, id, days).state != MissionState::Active) return {};
		const int giver = CastIndex(m->giver);
		log.Add({ days, giver, EventKind::Completed, 0.0f, id });
		if (m->rewardRegard != 0.0f) log.Add({ days, giver, EventKind::Regard, m->rewardRegard, id });
		if (m->type == ObjectiveType::Courier && CastIndex(m->to) >= 0) log.Add({ days, CastIndex(m->to), EventKind::Regard, cfg::kCourierRegard, id });
		if (!m->rewardFlag.empty()) log.Add({ days, -1, EventKind::Flag, 0.0f, m->rewardFlag });
		return m->rewardItems;
	}

	void MissionBook::Found(PlayerLog& log, const std::string& id, double days) const
	{
		const MissionDef* m = Find(id);
		if (!m) return;
		const MissionStatus s = Status(log, id, days);
		if (s.state == MissionState::Active && !s.found) log.Add({ days, CastIndex(m->giver), EventKind::Progress, 0.0f, id + ":found" });
	}

	std::vector<std::string> MissionBook::Expire(PlayerLog& log, double days) const
	{
		std::vector<std::string> failed;
		for (const auto& [m, s] : Taken(log, days))
		{
			if (s.state != MissionState::Active || s.due >= days) continue;
			const int giver = CastIndex(m->giver);
			log.Add({ s.due, giver, EventKind::Failed, 0.0f, m->id });
			if (m->penalty != 0.0f) log.Add({ s.due, giver, EventKind::Regard, -m->penalty, m->id });
			failed.push_back(m->id);
		}
		return failed;
	}

	std::vector<std::string> MissionBook::Arrived(const PlayerLog& log, double days, const std::function<bool(const std::string&)>& near) const
	{
		std::vector<std::string> out;
		for (const auto& [m, s] : Taken(log, days))
			if (m->type == ObjectiveType::Meet && s.state == MissionState::Active && days >= s.windowFrom && days <= s.windowTo && near(m->place)) out.push_back(m->id);
		return out;
	}

	// ---- In conversation -------------------------------------------------------------------------

	void MissionBook::Topics(const DialogContext& context, std::deque<Topic>& out) const
	{
		if (context.npc < 0 || !context.log) return;
		const std::string who = Cast()[context.npc].id;
		const PlayerLog& log = *context.log;
		const double days = context.days;
		auto topic = [&](const std::string& id, const std::string& title) -> Topic&
		{
			out.emplace_back();
			out.back().id = id;
			out.back().title = title;
			return out.back();
		};

		// Offers: written missions they give, when their condition holds.
		for (const MissionDef& m : mMissions)
		{
			if (m.giver != who || Status(log, m.id, days).state != MissionState::Open || !Matches(m.when, context)) continue;
			Topic& t = topic("mission:" + m.id, "Need a hand?  (" + m.title + ")");
			t.steps.push_back(Say(m.offer));
			Step ask;
			Choice yes, no;
			yes.text = "I'll do it.";
			yes.effects.push_back(MakeEffect(Effect::Kind::Accept, m.id));
			yes.then.push_back(Say(m.accept.empty() ? "Good. I'll hold you to it." : m.accept));
			no.text = "Not now.";
			no.then.push_back(Say("Suit yourself."));
			ask.choices = { yes, no };
			t.steps.push_back(ask);
		}

		for (const auto& entry : Taken(log, days))
		{
			const MissionDef* m = entry.first;
			const MissionStatus& s = entry.second;
			const bool mine = m->giver == who;
			if (s.state == MissionState::Failed && mine && !m->late.empty() && !log.Flag("late:" + m->id, days))
			{
				Topic& t = topic("late:" + m->id, "About " + m->title);
				Step said = Say(m->late);
				said.effects.push_back(MakeEffect(Effect::Kind::Flag, "late:" + m->id));
				t.steps.push_back(said);
				continue;
			}
			if (s.state != MissionState::Active) continue;
			const std::string done = m->done.empty() ? "Thank you." : m->done;
			auto finish = [&](Topic& t, std::vector<Effect> first)
			{
				Step step = Say(done);
				step.effects = std::move(first);
				step.effects.push_back(MakeEffect(Effect::Kind::Complete, m->id));
				t.steps.push_back(step);
			};
			if (m->type == ObjectiveType::Courier && m->to == who)
			{
				finish(topic("give:" + m->id, "Give them " + m->thing + " from " + NameOf(m->giver)), {});
				continue;
			}
			if (!mine) continue;
			const std::string status = m->status.empty() ? "Well? Get to it." : m->status;
			switch (m->type)
			{
			case ObjectiveType::Deliver:
				if (context.count && context.count(m->item) >= m->count)
					finish(topic("hand:" + m->id, "Hand over " + std::to_string(m->count) + " " + m->item), { MakeEffect(Effect::Kind::Take, m->item, static_cast<float>(m->count)) });
				else topic("about:" + m->id, "About " + m->title).steps.push_back(Say(status));
				break;
			case ObjectiveType::Retrieve:
				if (s.found) finish(topic("return:" + m->id, "Return " + m->thing), {});
				else topic("about:" + m->id, "About " + m->title).steps.push_back(Say(status));
				break;
			case ObjectiveType::Wait:
				if (days >= s.accepted + m->waitDays) finish(topic("back:" + m->id, "I'm back, as you asked"), {});
				else topic("about:" + m->id, "About " + m->title).steps.push_back(Say(status));
				break;
			default:
				topic("about:" + m->id, "About " + m->title).steps.push_back(Say(status));
				break;
			}
		}
	}

	// ---- Time left -------------------------------------------------------------------------------

	std::string RealSpanText(double seconds)
	{
		char text[64];
		if (seconds >= 3600.0)
		{
			const int h = static_cast<int>(seconds / 3600.0), m = static_cast<int>(std::fmod(seconds, 3600.0) / 60.0);
			if (m > 0) std::snprintf(text, sizeof(text), "%d hour%s %d min", h, h == 1 ? "" : "s", m);
			else std::snprintf(text, sizeof(text), "%d hour%s", h, h == 1 ? "" : "s");
		}
		else if (seconds >= 60.0) std::snprintf(text, sizeof(text), "%d min", static_cast<int>(seconds / 60.0));
		else std::snprintf(text, sizeof(text), "%d s", std::max(1, static_cast<int>(seconds)));
		return text;
	}

	std::string DeadlineText(double daysLeft)
	{
		if (daysLeft <= 0.0) return "too late";
		// Down to the first depth where it's under a minute.
		std::string text = OutsideClock::SpanText(daysLeft) + " left: " + RealSpanText(daysLeft * 86400.0 / cfg::kSurfaceRate) + " outside";
		for (int zone = 0; zone < 5; ++zone)
		{
			const double seconds = daysLeft * 86400.0 / cfg::kZoneRate[zone];
			text += ", " + (seconds < 60.0 ? std::string("under a minute") : RealSpanText(seconds)) + " in zone " + std::to_string(zone + 1);
			if (seconds < 60.0) break;
		}
		return text;
	}
}
