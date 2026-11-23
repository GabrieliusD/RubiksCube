#include "Dialog.h"
#include "Npcs.h"
#include "Clock.h"
#include "Life.h"
#include <algorithm>
#include <cstring>
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
			uint32_t h = a * 374761393u + b * 668265263u + 0x9e3779b9u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return h ^ (h >> 16);
		}

		const char* const kTierNames[] = { "Hostile", "Wary", "Stranger", "Acquaintance", "Friendly", "Friend", "Close" };
		const char* const kEraNames[] = { "Camp", "Settlement", "Boomtown", "Industry", "Abandoned", "Gone" };

		// Parsing, collecting problems with where they are ("agnes.dialog: topics[1].steps[0]: ...").
		struct Parser
		{
			std::string file;
			std::vector<std::string>& errors;
			std::string npcId;
			std::vector<std::pair<std::string, std::string>> doneRefs;   // (where, topic)

			void Error(const std::string& where, const std::string& what) { errors.push_back(file + ": " + where + ": " + what); }

			void Keys(const json& j, const std::string& where, std::initializer_list<const char*> allowed)
			{
				if (!j.is_object()) { Error(where, "expected an object"); return; }
				for (auto it = j.begin(); it != j.end(); ++it)
					if (std::none_of(allowed.begin(), allowed.end(), [&](const char* k) { return it.key() == k; })) Error(where, "unknown key '" + it.key() + "'");
			}

			// ">=Friendly" and the like: an operator, then a value.
			static std::pair<std::string, std::string> SplitCompare(const std::string& text)
			{
				for (const char* op : { ">=", "<=", ">", "<", "=" })
					if (text.rfind(op, 0) == 0) return { op, text.substr(std::strlen(op)) };
				return { "=", text };
			}
			template <typename T> static void Range(const std::string& op, T value, T step, T& lo, T& hi)
			{
				if (op == ">=") lo = value;
				else if (op == ">") lo = value + step;
				else if (op == "<=") hi = value;
				else if (op == "<") hi = value - step;
				else { lo = value; hi = value; }
			}

			Condition When(const json& j, const std::string& where)
			{
				Condition c;
				if (j.is_null()) return c;
				Keys(j, where, { "tier", "met", "away", "hour", "era", "rain", "night", "dejaVu", "flag", "done", "voice", "gift", "has", "dead", "years" });
				if (!j.is_object()) return c;
				if (j.contains("tier"))
				{
					const auto [op, name] = SplitCompare(j["tier"].get<std::string>());
					const auto found = std::find(std::begin(kTierNames), std::end(kTierNames), name);
					if (found == std::end(kTierNames)) Error(where, "unknown tier '" + name + "'");
					else Range(op, static_cast<int>(found - std::begin(kTierNames)), 1, c.tierMin, c.tierMax);
				}
				auto boolean = [&](const char* key, int& out) { if (j.contains(key)) { if (!j[key].is_boolean()) Error(where, std::string(key) + " must be true or false"); else out = j[key].get<bool>() ? 1 : 0; } };
				boolean("met", c.met);
				boolean("rain", c.rain);
				boolean("night", c.night);
				boolean("dejaVu", c.dejaVu);
				if (j.contains("away"))
				{
					const json& a = j["away"];
					const auto [op, value] = a.is_string() ? SplitCompare(a.get<std::string>()) : std::make_pair(std::string(">="), a.dump());
					try { Range(op, std::stod(value), 0.0, c.awayMin, c.awayMax); }
					catch (...) { Error(where, "away: expected a number of days"); }
				}
				if (j.contains("hour"))
				{
					const json& h = j["hour"];
					if (!h.is_array() || h.size() != 2 || !h[0].is_number() || !h[1].is_number()) Error(where, "hour: expected [from, to]");
					else { c.hourFrom = h[0].get<float>(); c.hourTo = h[1].get<float>(); }
				}
				if (j.contains("era"))
				{
					const json& e = j["era"];
					if (e.is_number_integer()) c.eraMin = c.eraMax = e.get<int>();
					else if (e.is_array() && e.size() == 2) { c.eraMin = e[0].get<int>(); c.eraMax = e[1].get<int>(); }
					else Error(where, "era: expected a number or [from, to]");
				}
				auto flags = [&](const json& f, bool done)
				{
					std::vector<std::string> names;
					if (f.is_string()) names.push_back(f.get<std::string>());
					else if (f.is_array()) for (const json& x : f) names.push_back(x.get<std::string>());
					else Error(where, "expected a name or a list of names");
					for (std::string name : names)
					{
						const bool negated = !name.empty() && name[0] == '!';
						if (negated) name = name.substr(1);
						if (done)
						{
							doneRefs.push_back({ where, name });
							name = "done:" + npcId + "." + name;
						}
						(negated ? c.notFlags : c.flags).push_back(name);
					}
				};
				if (j.contains("flag")) flags(j["flag"], false);
				if (j.contains("done")) flags(j["done"], true);
				if (j.contains("voice")) c.voice = j["voice"].get<std::string>();
				if (j.contains("dead"))
				{
					std::vector<std::string> names;
					if (j["dead"].is_string()) names.push_back(j["dead"].get<std::string>());
					else for (const json& x : j["dead"]) names.push_back(x.get<std::string>());
					for (std::string name : names)
					{
						const bool negated = !name.empty() && name[0] == '!';
						if (negated) name = name.substr(1);
						if (CastIndex(name) < 0) Error(where, "dead: no one called '" + name + "'");
						(negated ? c.alive : c.dead).push_back(name);
					}
				}
				if (j.contains("years"))
				{
					const json& y = j["years"];
					if (!y.is_array() || y.size() != 2) Error(where, "years: expected [from, to]");
					else { c.yearsMin = y[0].get<double>(); c.yearsMax = y[1].get<double>(); }
				}
				if (j.contains("has"))
				{
					if (!j["has"].is_object()) Error(where, "has: expected { \"item\": count }");
					else for (auto it = j["has"].begin(); it != j["has"].end(); ++it) c.has.push_back({ it.key(), it.value().get<int>() });
				}
				if (j.contains("gift"))
				{
					c.gift = j["gift"].get<std::string>();
					if (c.gift != "liked" && c.gift != "neutral" && c.gift != "disliked" && c.gift != "again") Error(where, "gift: liked, neutral, disliked or again");
				}
				return c;
			}

			std::vector<Effect> Effects(const json& j, const std::string& where)
			{
				std::vector<Effect> out;
				if (!j.is_array()) { Error(where, "do: expected a list"); return out; }
				for (size_t i = 0; i < j.size(); ++i)
				{
					const std::string at = where + "[" + std::to_string(i) + "]";
					Keys(j[i], at, { "regard", "flag", "unflag", "end" });
					if (j[i].size() != 1) { Error(at, "one effect per entry"); continue; }
					Effect e;
					if (j[i].contains("regard")) { e.kind = Effect::Kind::Regard; e.amount = j[i]["regard"].get<float>(); }
					else if (j[i].contains("flag")) { e.kind = Effect::Kind::Flag; e.name = j[i]["flag"].get<std::string>(); }
					else if (j[i].contains("unflag")) { e.kind = Effect::Kind::Unflag; e.name = j[i]["unflag"].get<std::string>(); }
					else if (j[i].contains("end")) e.kind = Effect::Kind::End;
					else continue;
					out.push_back(e);
				}
				return out;
			}

			std::vector<Step> Steps(const json& j, const std::string& where)
			{
				std::vector<Step> out;
				if (!j.is_array()) { Error(where, "expected a list of steps"); return out; }
				for (size_t i = 0; i < j.size(); ++i)
				{
					const std::string at = where + "[" + std::to_string(i) + "]";
					Keys(j[i], at, { "say", "do", "choice" });
					Step s;
					if (j[i].contains("say")) s.say = j[i]["say"].get<std::string>();
					if (j[i].contains("do")) s.effects = Effects(j[i]["do"], at + ".do");
					if (j[i].contains("choice"))
					{
						const json& choices = j[i]["choice"];
						if (!choices.is_array() || choices.empty()) Error(at, "choice: expected a list");
						else
							for (size_t c = 0; c < choices.size(); ++c)
							{
								const std::string cat = at + ".choice[" + std::to_string(c) + "]";
								Keys(choices[c], cat, { "text", "when", "do", "then" });
								Choice choice;
								if (!choices[c].contains("text")) Error(cat, "a choice needs text");
								else choice.text = choices[c]["text"].get<std::string>();
								if (choices[c].contains("when")) choice.when = When(choices[c]["when"], cat + ".when");
								if (choices[c].contains("do")) choice.effects = Effects(choices[c]["do"], cat + ".do");
								if (choices[c].contains("then")) choice.then = Steps(choices[c]["then"], cat + ".then");
								s.choices.push_back(std::move(choice));
							}
					}
					if (s.say.empty() && s.effects.empty() && s.choices.empty()) Error(at, "an empty step");
					out.push_back(std::move(s));
				}
				return out;
			}

			std::vector<Line> Lines(const json& j, const std::string& where)
			{
				std::vector<Line> out;
				if (!j.is_array()) { Error(where, "expected a list of lines"); return out; }
				for (size_t i = 0; i < j.size(); ++i)
				{
					const std::string at = where + "[" + std::to_string(i) + "]";
					Keys(j[i], at, { "say", "when", "priority" });
					Line line;
					if (j[i].contains("say"))
					{
						const json& say = j[i]["say"];
						if (say.is_string()) line.say.push_back(say.get<std::string>());
						else if (say.is_array()) for (const json& s : say) line.say.push_back(s.get<std::string>());
					}
					if (line.say.empty()) Error(at, "a line needs something to say");
					if (j[i].contains("when")) line.when = When(j[i]["when"], at + ".when");
					line.priority = j[i].value("priority", 0);
					out.push_back(std::move(line));
				}
				return out;
			}
		};
	}

	std::string Describe(const Condition& c)
	{
		std::vector<std::string> parts;
		auto number = [](double v)
		{
			char text[32];
			std::snprintf(text, sizeof(text), "%g", v);
			return std::string(text);
		};
		if (c.tierMin > 0 || c.tierMax < 6)
		{
			if (c.tierMin == c.tierMax) parts.push_back(std::string("tier ") + kTierNames[c.tierMin]);
			else if (c.tierMax >= 6) parts.push_back(std::string("tier >= ") + kTierNames[c.tierMin]);
			else if (c.tierMin <= 0) parts.push_back(std::string("tier <= ") + kTierNames[c.tierMax]);
			else parts.push_back(std::string("tier ") + kTierNames[c.tierMin] + ".." + kTierNames[c.tierMax]);
		}
		if (c.met >= 0) parts.push_back(c.met ? "met" : "never met");
		if (c.awayMin >= 0.0) parts.push_back("away >= " + number(c.awayMin) + " days");
		if (c.awayMax < 1e29) parts.push_back("away <= " + number(c.awayMax) + " days");
		if (c.hourFrom >= 0.0f) parts.push_back("hours " + number(c.hourFrom) + "-" + number(c.hourTo));
		if (c.eraMin > 1 || c.eraMax < 6) parts.push_back(c.eraMin == c.eraMax ? "era " + std::string(kEraNames[c.eraMin - 1]) : "eras " + std::string(kEraNames[c.eraMin - 1]) + ".." + kEraNames[c.eraMax - 1]);
		if (c.rain >= 0) parts.push_back(c.rain ? "raining" : "dry");
		if (c.night >= 0) parts.push_back(c.night ? "night" : "day");
		if (c.dejaVu >= 0) parts.push_back(c.dejaVu ? "deja vu" : "no deja vu");
		for (const std::string& f : c.flags) parts.push_back(f.rfind("done:", 0) == 0 ? "done " + f.substr(f.find('.') + 1) : "flag " + f);
		for (const std::string& f : c.notFlags) parts.push_back(f.rfind("done:", 0) == 0 ? "not done " + f.substr(f.find('.') + 1) : "no flag " + f);
		if (!c.voice.empty()) parts.push_back("voice " + c.voice);
		if (!c.gift.empty()) parts.push_back("gift " + c.gift);
		for (const auto& [item, n] : c.has) parts.push_back("has " + std::to_string(n) + " " + item);
		for (const std::string& d : c.dead) parts.push_back(d + " dead");
		for (const std::string& d : c.alive) parts.push_back(d + " alive");
		if (c.yearsMin > -1e29 || c.yearsMax < 1e29) parts.push_back("years " + number(c.yearsMin) + "-" + number(c.yearsMax));
		std::string out;
		for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
		return out;
	}

	Condition ParseCondition(const json& when, const std::string& npc, const std::string& where, std::vector<std::string>& errors)
	{
		Parser p{ "", errors };
		p.npcId = npc;
		try { return p.When(when, where); }
		catch (const std::exception& e)
		{
			errors.push_back(where + ": wrong type of value: " + e.what());
			return {};
		}
	}

	std::string DoneFlag(int npc, const std::string& topic)
	{
		return "done:" + std::string(npc >= 0 && npc < static_cast<int>(Cast().size()) ? Cast()[npc].id : "*") + "." + topic;
	}

	bool Matches(const Condition& c, const DialogContext& x)
	{
		const Standing& s = x.standing;
		const int tier = static_cast<int>(s.tier);
		if (tier < c.tierMin || tier > c.tierMax) return false;
		if (c.met >= 0 && s.met != (c.met == 1)) return false;
		if (c.awayMin >= 0.0 || c.awayMax < 1e29)
		{
			if (!s.met) return false;
			const double away = s.Away(x.days);
			if (away < c.awayMin || away > c.awayMax) return false;
		}
		if (c.hourFrom >= 0.0f)
		{
			const bool inside = c.hourFrom <= c.hourTo ? (x.hour >= c.hourFrom && x.hour < c.hourTo) : (x.hour >= c.hourFrom || x.hour < c.hourTo);
			if (!inside) return false;
		}
		if (x.era < c.eraMin || x.era > c.eraMax) return false;
		if (c.rain >= 0 && x.rain != (c.rain == 1)) return false;
		if (c.night >= 0 && x.night != (c.night == 1)) return false;
		if (c.dejaVu >= 0 && (s.lostRegard > 0.0f && !s.met) != (c.dejaVu == 1)) return false;
		for (const std::string& f : c.flags)
			if (!x.log || !x.log->Flag(f, x.days)) return false;
		for (const std::string& f : c.notFlags)
			if (x.log && x.log->Flag(f, x.days)) return false;
		if (!c.voice.empty() && (x.npc < 0 || c.voice != Cast()[x.npc].voice)) return false;
		if (c.gift != x.gift) return false;   // gift lines only answer gifts, and other lines never do
		for (const auto& [item, n] : c.has)
			if (!x.count || x.count(item) < n) return false;
		for (const std::string& id : c.dead)
			if (LifeAt(CastIndex(id), x.days, x.log).alive) return false;
		for (const std::string& id : c.alive)
			if (!LifeAt(CastIndex(id), x.days, x.log).alive) return false;
		const double years = x.days / 365.0;
		if (years < c.yearsMin || years > c.yearsMax) return false;
		return true;
	}

	std::string FillText(const std::string& text, const DialogContext& x)
	{
		if (x.npc < 0) return text;
		const NpcDef& who = Cast()[x.npc];
		auto firstName = [](const NpcDef& n)
		{
			const std::string name = n.name;
			if (name.rfind("The ", 0) == 0) return name;
			return name.substr(0, name.find(' '));
		};
		std::string out;
		for (size_t i = 0; i < text.size();)
		{
			if (text[i] != '{')
			{
				out += text[i++];
				continue;
			}
			const size_t close = text.find('}', i);
			if (close == std::string::npos)
			{
				out += text.substr(i);
				break;
			}
			const std::string key = text.substr(i + 1, close - i - 1);
			if (key == "name") out += who.name;
			else if (key == "first") out += firstName(who);
			else if (key == "role") out += who.role;
			else if (key == "away") out += OutsideClock::SpanText(x.standing.Away(x.days));
			else if (key == "era") out += kEraNames[std::clamp(x.era, 1, 6) - 1];
			else if (key == "about") out += x.about >= 0 && x.about < static_cast<int>(Cast().size()) ? firstName(Cast()[x.about]) : std::string("someone");
			else if (key.rfind("tie:", 0) == 0)
			{
				const std::string relation = key.substr(4);
				for (const Tie& tie : who.ties)
					if (relation == tie.relation && CastIndex(tie.id) >= 0) { out += firstName(Cast()[CastIndex(tie.id)]); break; }
			}
			else out += "{" + key + "}";
			i = close + 1;
		}
		return out;
	}

	// ---- The library -----------------------------------------------------------------------------

	bool DialogLibrary::LoadText(const std::string& name, const std::string& text)
	{
		const size_t errorsBefore = mErrors.size();
		json j;
		try { j = json::parse(text); }
		catch (const std::exception& e)
		{
			mErrors.push_back(name + ": not valid JSON: " + e.what());
			return false;
		}
		Parser p{ name, mErrors };
		p.Keys(j, "file", { "npc", "greetings", "farewells", "barks", "gifts", "topics", "letters" });
		if (!j.is_object() || !j.contains("npc") || !j["npc"].is_string())
		{
			p.Error("file", "needs \"npc\": the person's id (\"*\" for shared lines)");
			return false;
		}
		auto file = std::make_shared<DialogFile>();
		file->npc = j["npc"].get<std::string>();
		p.npcId = file->npc;
		if (file->npc != "*" && CastIndex(file->npc) < 0) p.Error("npc", "no one called '" + file->npc + "' in the cast (Npcs.cpp)");
		if (mFiles.count(file->npc)) p.Error("npc", "a second file for '" + file->npc + "'");
		try
		{
			if (j.contains("greetings")) file->greetings = p.Lines(j["greetings"], "greetings");
			if (j.contains("farewells")) file->farewells = p.Lines(j["farewells"], "farewells");
			if (j.contains("barks")) file->barks = p.Lines(j["barks"], "barks");
			if (j.contains("gifts")) file->gifts = p.Lines(j["gifts"], "gifts");
			if (j.contains("letters"))
			{
				static const char* const kOn[] = { "away", "wedding", "birth", "death", "arrival", "role", "departure" };
				const json& letters = j["letters"];
				for (size_t i = 0; letters.is_array() && i < letters.size(); ++i)
				{
					const std::string at = "letters[" + std::to_string(i) + "]";
					p.Keys(letters[i], at, { "on", "about", "days", "when", "say" });
					LetterDef l;
					l.on = letters[i].value("on", std::string());
					l.about = letters[i].value("about", std::string());
					l.days = letters[i].value("days", 60.0);
					if (std::none_of(std::begin(kOn), std::end(kOn), [&](const char* k) { return l.on == k; })) p.Error(at, "on: away, wedding, birth, death, arrival, role or departure");
					if (!l.about.empty() && CastIndex(l.about) < 0) p.Error(at, "about: no one called '" + l.about + "'");
					if (letters[i].contains("when")) l.when = p.When(letters[i]["when"], at + ".when");
					const json& say = letters[i].value("say", json());
					if (say.is_string()) l.say.push_back(say.get<std::string>());
					else if (say.is_array()) for (const json& x : say) l.say.push_back(x.get<std::string>());
					if (l.say.empty()) p.Error(at, "a letter needs something to say");
					file->letters.push_back(std::move(l));
				}
			}
			if (j.contains("topics"))
			{
				const json& topics = j["topics"];
				std::set<std::string> ids;
				for (size_t i = 0; topics.is_array() && i < topics.size(); ++i)
				{
					const std::string at = "topics[" + std::to_string(i) + "]";
					p.Keys(topics[i], at, { "id", "title", "when", "once", "priority", "steps" });
					Topic t;
					t.id = topics[i].value("id", std::string());
					t.title = topics[i].value("title", std::string());
					if (t.id.empty() || t.title.empty()) p.Error(at, "a topic needs an id and a title");
					if (!ids.insert(t.id).second) p.Error(at, "a second topic called '" + t.id + "'");
					if (topics[i].contains("when")) t.when = p.When(topics[i]["when"], at + ".when");
					t.once = topics[i].value("once", false);
					t.priority = topics[i].value("priority", 0);
					if (topics[i].contains("steps")) t.steps = p.Steps(topics[i]["steps"], at + ".steps");
					if (t.steps.empty()) p.Error(at, "a topic needs steps");
					file->topics.push_back(std::move(t));
				}
				if (!topics.is_array()) p.Error("topics", "expected a list");
			}
		}
		catch (const std::exception& e)
		{
			p.Error("file", std::string("wrong type of value: ") + e.what());
		}
		for (const auto& [where, topic] : p.doneRefs)
		{
			const auto found = std::find_if(file->topics.begin(), file->topics.end(), [&](const Topic& t) { return t.id == topic; });
			if (found == file->topics.end() || !found->once) p.Error(where, "done: '" + topic + "' isn't a once-topic in this file");
		}
		if (mErrors.size() != errorsBefore) return false;
		mFiles[file->npc] = file;
		return true;
	}

	void DialogLibrary::LoadFolder(const std::string& folder)
	{
		mFiles.clear();
		mErrors.clear();
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(folder, error))
		{
			if (entry.path().extension() != ".dialog") continue;
			std::ifstream in(entry.path(), std::ios::binary);
			std::stringstream text;
			text << in.rdbuf();
			LoadText(entry.path().filename().string(), text.str());
		}
		if (error) mErrors.push_back(folder + ": " + error.message());
	}

	const DialogFile* DialogLibrary::For(int npc) const
	{
		if (npc < 0 || npc >= static_cast<int>(Cast().size())) return nullptr;
		const auto it = mFiles.find(Cast()[npc].id);
		return it == mFiles.end() ? nullptr : it->second.get();
	}

	const DialogFile* DialogLibrary::For(const std::string& npcId) const
	{
		const auto it = mFiles.find(npcId);
		return it == mFiles.end() ? nullptr : it->second.get();
	}

	const DialogFile* DialogLibrary::Shared() const
	{
		const auto it = mFiles.find("*");
		return it == mFiles.end() ? nullptr : it->second.get();
	}

	std::string DialogLibrary::Pick(std::vector<Line> DialogFile::* lines, const DialogContext& context, uint32_t salt) const
	{
		// The person's own lines win ties with shared ones.
		std::vector<std::pair<int, const Line*>> found;
		int best = -1000000;
		for (const DialogFile* file : { For(context.npc), Shared() })
		{
			if (!file) continue;
			const int bias = file == Shared() ? 0 : 1;
			for (const Line& line : file->*lines)
			{
				if (!Matches(line.when, context)) continue;
				const int rank = line.priority * 2 + bias;
				if (rank > best) { best = rank; found.clear(); }
				if (rank == best) found.push_back({ rank, &line });
			}
		}
		if (found.empty()) return {};
		const Line& line = *found[Hash(salt, 1) % found.size()].second;
		return FillText(line.say[Hash(salt, 2) % line.say.size()], context);
	}

	std::vector<const Topic*> DialogLibrary::TopicsFor(const DialogContext& context) const
	{
		std::vector<const Topic*> topics;
		for (const DialogFile* file : { For(context.npc), Shared() })
		{
			if (!file) continue;
			for (const Topic& t : file->topics)
			{
				if (!Matches(t.when, context)) continue;
				if (t.once && context.log && context.log->Flag(DoneFlag(context.npc, t.id), context.days)) continue;
				topics.push_back(&t);
			}
		}
		std::stable_sort(topics.begin(), topics.end(), [](const Topic* a, const Topic* b) { return a->priority > b->priority; });
		return topics;
	}

	// ---- A conversation --------------------------------------------------------------------------

	DialogContext Conversation::Context() const
	{
		DialogContext c;
		c.npc = mNpc;
		c.days = mSituation.days;
		c.hour = mSituation.hour;
		c.rain = mSituation.rain;
		c.night = mSituation.night;
		c.era = mSituation.era;
		c.log = mLog;
		c.count = mHooks.count;
		if (mLog) c.standing = mLog->StandingOf(mNpc, mSituation.days);
		return c;
	}

	void Conversation::Start(const DialogLibrary& library, PlayerLog& log, int npc, const Situation& situation)
	{
		mLibrary = &library;
		mLog = &log;
		mNpc = npc;
		mSituation = situation;
		mStack.clear();
		mChoices.clear();
		mTopics.clear();
		mTopic = nullptr;
		mEnding = false;
		mSalt = Hash(static_cast<uint32_t>(npc) + 7u, static_cast<uint32_t>(std::floor(situation.days * 96.0)));
		// The greeting is picked for how things stood before this conversation (a first meeting,
		// a long absence), then the conversation is logged.
		const DialogContext before = Context();
		std::string greeting = library.Pick(&DialogFile::greetings, before, mSalt);
		log.Talk(npc, situation.days);
		if (greeting.empty()) greeting = "...";
		Say(greeting);
	}

	void Conversation::Say(const std::string& text)
	{
		mText = text;
		mMode = Mode::Line;
	}

	std::vector<std::string> Conversation::Options(bool canGive) const
	{
		std::vector<std::string> out;
		if (mMode == Mode::Choices)
			for (const Choice* c : mChoices) out.push_back(FillText(c->text, Context()));
		else if (mMode == Mode::Topics)
		{
			for (const Topic* t : mTopics) out.push_back(FillText(t->title, Context()));
			if (canGive) out.push_back("Give something");
			out.push_back("Goodbye");
		}
		return out;
	}

	void Conversation::Continue()
	{
		if (mMode != Mode::Line) return;
		if (mEnding && mStack.empty())
		{
			mMode = Mode::Over;
			return;
		}
		Run();
	}

	bool Conversation::Choose(int index, bool canGive)
	{
		if (mMode == Mode::Choices)
		{
			if (index < 0 || index >= static_cast<int>(mChoices.size())) return false;
			const Choice* choice = mChoices[index];
			Apply(choice->effects);
			if (!choice->then.empty()) mStack.push_back({ &choice->then, 0 });
			Run();
			return false;
		}
		if (mMode != Mode::Topics) return false;
		const int topics = static_cast<int>(mTopics.size());
		if (index >= 0 && index < topics)
		{
			mTopic = mTopics[index];
			mStack.push_back({ &mTopic->steps, 0 });
			Run();
			return false;
		}
		if (canGive && index == topics) return true;
		if (index == topics + (canGive ? 1 : 0)) Farewell();
		return false;
	}

	void Conversation::Give(const std::string& item)
	{
		const NpcDef& who = Cast()[mNpc];
		const Standing before = mLog->StandingOf(mNpc, mSituation.days);
		const auto has = [&](const std::vector<const char*>& list) { return std::any_of(list.begin(), list.end(), [&](const char* x) { return item == x; }); };
		DialogContext c = Context();
		c.gift = std::floor(before.lastGift) == std::floor(mSituation.days) ? "again" : has(who.likes) ? "liked" : has(who.dislikes) ? "disliked" : "neutral";
		mLog->Give(mNpc, mSituation.days, item);
		c.standing = mLog->StandingOf(mNpc, mSituation.days);
		std::string reply = mLibrary->Pick(&DialogFile::gifts, c, mSalt + 101u);
		if (reply.empty()) reply = "Thank you.";
		mStack.clear();
		Say(reply);
	}

	void Conversation::Apply(const std::vector<Effect>& effects)
	{
		for (const Effect& e : effects)
		{
			switch (e.kind)
			{
			case Effect::Kind::Regard: mLog->Add({ mSituation.days, mNpc, EventKind::Regard, e.amount, mTopic ? mTopic->id : "" }); break;
			case Effect::Kind::Flag: mLog->Add({ mSituation.days, -1, EventKind::Flag, 0.0f, e.name }); break;
			case Effect::Kind::Unflag: mLog->Add({ mSituation.days, -1, EventKind::Unflag, 0.0f, e.name }); break;
			case Effect::Kind::End: mEnding = true; break;
			default: if (mHooks.effect) mHooks.effect(e); break;
			}
		}
	}

	void Conversation::Run()
	{
		while (!mStack.empty())
		{
			Frame& frame = mStack.back();
			if (frame.next >= frame.steps->size())
			{
				mStack.pop_back();
				continue;
			}
			const Step& step = (*frame.steps)[frame.next++];
			Apply(step.effects);
			if (!step.choices.empty())
			{
				mChoices.clear();
				const DialogContext c = Context();
				for (const Choice& choice : step.choices)
					if (Matches(choice.when, c)) mChoices.push_back(&choice);
				if (mChoices.empty()) continue;
				if (!step.say.empty()) mText = FillText(step.say, c);
				mMode = Mode::Choices;
				return;
			}
			if (!step.say.empty())
			{
				Say(FillText(step.say, Context()));
				return;
			}
		}
		// The topic is done.
		if (mTopic && mTopic->once) mLog->Add({ mSituation.days, -1, EventKind::Flag, 0.0f, DoneFlag(mNpc, mTopic->id) });
		mTopic = nullptr;
		if (mEnding)
		{
			mMode = Mode::Over;
			return;
		}
		ListTopics();
	}

	void Conversation::ListTopics()
	{
		// The game's topics (missions) first, then the dialog file's.
		const DialogContext c = Context();
		mMade.clear();
		if (mHooks.topics) mHooks.topics(c, mMade);
		mTopics.clear();
		for (const Topic& t : mMade) mTopics.push_back(&t);
		for (const Topic* t : mLibrary->TopicsFor(c)) mTopics.push_back(t);
		mMode = Mode::Topics;
	}

	void Conversation::Farewell()
	{
		mStack.clear();
		const std::string line = mLibrary->Pick(&DialogFile::farewells, Context(), mSalt + 57u);
		mEnding = true;
		if (line.empty()) mMode = Mode::Over;
		else Say(line);
	}
}
