#pragma once
// Missions (Docs/NPCs.md §6): work people give you, with deadlines on the outside clock, so
// choosing how deep to dig is choosing which promises you can keep. Written ones come from
// Content/Missions/<id>.mission; Board.mission holds templates for the notice board's jobs,
// a few each outside week, each posted by a written person. Whether a mission is on offer,
// running, done or failed is worked out from the player log, like friendship, so turning time
// back undoes it. Pure C++ (tested in UndertimeTests).
//
// <id>.mission: { "missions": [ {
//     "id": "tom_pan" (required, unique), "giver": "tom" (required), "title": "Tom's lost pan" (required),
//     "brief": "for the journal", "when": { dialog condition: when it's offered },
//     "offer": "what they ask" (required), "accept": "their thanks", "status": "while it runs",
//     "done": "when you finish", "late": "when you failed it (said once)",
//     "objective": { "type": "deliver", "item": "Copper", "count": 15 }
//                | { "type": "retrieve", "thing": "Tom's gold pan", "zone": 1 }   (lies in a cavern of that zone)
//                | { "type": "courier", "thing": "a letter", "to": "agnes" }
//                | { "type": "meet", "place": "fire.e", "inDays": 2, "hours": [18, 21] }   (be there then)
//                | { "type": "wait", "days": 365 }   (come back after that long)
//     "deadline": 10 (outside days after accepting; 0: none),
//     "reward": { "regard": 10, "items": { "Silver": 3 }, "flag": "name" },
//     "penalty": 8 (friendship lost when you fail) } ] }
// Board.mission: { "jobs": [ { "id": "stone" (required), "giver": "agnes", "title": "...", "brief": "...",
//     "offer": "...", "done": "...", "objective": { deliver or courier; "count": [20, 40] picks in a range },
//     "deadline": 7, "reward": {...}, "era": [1, 1] } ] }
#include "Dialog.h"
#include <map>
#include <string>
#include <vector>

namespace npc
{
	enum class ObjectiveType { Deliver, Retrieve, Courier, Meet, Wait };

	struct MissionDef
	{
		std::string id, giver, title, brief;
		Condition when;
		std::string offer, accept, status, done, late;
		ObjectiveType type = ObjectiveType::Deliver;
		std::string item;          // deliver
		int count = 0;
		std::string thing;         // retrieve, courier: what you carry
		int zone = 1;              // retrieve: the cave zone (1..5)
		std::string to;            // courier: who it's for
		std::string place;         // meet: a place id (Npcs.cpp)
		double inDays = 0.0;       // meet: days after accepting (the day of it)
		float hourFrom = 0.0f, hourTo = 24.0f;
		double waitDays = 0.0;     // wait
		double deadline = 0.0;     // outside days after accepting; 0: none
		float rewardRegard = 0.0f;
		std::map<std::string, int> rewardItems;
		std::string rewardFlag;
		float penalty = 0.0f;
		bool job = false;          // from the notice board
	};

	enum class MissionState { Open, Active, Done, Failed };

	struct MissionStatus
	{
		MissionState state = MissionState::Open;
		double accepted = 0.0;     // outside days
		double ended = 0.0;
		bool found = false;        // retrieve: you have the thing
		// When it must be done by (outside days; a large number for none), and for meet, the window.
		double due = 1e30, windowFrom = 0.0, windowTo = 0.0;
	};

	class MissionBook
	{
	public:
		// Reads every .mission file in `folder`; problems go to Errors() (and the mission is skipped).
		void LoadFolder(const std::string& folder);
		bool LoadText(const std::string& name, const std::string& text);
		const std::vector<std::string>& Errors() const { return mErrors; }
		size_t Count() const { return mMissions.size(); }
		size_t JobTemplates() const { return mJobs.size(); }

		// A mission by id, written or a board job ("<template>@<week>"; made from its template).
		const MissionDef* Find(const std::string& id) const;
		const std::vector<MissionDef>& Written() const { return mMissions; }

		// The notice board in outside week `week` (days / 7), in `era`: the same every time.
		std::vector<std::string> BoardJobs(long long week, int era) const;

		MissionStatus Status(const PlayerLog& log, const std::string& id, double days) const;
		// Every mission taken on (in the order accepted), with where it stands at `days`.
		std::vector<std::pair<const MissionDef*, MissionStatus>> Taken(const PlayerLog& log, double days) const;

		// Taking one on, finishing it (logs the reward friendship and flag; returns the items to
		// give), and the steps between.
		void Accept(PlayerLog& log, const std::string& id, double days) const;
		std::map<std::string, int> Complete(PlayerLog& log, const std::string& id, double days) const;
		void Found(PlayerLog& log, const std::string& id, double days) const;
		// Missions whose time ran out by `days` are failed (logged on their deadline, with the lost
		// friendship). Returns the ones that failed now.
		std::vector<std::string> Expire(PlayerLog& log, double days) const;
		// Meet missions you've just made: you're at the place (`near(place)`) in its window.
		// The game then calls Complete for each.
		std::vector<std::string> Arrived(const PlayerLog& log, double days, const std::function<bool(const std::string& place)>& near) const;

		// The mission topics for a conversation with `context.npc`: offers, handing things over,
		// news on the ones running, deliveries for them (courier).
		void Topics(const DialogContext& context, std::deque<Topic>& out) const;

	private:
		bool Parse(const std::string& where, const nlohmann::json& j, MissionDef& out, bool job);
		MissionDef MakeJob(const MissionDef& tmpl, const nlohmann::json& source, long long week) const;

		std::vector<MissionDef> mMissions;
		struct JobTemplate { MissionDef def; nlohmann::json source; int eraFrom = 1, eraTo = 6; };
		std::vector<JobTemplate> mJobs;
		mutable std::map<std::string, MissionDef> mMadeJobs;   // board jobs made so far, by id
		std::vector<std::string> mErrors;
	};

	// "5 days: 1 hour 15 minutes on the surface, 15 minutes in zone 1, ..." (real time, how
	// long you can stay at each depth before it's too late).
	std::string DeadlineText(double daysLeft);
	// A real duration in words: "1 hour 15 minutes", "40 seconds".
	std::string RealSpanText(double seconds);
}
