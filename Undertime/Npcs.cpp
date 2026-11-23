// UNDERTIME's people and the places they go (Docs/NPCs.md). Placeholders: the first cast,
// written briefly so their days can be seen; their stories are to be written properly.
//
// Places on props are in the prop's model space (Tools/Placeholders.cpp builds the models;
// 0.25 m voxels, pivot at the bottom centre, doors on the -z side). Keep them in step with the
// models: a model's voxel (x, y, z) is at ((x + 0.5) * 0.25 - width / 2, y * 0.25, (z + 0.5) *
// 0.25 - depth / 2) metres, its width and depth in metres.
#include "Npcs.h"

namespace npc
{
	const char* ActivityName(Activity activity)
	{
		static const char* const kNames[] = { "stand", "work", "hammer", "chop", "cook", "sit", "pan", "sleep", "walk" };
		const int i = static_cast<int>(activity);
		return i >= 0 && i < static_cast<int>(Activity::Count) ? kNames[i] : "?";
	}

	const std::vector<ScheduleBlock>& DayFor(const NpcDef& npc, int era)
	{
		static const std::vector<ScheduleBlock> kNone;
		for (const EraDay& d : npc.eraDays)
			if (era >= d.eraFrom && era <= d.eraTo) return d.blocks;
		return era >= npc.eraFrom && era <= npc.eraTo ? npc.day : kNone;
	}

	namespace
	{
		// A plank building from Tools/Placeholders.cpp (BuildingSpec): its voxel (x, y, z) in
		// model space, metres. Door at x = (1 + width) / 2 in the front wall z = porch + 1.
		struct Plank { int width, depth, porch; };
		std::array<float, 3> V(const Plank& b, float x, float y, float z)
		{
			return { (x + 0.5f) * 0.25f - (b.width + 2) * 0.125f, y * 0.25f, (z + 0.5f) * 0.25f - (b.porch + b.depth + 2) * 0.125f };
		}
		// From outside the doorway in: the ground in front, over the porch, just inside.
		std::vector<std::array<float, 3>> Way(const Plank& b)
		{
			const float cx = static_cast<float>((1 + b.width) / 2);   // the generator's whole voxel
			std::vector<std::array<float, 3>> way{ V(b, cx, 0, -2.0f) };
			if (b.porch > 0) way.push_back(V(b, cx, 1, b.porch * 0.5f));
			way.push_back(V(b, cx, 1, b.porch + 3.0f));
			return way;
		}
		const Plank kStore{ 36, 36, 8 }, kOffice{ 24, 26, 6 }, kPost{ 24, 26, 0 }, kSaloon{ 40, 44, 8 }, kSmithy{ 30, 30, 0 }, kHome{ 22, 20, 0 };
	}

