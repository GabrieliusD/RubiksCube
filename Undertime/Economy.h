#pragma once
// Money, goods, the town's shops and your workbench (Docs/Hauling.md). Pure C++, tested in
// Tests/HaulingTests.cpp.
//
// Goods are counted by id: block names for mined blocks ("Stone", "Copper", as cfg::kBlocks
// and the missions use them) and lower-case ids for everything else ("timber", "potato").
// Money is in cents.
#include "Config.h"
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace econ
{
	using Goods = std::map<std::string, int>;

	enum class Kind
	{
		Material,   // timber, logs, oil...: used by the bench, building and machines
		Food,       // eaten (E in the bag, or at a fire): fills the hunger meter
		Seed,       // planted in a farm plot
		Tool,       // a tool for a slot (the axe), or a thing that's placed once (the barrow)
		Upgrade,    // sets a gear level (pick, pack, lantern) when it's bought
		Placeable,  // put in the world with the hammer: props, ladders, chutes, machines
	};

	// What an upgrade changes.
	enum class Gear { None, Pick, Pack, Lantern };

	struct Item
	{
		const char* id;
		const char* name;
		Kind kind;
		int cents;            // the town's price for one (shops pay kShopBuyBack of it back)
		float food = 0.0f;    // Food: share of the hunger meter it fills
		Gear gear = Gear::None;
		int level = 0;        // Upgrade: the level it sets
		const char* about = "";
	};

	const std::vector<Item>& Items();
	const Item* FindItem(const std::string& id);
	// The name to show: an item's name, or the block name itself.
	std::string NameOf(const std::string& id);
	// The block with that name (cfg::Block::Air if it isn't one).
	cfg::Block BlockNamed(const std::string& name);

	// "$3.20", "-$0.05".
	std::string Money(int cents);

	// ---- Shops ------------------------------------------------------------------------
	enum class Shop { TradingPost, GeneralStore, Smith, Kitchen, Assayer, Count };
	const char* ShopName(Shop shop);
	// The shop a person keeps, from the role they have at the time (npc::LifeAt(...).role).
	// Shop::Count if none.
	Shop ShopForRole(const std::string& role);

	struct Offer
	{
		const char* item;
		Shop shop;
		int eraFrom = 1, eraTo = 6;
		int cents = 0;                                     // 0: the item's own price
		std::vector<std::pair<const char*, int>> needs;    // materials you bring (block names or item ids)
	};
	const std::vector<Offer>& Offers();
	// What `shop` sells in `era`, in catalogue order.
	std::vector<const Offer*> OffersAt(Shop shop, int era);
	int OfferCents(const Offer& offer);

	// What the shop pays for one of `id` in `era` (0: it doesn't buy it). Blocks: ore at the
	// trading post and the assayer, stone only in the yard (YardCents). Goods: at shops that
	// sell that kind (kShopBuyBack of the price); crops and logs at the kitchen / store.
	int BuyPrice(Shop shop, const std::string& id, int era);
	// A block of stone tipped into the yard.
	int YardCents(int era);

	// ---- Buying and selling, on a purse and a bag ---------------------------------------
	struct Purse
	{
		int cents = 0;
		Goods goods;      // everything you carry that isn't counted elsewhere
		int Count(const std::string& id) const { auto it = goods.find(id); return it == goods.end() ? 0 : it->second; }
		void Add(const std::string& id, int n);
	};

	// Whether you can afford `offer` (money and materials from `have`, which may count
	// blocks in the backpack too).
	struct Verdict { bool ok = false; std::string missing; };
	Verdict CanBuy(const Offer& offer, int cents, const std::function<int(const std::string&)>& have);

	// ---- Town projects -----------------------------------------------------------------
	// What the town builds with the stone in its yard, an era's projects one after another
	// (Docs/Hauling.md H12). The town works through the yard's heap at cfg::kTownStonePerDay.
	struct Project
	{
		const char* id;
		const char* name;
		int era;
		int stone;          // blocks it takes
		int rewardCents;    // the town's thanks when it's finished
		const char* done;   // what you hear when it's finished
	};
	const std::vector<Project>& Projects();

	// ---- Your workbench ----------------------------------------------------------------
	struct Recipe
	{
		const char* item;
		int makes = 1;
		std::vector<std::pair<const char*, int>> needs;
	};
	const std::vector<Recipe>& Recipes();
	// At a fire: raw food into meals that fill more.
	const std::vector<Recipe>& CookRecipes();
	// The most filling food in `have` (an item id, or "" if there's none).
	std::string BestFood(const std::function<int(const std::string&)>& have);
	Verdict CanMake(const Recipe& recipe, const std::function<int(const std::string&)>& have);
}
