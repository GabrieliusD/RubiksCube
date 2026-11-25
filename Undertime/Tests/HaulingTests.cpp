// Tests for money, shops and the workbench (Economy.h), and later the claim, farming, wood and
// machines (Docs/Hauling.md).
#include "../Economy.h"
#include "../Building.h"
#include "../Growth.h"
#include "../Clock.h"
#include "../Config.h"
#include <cstdio>
#include <string>

namespace
{
	int gHaulFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gHaulFailures;
	}

	void EconomyTests()
	{
		using namespace econ;
		Check(Money(0) == "$0.00" && Money(5) == "$0.05" && Money(1234) == "$12.34" && Money(-250) == "-$2.50", "money: cents shown as dollars");
		Check(BlockNamed("Copper") == cfg::Block::Copper && BlockNamed("timber") == cfg::Block::Air, "goods: block names are blocks, ids are items");

		// Every offer names a real item, sold in an era range that makes sense, by a real shop.
		bool offersOk = true;
		for (const Offer& o : Offers())
			offersOk = offersOk && FindItem(o.item) && o.eraFrom >= 1 && o.eraTo <= 6 && o.eraFrom <= o.eraTo && o.shop != Shop::Count && OfferCents(o) > 0;
		Check(offersOk, "shops: every offer is a known item with a price and an era range");
		bool needsOk = true;
		for (const Offer& o : Offers())
			for (const auto& [id, n] : o.needs) needsOk = needsOk && n > 0 && (FindItem(id) || BlockNamed(id) != cfg::Block::Air);
		for (const Recipe& r : Recipes())
		{
			needsOk = needsOk && FindItem(r.item) && r.makes > 0;
			for (const auto& [id, n] : r.needs) needsOk = needsOk && n > 0 && (FindItem(id) || BlockNamed(id) != cfg::Block::Air);
		}
		Check(needsOk, "shops and the bench: every material named is a block or an item");

		// Ore by era; stone only in the yard; the watches aren't for sale.
		const int copperCamp = BuyPrice(Shop::TradingPost, "Copper", 1);
		Check(copperCamp == cfg::Info(cfg::Block::Copper).basePrice * 100, "selling: copper at the trading post in Camp is its base price");
		Check(BuyPrice(Shop::TradingPost, "Copper", 3) > copperCamp && BuyPrice(Shop::TradingPost, "Copper", 5) < copperCamp, "selling: ore pays more in the Boomtown, less once the town's abandoned");
		Check(BuyPrice(Shop::Assayer, "Gold", 2) > BuyPrice(Shop::GeneralStore, "Gold", 2), "selling: the assayer pays a little more");
		Check(BuyPrice(Shop::TradingPost, "Stone", 1) == 0 && YardCents(1) > 0 && YardCents(1) > YardCents(4), "selling: stone goes to the yard, and pays less once the town builds in brick");
		Check(BuyPrice(Shop::TradingPost, "Strange watch", 1) == 0, "selling: nobody buys a strange watch");
		Check(BuyPrice(Shop::Smith, "Copper", 1) == 0, "selling: the smith doesn't buy ore");
		Check(BuyPrice(Shop::Kitchen, "potato", 1) > 0 && BuyPrice(Shop::Kitchen, "timber", 1) == 0, "selling: the kitchen buys crops, not timber");
		Check(BuyPrice(Shop::TradingPost, "copper_pick", 1) == 0, "selling: tools and upgrades aren't bought back");

		// Who keeps which shop, by role.
		Check(ShopForRole("Keeper of the trading post") == Shop::TradingPost && ShopForRole("Keeper of the general store") == Shop::GeneralStore &&
			ShopForRole("Smith") == Shop::Smith && ShopForRole("Prospector") == Shop::Count, "shops: kept by the people with those roles");
		Check(!OffersAt(Shop::TradingPost, 1).empty() && !OffersAt(Shop::Smith, 1).empty() && OffersAt(Shop::Smith, 1).size() < OffersAt(Shop::Smith, 3).size(),
			"shops: the smith makes more as the eras go on");

		// Buying: money and materials both count; what's missing is said.
		const Offer* pick = nullptr;
		for (const Offer* o : OffersAt(Shop::Smith, 1)) if (std::string(o->item) == "copper_pick") pick = o;
		Check(pick != nullptr, "shops: the smith sells a copper pick in Camp");
		if (pick)
		{
			auto none = [](const std::string&) { return 0; };
			auto plenty = [](const std::string&) { return 1000; };
			const Verdict poor = CanBuy(*pick, 100, none);
			Check(!poor.ok && poor.missing.find("Copper") != std::string::npos && poor.missing.find("$") != std::string::npos, "buying: short of money and copper, both are named");
			Check(CanBuy(*pick, OfferCents(*pick), plenty).ok, "buying: with the money and the materials, it's yours");
		}
		Purse p;
		p.Add("timber", 3);
		p.Add("timber", -5);
		Check(p.Count("timber") == 0 && p.goods.empty(), "purse: goods never go below none");
	}

	build::Piece Make(const char* id, int x, int y, int z, int turn = 0, bool built = true)
	{
		build::Piece p;
		p.type = build::PieceIndex(id);
		p.x = x; p.y = y; p.z = z; p.turn = turn;
		p.swings = built ? p.Def().swings : 0;
		return p;
	}

	void BuildingTests()
	{
		using namespace build;
		bool known = true;
		for (const PieceDef& d : Pieces())
			for (const auto& [id, n] : d.cost) known = known && n > 0 && (econ::FindItem(id) || econ::BlockNamed(id) != cfg::Block::Air);
		Check(known, "building: every piece costs blocks or items that exist");
		const PieceDef& wall = Pieces()[PieceIndex("stone_wall")];
		int stone = 0;
		for (int n = 0; n < wall.swings; ++n) for (const auto& [id, c] : SwingCost(wall, n)) stone += c;
		Check(stone == 16, "building: a stone wall takes exactly the 16 blocks it's made of, over its swings");
		Check(PaidFor(wall, wall.swings).size() == 1 && PaidFor(wall, wall.swings)[0].second == 16 && PaidFor(wall, 0).empty(), "building: taking it down gives back what was paid so far");

		Plan plan(8, 8, 4);
		Check(plan.CanPlace(Make("plank_wall", 2, 0, 2)).empty(), "building: a wall stands on the ground");
		Check(!plan.CanPlace(Make("plank_wall", 2, 1, 2)).empty(), "building: a wall in mid-air is held up by nothing");
		Check(!plan.CanPlace(Make("plank_floor", 9, 0, 2)).empty(), "building: nothing outside the claim");
		plan.Place(Make("plank_wall", 2, 0, 2, 0));
		Check(!plan.CanPlace(Make("stone_wall", 2, 0, 1, 2)).empty(), "building: the same edge from the next cell is taken");
		Check(plan.CanPlace(Make("plank_wall", 2, 1, 2, 0)).empty(), "building: walls stack");
		const int upper = plan.Place(Make("plank_wall", 2, 1, 2, 0));
		Check(plan.CanPlace(Make("plank_floor", 2, 2, 2)).empty() && plan.CanPlace(Make("plank_floor", 2, 2, 1)).empty(), "building: a floor rests on a wall below its edge, either side");
		const int floor = plan.Place(Make("plank_floor", 2, 2, 2));
		Check(plan.CanPlace(Make("plank_floor", 3, 2, 2)).empty(), "building: a floor hangs out one cell");
		plan.Place(Make("plank_floor", 3, 2, 2));
		Check(plan.CanPlace(Make("plank_floor", 4, 2, 2)).empty(), "building: ... and two");
		plan.Place(Make("plank_floor", 4, 2, 2));
		Check(!plan.CanPlace(Make("plank_floor", 5, 2, 2)).empty(), "building: but not three");
		Check(!plan.CanRemove(upper).empty(), "building: a wall holding a floor can't be taken down");
		Check(plan.CanRemove(floor + 2).empty(), "building: the end of the overhang can");
		Check(plan.AllSupported(), "building: everything placed is held up");
		Check(plan.CanPlace(Make("workbench", 5, 0, 5)).empty() && !plan.CanPlace(Make("workbench", 5, 1, 5)).empty(), "building: furniture on the ground, not in the air");
		plan.Place(Make("workbench", 5, 0, 5));
		Check(!plan.CanPlace(Make("plank_floor", 5, 1, 5)).empty(), "building: furniture holds nothing up");
		// The model: a built wall's voxels are where PieceAt finds it.
		Plan one(4, 4, 2);
		one.Place(Make("stone_wall", 1, 0, 1, 1));
		const VoxelModel m = one.Model(false);
		const std::array<int, 3> o = Plan::PieceOrigin(one.Pieces()[0]);
		Check(m.At(o[0], o[1], o[2] + 1) != 0 && one.PieceAt(o[0], o[1], o[2] + 1) == 0, "building: a wall's voxels are where it stands, and found there");
		Check(Plan::PieceModel(one.Pieces()[0]).SolidCount() == 16, "building: a stone wall is 16 blocks of model");
		Plan site(4, 4, 2);
		site.Place(Make("stone_wall", 1, 0, 1, 0, false));
		Check(site.Model(false).SolidCount() == 0 && site.Model(true).SolidCount() == 0, "building: a site with no swings yet shows nothing");
		site.Pieces()[0].swings = 3;
		Check(site.Model(true).SolidCount() > 0 && site.Model(true).SolidCount() < 16, "building: a half-built site shows its lower part");
	}

	void GrowthTests()
	{
		Check(grow::TreeGrowth(0.0) == 0.0f && grow::TreeGrowth(cfg::kStumpDays + 1.0) > 0.0f && grow::TreeGrowth(cfg::kRegrowDays + 1.0) == 1.0f,
			"trees: a stump, then a sapling, then grown back");
		Check(grow::TreeGrowth(-5.0) == 1.0f, "trees: time turned back past the felling, the tree stands");
		const int potato = grow::CropIndex("potato_seed");
		Check(potato >= 0 && grow::CropIndex("timber") < 0, "crops: seeds are crops, other things aren't");
		Check(grow::CropAt(potato, 1.0, 100).stage == grow::CropStage::Growing && grow::CropAt(potato, 3.5, 100).stage == grow::CropStage::Ripe &&
			grow::CropAt(potato, 8.0, 100).stage == grow::CropStage::Spoiled, "crops: potatoes grow, ripen in 3 days, go to seed after 4 more");
		Check(std::abs(grow::CropAt(potato, 1.0, 100).ripeIn - 2.0) < 1e-9, "crops: says how long until ripe");
		const int apple = grow::CropIndex("apple_sapling");
		Check(grow::CropAt(apple, 400.0, 260).stage == grow::CropStage::Growing && grow::CropAt(apple, 4.0 * 365.0, 260).stage == grow::CropStage::Ripe &&
			grow::CropAt(apple, 4.0 * 365.0, 100).stage == grow::CropStage::Growing, "crops: an apple tree fruits every autumn once it's three years old");
		// One zone-1 dive of 9 real minutes is 3 outside days: just right for potatoes.
		const double zone1Days = 9.0 * 8.0 / 24.0;
		Check(grow::CropAt(potato, zone1Days + 0.1, 100).stage == grow::CropStage::Ripe, "crops: a 9 minute dive in zone 1 brings you up to ripe potatoes");
	}

	void ProjectAndMachineTests()
	{
		bool ok = true;
		for (const econ::Project& p : econ::Projects()) ok = ok && p.era >= 1 && p.era <= 5 && p.stone > 0 && p.rewardCents > 0;
		Check(ok && !econ::Projects().empty(), "town projects: each era's projects need stone and pay something");
		// Machines work in their own time: a drill in zone 1 while you're in zone 3 runs as many
		// times faster than you as zone 3's rate is over zone 1's.
		const std::vector<float> zones{ 10.0f, 40.0f, 70.0f, 100.0f, 130.0f };
		const float you = OutsideClock::Rate(true, 75.0f, zones), drill = OutsideClock::Rate(true, 20.0f, zones);
		Check(you / drill > 15.0f && you / drill < 30.0f, "machines: a zone 1 drill works ~21 times your pace while you're in zone 3");
		Check(OutsideClock::Rate(false, 0.0f, zones) < drill, "machines: seen from the cave, surface machines run fastest (the surface rate is the slowest)");
	}
}

int HaulingTests()
{
	gHaulFailures = 0;
	EconomyTests();
	BuildingTests();
	GrowthTests();
	ProjectAndMachineTests();
	return gHaulFailures;
}
