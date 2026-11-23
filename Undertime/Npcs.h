#pragma once
// UNDERTIME's people (Docs/NPCs.md). Every NPC is written by hand with their own story; this
// is that data: who they are, what they're like, how their placeholder model looks, and how
// they spend a day in the eras they live in the valley. Pure C++ (tested in UndertimeTests,
// and read by the placeholder tool, which writes their models).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace npc
{
	// What someone is doing where they are: the puppet's pose and motion (NpcWorld).
	enum class Activity : uint8_t
	{
		Stand,    // idle, looking about
		Work,     // hands busy in front (a counter, a bench, a washtub)
		Hammer,   // one arm striking (the anvil)
		Chop,     // both arms over the head and down (splitting wood)
		Cook,     // stirring a pot
		Sit,      // on the ground (by the fire)
		Pan,      // sitting at the water, swirling a gold pan
		Sleep,    // lying down (the place is the body's middle, facing where the head points)
		Walk,     // between places (never in a schedule: Evaluate reports it)
		Count
	};
	const char* ActivityName(Activity activity);

	// Five sliders, 0..1 (Docs/NPCs.md §3). Dialog and regard use them from NPC milestone 2.
	struct Traits
	{
		float warmth = 0.5f;
		float greed = 0.5f;
		float patience = 0.5f;
		float superstition = 0.5f;
		float curiosity = 0.5f;
	};

	// How the placeholder model looks (sRGB 0xRRGGBB). Tools/Placeholders.cpp writes it as one
	// .vox per body part into Content/Models/Npcs/<id>/; hand-made models replace those files.
	struct Look
	{
		uint32_t skin = 0xd9a47e;
		uint32_t hair = 0x4a3020;
		uint32_t shirt = 0x8a6a4a;
		uint32_t trousers = 0x4a4038;
		uint32_t boots = 0x2e2420;
		uint32_t hat = 0;          // 0: none
		uint32_t apron = 0;        // 0: none
		bool beard = false;
		bool longHair = false;
		float height = 1.0f;       // scale of the whole figure
	};

	// A named spot people go to (Docs/NPCs.md §2.2).
	struct PlaceDef
	{
		const char* id = "";
		// On a scene entity (by its EntityInfo name; a ValleyProp): `at` is in the prop's model
		// space (metres from its pivot, the bottom centre; the placeholder buildings' doors are
		// on their -z side). Without `entity`, `at` is a world x and z on the ground (y unused).
		const char* entity = nullptr;
		std::array<float, 3> at{};
		float yawDegrees = 0.0f;   // facing (0 = +z), in the prop's space or the world's
		// Inside the model: `at` keeps its height above the prop (a floor), and routes in and
		// out go through `approach`, from outside the doorway inwards (prop space).
		bool inside = false;
		std::vector<std::array<float, 3>> approach;
		// A world spot moved to the dry bank of the river nearest to it, facing the water.
		bool riverBank = false;
	};

	struct ScheduleBlock
	{
		float hour = 0.0f;                         // starts at this hour (0..24), until the next block
		const char* place = "";
		Activity activity = Activity::Stand;
		const char* rainPlace = nullptr;           // when it rains as the block starts: here instead...
		Activity rainActivity = Activity::Stand;   // ... doing this
	};

	// Their life (Docs/NPCs.md §2.1), in years since you arrived (negative: before). A hook moves
	// a date when a story flag was set before it (a mission that saves a life).
	struct LifeHook
	{
		const char* flag;
		double dies = -1.0;          // the new date of death (-1: unchanged)
		double leaves = -1.0;        // the new date they leave (-1: unchanged)
	};
	struct RoleChange
	{
		double year;
		const char* role;
	};
	// Their day in some eras (NpcDef::day covers eraFrom..eraTo).
	struct EraDay
	{
		int eraFrom = 1, eraTo = 6;
		std::vector<ScheduleBlock> blocks;
	};

	struct Tie
	{
		const char* id;        // another NPC
		const char* relation;  // "wife", "friend", "rival", ...
	};

	struct NpcDef
	{
		const char* id = "";        // code name; their models are in Content/Models/Npcs/<id>/
		const char* name = "";
		const char* role = "";
		const char* story = "";     // a placeholder outline, to be written properly
		Traits traits;
		Look look;
		int eraFrom = 1, eraTo = 1; // the eras they live in the valley (1 Camp .. 6 Gone)
		std::vector<ScheduleBlock> day;   // their day, blocks in hour order, the first at 0:00
		std::vector<Tie> ties;
		bool ageless = false;       // the clockmaker: never ages, and remembers you across turning time back
		// How they talk: picks their version of shared barks ("terse", "chatty", "formal", "rough").
		const char* voice = "chatty";
		// Gifts they like and dislike, by backpack item name (cfg::kBlocks: "Copper", "Gold", ...).
		std::vector<const char*> likes;
		std::vector<const char*> dislikes;

		// Their life: born, in the valley from `arrives` until they leave or die. Years since you arrived.
		double born = -30.0;
		double arrives = -1.0;
		double leaves = 1e9;
		double dies = 1e9;
		std::vector<LifeHook> hooks;
		std::vector<RoleChange> roles;        // what they do from then on (else `role`)
		const char* spouse = nullptr;         // married on `marries` (both of the pair say so)
		double marries = -1.0;
		std::vector<const char*> parents;     // children start with a share of their parents' friendship
		const char* epitaph = nullptr;        // on their grave
		std::vector<EraDay> eraDays;          // their day in later eras
	};

	// Their day in `era` (empty: no life in the valley in that era's town).
	const std::vector<ScheduleBlock>& DayFor(const NpcDef& npc, int era);

	const std::vector<NpcDef>& Cast();
	int CastIndex(const std::string& id);   // -1 if there's no such person
	const std::vector<PlaceDef>& Places();
	const PlaceDef* FindPlace(const std::string& id);   // null if there's none
	// "Models/Npcs/<id>/<part>.vox", part: Head, Torso, Arm, Leg.
	std::string PartModel(const NpcDef& npc, const char* part);
	constexpr const char* kParts[4] = { "Head", "Torso", "Arm", "Leg" };
}