	const std::vector<PlaceDef>& Places()
	{
		static const std::vector<PlaceDef> kPlaces = []
		{
			std::vector<PlaceDef> p;
			auto onProp = [&](const char* id, const char* entity, std::array<float, 3> at, float yaw)
			{
				PlaceDef d;
				d.id = id;
				d.entity = entity;
				d.at = at;
				d.yawDegrees = yaw;
				p.push_back(d);
			};
			auto inside = [&](const char* id, const char* entity, std::array<float, 3> at, float yaw, std::vector<std::array<float, 3>> approach)
			{
				PlaceDef d;
				d.id = id;
				d.entity = entity;
				d.at = at;
				d.yawDegrees = yaw;
				d.inside = true;
				d.approach = std::move(approach);
				p.push_back(d);
			};
			auto bank = [&](const char* id, float x, float z)
			{
				PlaceDef d;
				d.id = id;
				d.at = { x, 0.0f, z };
				d.riverBank = true;
				p.push_back(d);
			};

			// The trading post (34 x 26 x 40 voxels): porch at the front, counter across the
			// room with a gap in the middle, barrels by the front wall.
			const std::array<float, 3> postDoor{ 0.0f, 0.0f, -5.4f }, postPorch{ 0.0f, 0.25f, -4.0f }, postIn{ 0.0f, 0.25f, -2.0f };
			inside("post.counter", "Trading Post", { -1.25f, 0.25f, 0.0f }, 180.0f, { postDoor, postPorch, postIn, { -0.125f, 0.25f, -0.75f } });
			inside("post.customer", "Trading Post", { -0.125f, 0.25f, -1.5f }, 0.0f, { postDoor, postPorch, postIn });
			inside("post.barrels", "Trading Post", { 2.3f, 0.25f, -1.9f }, -90.0f, { postDoor, postPorch, postIn });
			inside("post.porch", "Trading Post", { 1.2f, 0.25f, -3.6f }, 180.0f, { { 1.2f, 0.0f, -5.4f } });
			onProp("crates", "Crates", { 0.0f, 0.0f, -1.7f }, 0.0f);
			// The notice board (missions) stands beside the porch, facing the camp.
			onProp("post.board", "Trading Post", { 5.4f, 0.0f, -4.2f }, 180.0f);

			// Round the campfire (stone ring about 1.15 m across the middle).
			onProp("fire.n", "Campfire", { 0.0f, 0.0f, 2.0f }, 180.0f);
			onProp("fire.s", "Campfire", { 0.0f, 0.0f, -2.0f }, 0.0f);
			onProp("fire.w", "Campfire", { -2.0f, 0.0f, 0.0f }, 90.0f);
			onProp("fire.e", "Campfire", { 2.0f, 0.0f, 0.0f }, -90.0f);
			onProp("fire.ne", "Campfire", { 1.45f, 0.0f, 1.45f }, -135.0f);
			onProp("fire.cook", "Campfire", { 1.2f, 0.0f, -1.2f }, -45.0f);

			// Tents (15 x 11 x 18): the flap at the front, room for two lying side by side.
			const std::array<float, 3> flapOut{ 0.0f, 0.0f, -2.6f }, flapIn{ 0.0f, 0.0f, -1.3f };
			inside("tentE.bed", "Tent (east)", { 0.0f, 0.0f, 0.4f }, 0.0f, { flapOut, flapIn });
			inside("tentSW.bed1", "Tent (south-west)", { -0.4f, 0.0f, 0.4f }, 0.0f, { flapOut, flapIn });
			inside("tentSW.bed2", "Tent (south-west)", { 0.4f, 0.0f, 0.4f }, 0.0f, { flapOut, flapIn });
			inside("tentS.bed", "Tent (south)", { 0.0f, 0.0f, 0.4f }, 0.0f, { flapOut, flapIn });
			onProp("tentSW.door", "Tent (south-west)", { 0.6f, 0.0f, -2.4f }, 180.0f);

			// The smith's lean-to (16 x 14 x 14): the anvil on its stump, open at the front.
			onProp("smithy.anvil", "Smithy Lean-to", { 0.0f, 0.0f, -0.95f }, 0.0f);
			// The camp's log pile (14 x 9 x 10): splitting wood in front of it.
			onProp("logpile.chop", "Log Pile", { 0.6f, 0.0f, -2.1f }, 0.0f);

			// The clockmaker's cottage (26 x 32 x 22): the workbench along the back wall, the
			// bed by the left wall, the door in the front.
			const std::array<float, 3> cottageDoor{ 0.0f, 0.0f, -3.2f }, cottageIn{ 0.0f, 0.25f, -1.9f };
			inside("cottage.bench", "Clockmaker Cottage", { 0.0f, 0.25f, 0.95f }, 0.0f, { cottageDoor, cottageIn });
			inside("cottage.bed", "Clockmaker Cottage", { -1.94f, 0.75f, -0.94f }, 0.0f, { cottageDoor, cottageIn, { -1.0f, 0.25f, -0.9f } });
			onProp("cottage.door", "Clockmaker Cottage", { 0.0f, 0.0f, -3.0f }, 180.0f);

			// ---- The Settlement (era 2) ----------------------------------------------------
			// The general store: the counter across the room, a gap in its middle.
			{
				auto way = Way(kStore);
				way.push_back(V(kStore, 18, 1, 16.5f));
				inside("store.counter", "General Store", V(kStore, 13, 1, 19.5f), 180.0f, way);
				inside("store.customer", "General Store", V(kStore, 18, 1, 13), 0.0f, Way(kStore));
			}
			// The assay office and the post office: a desk at the back; a cot behind it.
			inside("assay.desk", "Assay Office", V(kOffice, 6.5f, 1, 29.5f), 180.0f, [] { auto w = Way(kOffice); w.push_back(V(kOffice, 11, 1, 29.5f)); return w; }());
			inside("assay.customer", "Assay Office", V(kOffice, 6.5f, 1, 24), 0.0f, Way(kOffice));
			inside("assay.cot", "Assay Office", V(kOffice, 16, 1, 28), 0.0f, Way(kOffice));
			inside("mail.desk", "Post Office", V(kPost, 6.5f, 1, 23.5f), 180.0f, [] { auto w = Way(kPost); w.push_back(V(kPost, 11, 1, 23.5f)); return w; }());
			inside("mail.customer", "Post Office", V(kPost, 6.5f, 1, 18), 0.0f, Way(kPost));
			inside("mail.cot", "Post Office", V(kPost, 16, 1, 21), 0.0f, Way(kPost));
			// The saloon: the bar along the back (a gap at its left end), tables with stools.
			{
				auto behind = Way(kSaloon);
				behind.push_back(V(kSaloon, 2, 1, 45));
				behind.push_back(V(kSaloon, 2, 1, 49.5f));
				inside("saloon.bar", "Saloon", V(kSaloon, 20, 1, 49.5f), 180.0f, behind);
				inside("saloon.cot", "Saloon", V(kSaloon, 30, 1, 49.5f), 90.0f, behind);
				inside("saloon.front", "Saloon", V(kSaloon, 20, 1, 45.5f), 0.0f, Way(kSaloon));
				const char* const seats[] = { "saloon.t1a", "saloon.t1b", "saloon.t2a", "saloon.t2b", "saloon.t3a", "saloon.t3b", "saloon.t4a", "saloon.t4b",
					"saloon.t5a", "saloon.t5b", "saloon.t6a", "saloon.t6b" };
				for (int i = 0; i < 12; ++i)
				{
					const int table = i / 2, row = table / 4, col = table % 4;
					const float tx = 5.0f + col * 9.0f, tz = 13.0f + row * 7.0f;
					const bool right = i % 2 == 1;
					// Sitting on the stool (its top 0.75 m up), facing the table.
					inside(seats[i], "Saloon", V(kSaloon, right ? tx + 4 : tx - 1, 3, tz + 1), right ? -90.0f : 90.0f, Way(kSaloon));
				}
			}
			onProp("saloon.board", "Saloon", V(kSaloon, 30, 0, -2.5f), 180.0f);
			// The smithy: the anvil in the middle, the forge at the back.
			inside("smithy2.anvil", "Blacksmith", V(kSmithy, 15, 1, 9), 0.0f, Way(kSmithy));
			// Homes: a bed by the left wall (two can share it), a table with two stools, a stove.
			for (const char* home : { "widow", "family", "doctor" })
			{
				const std::string entity = std::string("Home (") + home + ")";
				static std::vector<std::string> names;   // the PlaceDef keeps const char*
				auto name = [&](const char* what) { names.push_back(std::string(home) + "." + what); return names.back().c_str(); };
				names.reserve(64);
				static std::vector<std::string> entities;
				entities.reserve(8);
				entities.push_back(entity);
				const char* e = entities.back().c_str();
				const auto way = Way(kHome);
				inside(name("bed1"), e, V(kHome, 3.6f, 3, 14.5f), 0.0f, way);
				inside(name("bed2"), e, V(kHome, 5.4f, 3, 14.5f), 0.0f, way);
				inside(name("floor"), e, V(kHome, 16, 1, 6), 90.0f, way);
				inside(name("stool1"), e, V(kHome, 9, 3, 7), 90.0f, way);
				inside(name("stool2"), e, V(kHome, 14, 3, 7), -90.0f, way);
				inside(name("stove"), e, V(kHome, 19, 1, 14.5f), 0.0f, way);
				onProp(name("door"), e, V(kHome, 11, 0, -3.0f), 180.0f);
			}
			// The square: around the clock tower's scaffolding (6 m across).
			onProp("square.play1", "Clock Tower (scaffolding)", { 5.0f, 0.0f, 1.5f }, -90.0f);
			onProp("square.play2", "Clock Tower (scaffolding)", { -1.5f, 0.0f, -5.0f }, 0.0f);

			// On the river.
			bank("wheel.bank", 101.5f, -124.5f);   // by the clockmaker's water wheel
			bank("river.pan", 50.0f, -170.0f);     // Tom's panning spot, below the camp
			bank("river.wash", 42.0f, -178.0f);    // where Mae does the washing
			return p;
		}();
		return kPlaces;
	}

