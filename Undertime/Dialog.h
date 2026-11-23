#pragma once
// Dialog (Docs/NPCs.md §5): what people say, from Content/Dialog/<id>.dialog (JSON), plus
// Shared.dialog for lines anyone can use (by voice). A conversation opens with a greeting picked
// for the situation, then offers topics; topics are short trees of lines and choices whose
// effects go into the player log. Pure C++ (tested in UndertimeTests: every file must load).
//
// File format (every key optional unless noted):
//   { "npc": "agnes",                           (required; "*" in Shared.dialog)
//     "greetings": [ line ],  "farewells": [ line ],  "barks": [ line ],  "gifts": [ line ],
//     "letters": [ { "on": "away" / "wedding" / "birth" / "death" / "arrival" / "role" / "departure",
//                    "about": "bram" (whose news; any if left out), "days": 60 (away: after this long
//                    without a word from you), "when": {...}, "say": "text" or [...] } ],
//     "topics": [ { "id": "camp" (required), "title": "About the camp" (required), "when": {...},
//                   "once": false, "priority": 0, "steps": [ step ] } ] }
//   line:   { "say": "text" or ["one", "or another"] (required), "when": {...}, "priority": 0 }
//   step:   { "say": "text" }  or  { "do": [ effect ] }  or
//           { "choice": [ { "text": "..." (required), "when": {...}, "do": [ effect ], "then": [ step ] } ] }
//   effect: { "regard": 5 }  { "flag": "name" }  { "unflag": "name" }  { "end": true }
//           (missions, Missions.h, add: accept, complete, take items)
//   when:   { "tier": ">=Friendly" (a tier, optionally after >=, <=, >, <), "met": true,
//             "away": ">=30" (outside days since you last talked), "hour": [20, 4],
//             "era": 1 or [1, 3], "rain": true, "night": true, "dejaVu": true,
//             "flag": "name" or "!name" or [...], "done": "topic" or "!topic" (a once-topic of
//             this person), "voice": "terse" (shared lines), "gift": "liked" / "neutral" /
//             "disliked" / "again" (gift reactions), "has": { "Copper": 20 } (in the backpack),
//             "dead": "bram" or "!bram", "years": [7, 9] (years since you arrived) }
// Text can hold {name} (their full name), {first}, {role}, {away} ("3 years, 2 months"),
// {era}, {tie:wife} (the first name of whoever they call that), {about} (letters: whose news).
#include "NpcLog.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace npc
{
	struct Condition
	{
		int tierMin = 0, tierMax = 6;                // Tier values
		int met = -1, rain = -1, night = -1, dejaVu = -1;   // -1: either
		double awayMin = -1.0, awayMax = 1e30;
		float hourFrom = -1.0f, hourTo = -1.0f;      // wraps past midnight when from > to
		int eraMin = 1, eraMax = 6;
		std::vector<std::string> flags, notFlags;    // global flags ("done" topics included)
		std::string voice, gift;
		std::vector<std::pair<std::string, int>> has;   // backpack items needed
		std::vector<std::string> dead, alive;           // cast ids
		double yearsMin = -1e30, yearsMax = 1e30;
	};

	struct Effect
	{
		// Accept and Complete name a mission; Take takes `amount` of item `name` from the
		// backpack (the game does it, through Conversation::Hooks).
		enum class Kind { Regard, Flag, Unflag, End, Accept, Complete, Take } kind = Kind::Regard;
		float amount = 0.0f;
		std::string name;
	};

	struct Step;
	struct Choice
	{
		std::string text;
		Condition when;
		std::vector<Effect> effects;
		std::vector<Step> then;
	};
	struct Step
	{
		std::string say;
		std::vector<Choice> choices;
		std::vector<Effect> effects;
	};

	struct Topic
	{
		std::string id, title;
		Condition when;
		bool once = false;
		int priority = 0;
		std::vector<Step> steps;
	};

	struct Line
	{
		std::vector<std::string> say;   // variants
		Condition when;
		int priority = 0;
	};

	struct LetterDef
	{
		std::string on, about;     // what it's about (see the format above); `about`: a cast id or empty
		double days = 60.0;        // "away" letters
		Condition when;
		std::vector<std::string> say;
	};

	struct DialogFile
	{
		std::string npc;
		std::vector<Line> greetings, farewells, barks, gifts;
		std::vector<Topic> topics;
		std::vector<LetterDef> letters;
	};

	// The moment a line is picked for.
	struct DialogContext
	{
		int npc = -1;
		Standing standing;
		double days = 0.0;
		float hour = 12.0f;
		bool rain = false, night = false;
		int era = 1;
		std::string gift;                          // for gift reactions
		const PlayerLog* log = nullptr;            // global flags
		std::function<int(const std::string& item)> count;   // backpack ("has"); none: nothing
		int about = -1;                            // letters: whose news ({about})
	};

	bool Matches(const Condition& when, const DialogContext& context);
	// A condition in words, for the editor's dialog view: "tier >= Friendly, away >= 30 days".
	// Empty when it always holds.
	std::string Describe(const Condition& when);
	// A "when" object as in dialog files (`npc`: whose topics "done" refers to). Problems are
	// added to `errors`, prefixed with `where`.
	Condition ParseCondition(const nlohmann::json& when, const std::string& npc, const std::string& where, std::vector<std::string>& errors);
	// The flag a once-topic sets when it's done.
	std::string DoneFlag(int npc, const std::string& topic);
	// Fills in {name}, {away} and the rest.
	std::string FillText(const std::string& text, const DialogContext& context);

	class DialogLibrary
	{
	public:
		// Reads every .dialog file in `folder`. Problems (bad JSON, unknown keys, people or
		// topics) are listed in Errors(); files with errors are skipped.
		void LoadFolder(const std::string& folder);
		// One file's text (`name` for messages). False, with errors added, if it's broken.
		bool LoadText(const std::string& name, const std::string& text);
		const std::vector<std::string>& Errors() const { return mErrors; }
		size_t FileCount() const { return mFiles.size(); }

		const DialogFile* For(int npc) const;     // null if they have no file
		const DialogFile* For(const std::string& npcId) const;
		const DialogFile* Shared() const;

		// A line for the situation from `lines` of the person's file, then the shared file's:
		// the highest priority that matches; `salt` picks among equals (and variants) so the
		// same moment gives the same line. Empty if none.
		std::string Pick(std::vector<Line> DialogFile::* lines, const DialogContext& context, uint32_t salt) const;
		// Topics on offer, in priority order.
		std::vector<const Topic*> TopicsFor(const DialogContext& context) const;

	private:
		std::map<std::string, std::shared_ptr<DialogFile>> mFiles;   // by npc id ("*": shared)
		std::vector<std::string> mErrors;
	};

	// One conversation, step by step. The module shows Text() and Options() and calls Continue()
	// or Choose(); effects go straight into the log.
	class Conversation
	{
	public:
		enum class Mode { Line, Choices, Topics, Over };
		struct Situation { double days = 0.0; float hour = 12.0f; bool rain = false, night = false; int era = 1; };
		// What the game adds: topics made on the spot (missions), effects only it can carry out
		// (Accept, Complete, Take), and the backpack.
		struct Hooks
		{
			std::function<void(const DialogContext&, std::deque<Topic>& out)> topics;
			std::function<void(const Effect&)> effect;
			std::function<int(const std::string& item)> count;
		};
		void SetHooks(Hooks hooks) { mHooks = std::move(hooks); }

		// Logs the conversation (Talk) and says hello.
		void Start(const DialogLibrary& library, PlayerLog& log, int npc, const Situation& situation);
		Mode GetMode() const { return mMode; }
		int Npc() const { return mNpc; }
		const std::string& Text() const { return mText; }   // what they just said
		// Choices (Mode::Choices) or topics (Mode::Topics; the last two are always "Give
		// something" and "Goodbye", the gift one only if `canGive`).
		std::vector<std::string> Options(bool canGive) const;
		void Continue();                                   // after a line
		// Picks option `index` of Options(canGive). Returns true if it was "Give something"
		// (the module then asks what, and calls Give).
		bool Choose(int index, bool canGive);
		// Gives them one `item` (the module takes it from the backpack); they react.
		void Give(const std::string& item);
		DialogContext Context() const;

	private:
		void Run();          // runs steps until a line or a choice is waiting
		void ListTopics();
		void Apply(const std::vector<Effect>& effects);
		void Say(const std::string& text);
		void Farewell();

		const DialogLibrary* mLibrary = nullptr;
		PlayerLog* mLog = nullptr;
		int mNpc = -1;
		Situation mSituation;
		Mode mMode = Mode::Over;
		std::string mText;
		const Topic* mTopic = nullptr;
		struct Frame { const std::vector<Step>* steps; size_t next; };
		std::vector<Frame> mStack;
		std::vector<const Choice*> mChoices;
		std::vector<const Topic*> mTopics;
		bool mEnding = false;   // an "end" effect: the conversation closes after this line
		Hooks mHooks;
		std::deque<Topic> mMade;   // this listing's topics from Hooks::topics (kept while they run)
		uint32_t mSalt = 0;
	};
}