	const PlaceDef* FindPlace(const std::string& id)
	{
		for (const PlaceDef& place : Places())
			if (id == place.id) return &place;
		return nullptr;
	}

	int CastIndex(const std::string& id)
	{
		const std::vector<NpcDef>& cast = Cast();
		for (size_t i = 0; i < cast.size(); ++i)
			if (id == cast[i].id) return static_cast<int>(i);
		return -1;
	}

	std::string PartModel(const NpcDef& npc, const char* part)
	{
		return std::string("Models/Npcs/") + npc.id + "/" + part + ".vox";
	}

	const std::vector<NpcDef>& Cast()
	{
		static const std::vector<NpcDef> kCast = []
		{
			using A = Activity;
			std::vector<NpcDef> cast;
			// Placeholder lives (Docs/NPCs.md §2.1): Camp 0-2 years, Settlement 2-10.
			// The Settlement's people have their own days (eraDays); later eras have no town yet.

			{
				NpcDef n;
				n.id = "agnes";
				n.name = "Agnes Rook";
				n.role = "Keeper of the trading post";
				n.story = "Came up the pass with two mules and a debt. Runs the camp's only shop, marries Bram, and keeps the general store "
					"when the town comes. (Placeholder: her story is still to be written.)";
				n.traits = { 0.45f, 0.7f, 0.6f, 0.3f, 0.55f };
				n.look.skin = 0xe0b090; n.look.hair = 0x9a968a; n.look.longHair = true;
				n.look.shirt = 0x6a4a6a; n.look.apron = 0xd8c8a0; n.look.trousers = 0x3a3040; n.look.height = 0.95f;
				n.eraFrom = 1; n.eraTo = 1;
				n.born = -46.0; n.arrives = -0.5; n.dies = 24.0;
				n.spouse = "bram"; n.marries = 1.5;
				n.roles = { { 2.0, "Keeper of the general store" } };
				n.epitaph = "She knew what everything was worth.";
				n.day = {
					{ 0.0f, "tentE.bed", A::Sleep },
					{ 5.5f, "crates", A::Work, "post.counter", A::Work },
					{ 7.0f, "post.counter", A::Work },
					{ 12.0f, "fire.n", A::Sit, "post.counter", A::Work },
					{ 13.0f, "post.counter", A::Work },
					{ 18.5f, "fire.ne", A::Sit, "post.counter", A::Stand },
					{ 21.5f, "tentE.bed", A::Sleep },
				};
				n.eraDays = { { 2, 2, {
					{ 0.0f, "widow.bed1", A::Sleep },
					{ 6.0f, "store.counter", A::Work },
					{ 12.0f, "saloon.t1a", A::Sit },
					{ 13.5f, "store.counter", A::Work },
					{ 18.5f, "saloon.t1b", A::Sit },
					{ 21.5f, "widow.bed1", A::Sleep },
				} } };
				n.ties = { { "tom", "customer" }, { "bram", "husband" } };
				n.voice = "formal";
				n.likes = { "Silver", "Gold" };
				n.dislikes = { "Stone" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "tom";
				n.name = "Tom Harlan";
				n.role = "Prospector";
				n.story = "Followed a rumour of gold up the river. Pans every day and sells the dust; certain the big strike is one "
					"bend away. The river takes him in the end. (Placeholder.)";
				n.traits = { 0.7f, 0.65f, 0.3f, 0.4f, 0.6f };
				n.look.skin = 0xc89070; n.look.hair = 0x6a4020; n.look.beard = true; n.look.hat = 0x6a5030;
				n.look.shirt = 0x8a3a2a; n.look.trousers = 0x3a4a6a; n.look.height = 1.04f;
				n.eraFrom = 1; n.eraTo = 1;
				n.born = -31.0; n.arrives = -0.3; n.dies = 9.0;
				n.spouse = "mae"; n.marries = -6.0;
				n.epitaph = "One more bend.";
				n.day = {
					{ 0.0f, "tentSW.bed1", A::Sleep },
					{ 4.5f, "fire.w", A::Sit, "post.customer", A::Stand },
					{ 6.0f, "river.pan", A::Pan, "post.customer", A::Stand },
					{ 16.0f, "post.customer", A::Stand },
					{ 19.0f, "fire.w", A::Sit, "post.customer", A::Stand },
					{ 22.0f, "tentSW.bed1", A::Sleep },
				};
				n.eraDays = { { 2, 2, {
					{ 0.0f, "family.bed1", A::Sleep },
					{ 4.5f, "family.stool1", A::Sit },
					{ 6.0f, "river.pan", A::Pan, "saloon.t2a", A::Sit },
					{ 15.5f, "assay.customer", A::Stand },
					{ 19.5f, "saloon.t2a", A::Sit },
					{ 22.0f, "family.bed1", A::Sleep },
				} } };
				n.ties = { { "mae", "wife" }, { "agnes", "buys his gold" }, { "ella", "daughter" } };
				n.voice = "chatty";
				n.likes = { "Gold" };
				n.dislikes = { "Stone" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "mae";
				n.name = "Mae Harlan";
				n.role = "Cook";
				n.story = "Keeps the camp fed and Tom out of trouble. Thinks the mountain should be left alone. Raises Ella in the "
					"white house. (Placeholder.)";
				n.traits = { 0.8f, 0.2f, 0.7f, 0.75f, 0.4f };
				n.look.skin = 0xe8b898; n.look.hair = 0xa0522d; n.look.longHair = true;
				n.look.shirt = 0x5a7a5a; n.look.apron = 0xe8e0d0; n.look.trousers = 0x4a3a2a; n.look.height = 0.93f;
				n.eraFrom = 1; n.eraTo = 1;
				n.born = -28.0; n.arrives = -0.3; n.dies = 41.0;
				n.spouse = "tom"; n.marries = -6.0;
				n.epitaph = "Some things are meant to stay where they're put.";
				n.day = {
					{ 0.0f, "tentSW.bed2", A::Sleep },
					{ 5.0f, "fire.cook", A::Cook, "tentSW.door", A::Cook },
					{ 8.5f, "river.wash", A::Work, "tentSW.door", A::Stand },
					{ 14.0f, "fire.cook", A::Cook, "tentSW.door", A::Cook },
					{ 18.0f, "fire.e", A::Sit, "tentSW.door", A::Stand },
					{ 22.0f, "tentSW.bed2", A::Sleep },
				};
				n.eraDays = { { 2, 2, {
					{ 0.0f, "family.bed2", A::Sleep },
					{ 5.0f, "family.stove", A::Cook },
					{ 8.5f, "river.wash", A::Work, "family.stove", A::Work },
					{ 14.0f, "family.stove", A::Cook },
					{ 18.0f, "saloon.t2b", A::Sit, "family.stool2", A::Sit },
					{ 22.0f, "family.bed2", A::Sleep },
				} } };
				n.ties = { { "tom", "husband" }, { "ella", "daughter" } };
				n.voice = "chatty";
				n.likes = { "Copper" };   // a pot that doesn't leak
				n.dislikes = { "Time crystal", "Strange watch" };   // things the mountain should keep
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "bram";
				n.name = "Bram Kettle";
				n.role = "Smith";
				n.story = "Mends picks and shoes mules under a lean-to; marries Agnes; his lungs go bad from the forge. A remedy "
					"in time would give him years. (Placeholder.)";
				n.traits = { 0.4f, 0.4f, 0.8f, 0.5f, 0.3f };
				n.look.skin = 0xb07850; n.look.hair = 0x2a2018; n.look.beard = true;
				n.look.shirt = 0x5a5048; n.look.apron = 0x4a3424; n.look.trousers = 0x3a3530; n.look.height = 1.08f;
				n.eraFrom = 1; n.eraTo = 1;
				n.born = -44.0; n.arrives = -0.5; n.dies = 8.5;
				n.hooks = { { "bram.saved", 31.0 } };   // Dr Calloway's remedy (doc_bram)
				n.spouse = "agnes"; n.marries = 1.5;
				n.epitaph = "Iron's patient.";
				n.day = {
					{ 0.0f, "tentS.bed", A::Sleep },
					{ 5.0f, "smithy.anvil", A::Hammer },
					{ 12.0f, "fire.s", A::Sit, "post.barrels", A::Stand },
					{ 13.5f, "smithy.anvil", A::Hammer },
					{ 17.0f, "logpile.chop", A::Chop, "smithy.anvil", A::Work },
					{ 19.0f, "fire.n", A::Sit, "post.barrels", A::Stand },
					{ 23.0f, "tentS.bed", A::Sleep },
				};
				n.eraDays = { { 2, 2, {
					{ 0.0f, "widow.bed2", A::Sleep },
					{ 5.5f, "smithy2.anvil", A::Hammer },
					{ 12.0f, "saloon.t1b", A::Sit },
					{ 13.5f, "smithy2.anvil", A::Hammer },
					{ 18.5f, "saloon.t3a", A::Sit },
					{ 22.0f, "widow.bed2", A::Sleep },
				} } };
				n.ties = { { "agnes", "wife" } };
				n.voice = "terse";
				n.likes = { "Copper", "Silver" };
				n.dislikes = { "Strange watch" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "clockmaker";
				n.name = "The Clockmaker";
				n.role = "Clockmaker";
				n.story = "Was here before the camp and will be here after the town. Never ages, never says where they came from, "
					"and remembers every timeline you undo. (Placeholder.)";
				n.traits = { 0.6f, 0.1f, 1.0f, 0.5f, 0.9f };
				n.look.skin = 0xd8b8a0; n.look.hair = 0xe8e8e4; n.look.hat = 0x2a2a34;
				n.look.shirt = 0x3a4a5a; n.look.trousers = 0x2a2a30; n.look.boots = 0x1a1a1e; n.look.height = 0.98f;
				n.eraFrom = 1; n.eraTo = 6;
				n.born = -1000.0; n.arrives = -500.0;
				n.ageless = true;
				n.voice = "formal";
				n.likes = { "Time crystal", "Strange watch" };
				n.day = {
					{ 0.0f, "cottage.bed", A::Sleep },
					{ 6.0f, "cottage.bench", A::Work },
					{ 12.0f, "cottage.door", A::Stand, "cottage.bench", A::Work },
					{ 13.0f, "cottage.bench", A::Work },
					{ 18.0f, "wheel.bank", A::Stand, "cottage.bench", A::Work },
					{ 20.0f, "cottage.bench", A::Work },
					{ 23.0f, "cottage.bed", A::Sleep },
				};
				cast.push_back(n);
			}

			// ---- Arriving with the Settlement (and born in it) ---------------------------------------
			{
				NpcDef n;
				n.id = "ella";
				n.name = "Ella Harlan";
				n.role = "Tom and Mae's daughter";
				n.story = "Born in the white house in the Settlement's second year. Grows up hearing about the courier who never gets "
					"older. (Placeholder.)";
				n.traits = { 0.75f, 0.35f, 0.4f, 0.55f, 0.9f };
				n.look.skin = 0xe8b898; n.look.hair = 0xb0602d; n.look.longHair = true;
				n.look.shirt = 0x8a6a9a; n.look.trousers = 0x4a3a4a; n.look.height = 0.95f;
				n.eraFrom = 0; n.eraTo = 0;
				n.born = 3.0; n.arrives = 3.0;
				n.parents = { "tom", "mae" };
				n.eraDays = { { 2, 2, {
					{ 0.0f, "family.floor", A::Sleep },
					{ 7.0f, "family.stool2", A::Sit },
					{ 9.0f, "square.play1", A::Stand, "family.stool2", A::Sit },
					{ 12.0f, "family.stool2", A::Sit },
					{ 14.0f, "square.play2", A::Stand, "family.floor", A::Sit },
					{ 18.0f, "family.stool1", A::Sit },
					{ 20.0f, "family.floor", A::Sleep },
				} } };
				n.ties = { { "tom", "father" }, { "mae", "mother" } };
				n.voice = "chatty";
				n.likes = { "Silver" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "silas";
				n.name = "Silas Merrow";
				n.role = "Assayer";
				n.story = "Came over the pass the last summer it was open, with scales and a ledger. Weighs everything twice and pays "
					"for it once. (Placeholder.)";
				n.traits = { 0.25f, 0.9f, 0.5f, 0.15f, 0.5f };
				n.look.skin = 0xd8b090; n.look.hair = 0x3a3a3a; n.look.hat = 0x1e1e22;
				n.look.shirt = 0x4a4a52; n.look.trousers = 0x2a2a30; n.look.height = 1.0f;
				n.eraFrom = 0; n.eraTo = 0;
				n.born = -40.0; n.arrives = 2.0; n.dies = 18.0;
				n.epitaph = "Paid in full.";
				n.eraDays = { { 2, 2, {
					{ 0.0f, "assay.cot", A::Sleep },
					{ 7.0f, "assay.desk", A::Work },
					{ 12.0f, "saloon.t3b", A::Sit },
					{ 14.0f, "assay.desk", A::Work },
					{ 19.0f, "saloon.front", A::Stand },
					{ 22.0f, "assay.cot", A::Sleep },
				} } };
				n.ties = { { "tom", "buys his gold" }, { "jonah", "creditor" } };
				n.voice = "formal";
				n.likes = { "Gold", "Silver" };
				n.dislikes = { "Stone", "Copper" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "hattie";
				n.name = "Hattie Pell";
				n.role = "Postmistress";
				n.story = "Runs a post office for a valley no post reaches, and keeps every letter anyway. Writes to you while you're "
					"down there. (Placeholder.)";
				n.traits = { 0.85f, 0.3f, 0.8f, 0.5f, 0.85f };
				n.look.skin = 0xe0b8a0; n.look.hair = 0x5a3a2a; n.look.longHair = true;
				n.look.shirt = 0x3a5a7a; n.look.trousers = 0x2a3040; n.look.height = 0.92f;
				n.eraFrom = 0; n.eraTo = 0;
				n.born = -24.0; n.arrives = 2.2; n.dies = 52.0;
				n.epitaph = "Every letter delivered.";
				n.eraDays = { { 2, 2, {
					{ 0.0f, "mail.cot", A::Sleep },
					{ 7.0f, "mail.desk", A::Work },
					{ 12.0f, "saloon.t4a", A::Sit },
					{ 14.0f, "mail.desk", A::Work },
					{ 18.0f, "saloon.t4b", A::Sit },
					{ 21.0f, "mail.cot", A::Sleep },
				} } };
				n.ties = { { "agnes", "friend" }, { "ruth", "friend" } };
				n.voice = "chatty";
				n.likes = { "Silver", "Copper" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "jonah";
				n.name = "Jonah Quill";
				n.role = "Saloon keeper";
				n.story = "Built the saloon before anyone could drink in it. Hears every rumour in the valley, including the ones "
					"about the watches. (Placeholder.)";
				n.traits = { 0.6f, 0.6f, 0.5f, 0.6f, 0.7f };
				n.look.skin = 0xc08868; n.look.hair = 0x1a1410; n.look.beard = true;
				n.look.shirt = 0xd8d0c0; n.look.apron = 0x2a2a2a; n.look.trousers = 0x30282a; n.look.height = 1.02f;
				n.eraFrom = 0; n.eraTo = 0;
				n.born = -35.0; n.arrives = 2.0; n.dies = 33.0;
				n.epitaph = "Last orders.";
				n.eraDays = { { 2, 2, {
					{ 0.0f, "saloon.bar", A::Work },
					{ 1.0f, "saloon.cot", A::Sleep },
					{ 9.0f, "saloon.bar", A::Work },
				} } };
				n.ties = { { "silas", "owes him" } };
				n.voice = "rough";
				n.likes = { "Gold" };
				cast.push_back(n);
			}
			{
				NpcDef n;
				n.id = "ruth";
				n.name = "Dr Ruth Calloway";
				n.role = "Doctor";
				n.story = "The only doctor in a valley that keeps its dead. Treats Bram's lungs with whatever the mountain gives her. "
					"(Placeholder.)";
				n.traits = { 0.65f, 0.25f, 0.7f, 0.2f, 0.8f };
				n.look.skin = 0x8a5a3a; n.look.hair = 0x1a1410; n.look.longHair = true;
				n.look.shirt = 0xe0e0d8; n.look.trousers = 0x2a2a3a; n.look.height = 0.97f;
				n.eraFrom = 0; n.eraTo = 0;
				n.born = -33.0; n.arrives = 3.0; n.dies = 47.0;
				n.epitaph = "She kept them as long as she could.";
				n.eraDays = { { 2, 2, {
					{ 0.0f, "doctor.bed1", A::Sleep },
					{ 7.0f, "doctor.stool1", A::Sit },
					{ 9.0f, "family.door", A::Stand },
					{ 11.0f, "widow.door", A::Stand },
					{ 13.0f, "saloon.t3a", A::Sit },
					{ 15.0f, "doctor.stool2", A::Work },
					{ 22.0f, "doctor.bed1", A::Sleep },
				} } };
				n.ties = { { "hattie", "friend" }, { "bram", "patient" } };
				n.voice = "formal";
				n.likes = { "Silver", "Time crystal" };
				cast.push_back(n);
			}
			return cast;
		}();
		return kCast;
	}
}
