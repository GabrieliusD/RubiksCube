#include "Economy.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace econ
{
	const std::vector<Item>& Items()
	{
		static const std::vector<Item> items = {
			// Materials
			{ "timber", "Timber", Kind::Material, 12, 0, Gear::None, 0, "Sawn planks: for building, props, ladders and chutes." },
			{ "logs", "Logs", Kind::Material, 30, 0, Gear::None, 0, "A felled tree's trunk. Your bench saws one into four timber." },
			{ "leather", "Leather", Kind::Material, 80, 0, Gear::None, 0, "For packs." },
			{ "oil", "Lantern oil", Kind::Material, 25, 0, Gear::None, 0, "For torches." },
			{ "coal", "Coal", Kind::Material, 20, 0, Gear::None, 0, "Fuel for steam machines." },
			{ "glass", "Glass pane", Kind::Material, 60, 0, Gear::None, 0, "For windows and lanterns." },
			// Food
			{ "bread", "Bread", Kind::Food, 30, 0.30f, Gear::None, 0, "" },
			{ "beans", "Tin of beans", Kind::Food, 25, 0.25f, Gear::None, 0, "" },
			{ "jerky", "Jerky", Kind::Food, 40, 0.35f, Gear::None, 0, "Keeps forever. Good down the mine." },
			{ "stew", "Stew", Kind::Food, 60, 0.60f, Gear::None, 0, "A hot meal." },
			{ "potato", "Potato", Kind::Food, 8, 0.12f, Gear::None, 0, "Better baked." },
			{ "baked_potato", "Baked potato", Kind::Food, 20, 0.30f, Gear::None, 0, "" },
			{ "wheat", "Wheat", Kind::Material, 6, 0, Gear::None, 0, "The kitchen buys it." },
			{ "pumpkin", "Pumpkin", Kind::Food, 40, 0.20f, Gear::None, 0, "Better as soup." },
			{ "pumpkin_soup", "Pumpkin soup", Kind::Food, 70, 0.55f, Gear::None, 0, "" },
			{ "apple", "Apple", Kind::Food, 10, 0.10f, Gear::None, 0, "" },
			{ "apple_pie", "Apple pie", Kind::Food, 90, 0.60f, Gear::None, 0, "" },
			// Seeds
			{ "potato_seed", "Seed potatoes", Kind::Seed, 10, 0, Gear::None, 0, "Ripe in 3 outside days, good for 4." },
			{ "wheat_seed", "Wheat seed", Kind::Seed, 8, 0, Gear::None, 0, "Ripe in 10 outside days, good for 6." },
			{ "pumpkin_seed", "Pumpkin seed", Kind::Seed, 30, 0, Gear::None, 0, "Ripe in a month, good for 2 weeks." },
			{ "apple_sapling", "Apple sapling", Kind::Seed, 300, 0, Gear::None, 0, "Fruits every autumn from its third year." },
			// Tools
			{ "axe", "Axe", Kind::Tool, 250, 0, Gear::None, 0, "Fells trees (key 4)." },
			{ "wheelbarrow", "Wheelbarrow", Kind::Tool, 600, 0, Gear::None, 0, "Carries 160 blocks. Waiting at the trading post yard." },
			{ "hand_cart", "Hand cart", Kind::Placeable, 2500, 0, Gear::None, 0, "Runs on rails; set it on a rail with the hammer." },
			// Upgrades
			{ "copper_pick", "Copper pick", Kind::Upgrade, 800, 0, Gear::Pick, 1, "Faster, and breaks the first sealed band." },
			{ "silver_pick", "Silver pick", Kind::Upgrade, 2500, 0, Gear::Pick, 2, "Faster again, and breaks the second band." },
			{ "gold_pick", "Gold-edged pick", Kind::Upgrade, 8000, 0, Gear::Pick, 3, "The best there is. Breaks the deepest bands." },
			{ "big_pack", "Big pack", Kind::Upgrade, 400, 0, Gear::Pack, 1, "Carries 600 blocks." },
			{ "huge_pack", "Mule pack", Kind::Upgrade, 1500, 0, Gear::Pack, 2, "Carries 900 blocks." },
			{ "mine_lantern", "Miner's lantern", Kind::Upgrade, 300, 0, Gear::Lantern, 1, "Lights further." },
			{ "brass_lantern", "Brass lantern", Kind::Upgrade, 1200, 0, Gear::Lantern, 2, "Lights further again." },
			{ "electric_lamp", "Electric lamp", Kind::Upgrade, 5000, 0, Gear::Lantern, 3, "The brightest." },
			// Placed with the hammer
			{ "prop", "Timber prop", Kind::Placeable, 30, 0, Gear::None, 0, "Holds the cave roof where it stands." },
			{ "ladder", "Ladder", Kind::Placeable, 40, 0, Gear::None, 0, "Climbable, against a wall." },
			{ "chute", "Wooden chute", Kind::Placeable, 30, 0, Gear::None, 0, "A 1 m trough; blocks slide down it." },
			{ "torch", "Torch", Kind::Placeable, 20, 0, Gear::None, 0, "A light that burns for two hours of its own time." },
			{ "rails", "Rails", Kind::Placeable, 60, 0, Gear::None, 0, "1 m of track." },
			{ "hand_borer", "Hand borer", Kind::Placeable, 3000, 0, Gear::None, 0, "A drill you crank. Works only while you're there." },
			{ "steam_drill", "Steam drill", Kind::Placeable, 12000, 0, Gear::None, 0, "Bores on its own while it has coal. Works in its own time." },
			{ "electric_drill", "Electric drill", Kind::Placeable, 30000, 0, Gear::None, 0, "Bores on the power line, fast." },
			{ "belt", "Conveyor belt", Kind::Placeable, 250, 0, Gear::None, 0, "1 m of belt. Needs a steam engine within reach." },
			{ "steam_engine", "Steam engine", Kind::Placeable, 6000, 0, Gear::None, 0, "Drives belts within 30 m while it has coal." },
			{ "stamp_mill", "Stamp mill", Kind::Placeable, 8000, 0, Gear::None, 0, "Set beside the water wheel: crushes stone, washes out ore." },
			{ "parts", "Machine parts", Kind::Material, 500, 0, Gear::None, 0, "Mends a broken machine (E on it)." },
		};
		return items;
	}

	const Item* FindItem(const std::string& id)
	{
		for (const Item& item : Items())
			if (id == item.id) return &item;
		return nullptr;
	}

	cfg::Block BlockNamed(const std::string& name)
	{
		for (int b = 1; b < static_cast<int>(cfg::Block::Count); ++b)
			if (name == cfg::Info(static_cast<cfg::Block>(b)).name) return static_cast<cfg::Block>(b);
		return cfg::Block::Air;
	}

	std::string NameOf(const std::string& id)
	{
		if (const Item* item = FindItem(id)) return item->name;
		return id;
	}

	std::string Money(int cents)
	{
		char text[32];
		const int a = std::abs(cents);
		std::snprintf(text, sizeof(text), "%s$%d.%02d", cents < 0 ? "-" : "", a / 100, a % 100);
		return text;
	}

	const char* ShopName(Shop shop)
	{
		switch (shop)
		{
		case Shop::TradingPost: return "Trading post";
		case Shop::GeneralStore: return "General store";
		case Shop::Smith: return "Smithy";
		case Shop::Kitchen: return "Kitchen";
		case Shop::Assayer: return "Assay office";
		default: return "";
		}
	}

	Shop ShopForRole(const std::string& role)
	{
		if (role == "Keeper of the trading post") return Shop::TradingPost;
		if (role == "Keeper of the general store") return Shop::GeneralStore;
		if (role == "Smith") return Shop::Smith;
		if (role == "Cook" || role == "Saloon keeper") return Shop::Kitchen;
		if (role == "Assayer") return Shop::Assayer;
		return Shop::Count;
	}

	const std::vector<Offer>& Offers()
	{
		using S = Shop;
		static const std::vector<Offer> offers = {
			// The trading post (Camp) and the general store after it: supplies.
			{ "timber", S::TradingPost }, { "oil", S::TradingPost }, { "leather", S::TradingPost },
			{ "bread", S::TradingPost }, { "beans", S::TradingPost }, { "jerky", S::TradingPost },
			{ "potato_seed", S::TradingPost }, { "wheat_seed", S::TradingPost },
			{ "wheelbarrow", S::TradingPost },
			{ "big_pack", S::TradingPost, 1, 6, 0, { { "leather", 2 } } },
			{ "mine_lantern", S::TradingPost, 1, 6, 0, { { "Copper", 6 } } },
			{ "timber", S::GeneralStore }, { "oil", S::GeneralStore }, { "leather", S::GeneralStore }, { "coal", S::GeneralStore }, { "glass", S::GeneralStore },
			{ "bread", S::GeneralStore }, { "beans", S::GeneralStore }, { "jerky", S::GeneralStore },
			{ "potato_seed", S::GeneralStore }, { "wheat_seed", S::GeneralStore }, { "pumpkin_seed", S::GeneralStore }, { "apple_sapling", S::GeneralStore },
			{ "wheelbarrow", S::GeneralStore },
			{ "big_pack", S::GeneralStore, 1, 6, 0, { { "leather", 2 } } },
			{ "huge_pack", S::GeneralStore, 2, 6, 0, { { "leather", 4 }, { "Copper", 10 } } },
			{ "mine_lantern", S::GeneralStore, 1, 6, 0, { { "Copper", 6 } } },
			{ "brass_lantern", S::GeneralStore, 2, 6, 0, { { "Copper", 12 }, { "glass", 1 } } },
			{ "electric_lamp", S::GeneralStore, 4, 6 },
			// Machines the store orders in once the town is big enough (the valley's only way out
			// is closed; they come in pieces over the pass before it shut, the story will say).
			{ "steam_drill", S::GeneralStore, 3, 6, 0, { { "Silver", 20 } } },
			{ "steam_engine", S::GeneralStore, 3, 6, 0, { { "Copper", 30 } } },
			{ "belt", S::GeneralStore, 3, 6 },
			{ "electric_drill", S::GeneralStore, 4, 6, 0, { { "Gold", 10 } } },
			{ "hand_cart", S::GeneralStore, 2, 6, 0, { { "timber", 12 } } },
			// The smith: tools, by the ore they're made of.
			{ "axe", S::Smith },
			{ "copper_pick", S::Smith, 1, 6, 0, { { "Copper", 40 }, { "timber", 2 } } },
			{ "silver_pick", S::Smith, 1, 6, 0, { { "Silver", 40 }, { "timber", 2 } } },
			{ "gold_pick", S::Smith, 1, 6, 0, { { "Gold", 30 }, { "timber", 2 } } },
			{ "rails", S::Smith, 2, 6, 0, { { "timber", 1 } } },
			{ "hand_cart", S::Smith, 2, 6, 0, { { "timber", 12 } } },
			{ "hand_borer", S::Smith, 2, 6, 0, { { "Copper", 30 } } },
			{ "stamp_mill", S::Smith, 2, 6, 0, { { "Copper", 40 }, { "timber", 20 } } },
			{ "steam_drill", S::Smith, 3, 6, 0, { { "Silver", 20 } } },
			{ "steam_engine", S::Smith, 3, 6, 0, { { "Copper", 30 } } },
			{ "belt", S::Smith, 3, 6 },
			{ "parts", S::Smith, 2, 6, 0, { { "Copper", 5 } } },
			{ "parts", S::GeneralStore, 3, 6, 0, { { "Copper", 5 } } },
			// The kitchen: the camp's cook, then the saloon.
			{ "stew", S::Kitchen }, { "bread", S::Kitchen }, { "apple_pie", S::Kitchen, 2, 6 },
		};
		return offers;
	}

	std::vector<const Offer*> OffersAt(Shop shop, int era)
	{
		std::vector<const Offer*> out;
		for (const Offer& o : Offers())
			if (o.shop == shop && era >= o.eraFrom && era <= o.eraTo) out.push_back(&o);
		return out;
	}

	int OfferCents(const Offer& offer)
	{
		if (offer.cents > 0) return offer.cents;
		const Item* item = FindItem(offer.item);
		return item ? item->cents : 0;
	}

	static int EraIndex(int era) { return std::clamp(era, 1, 6) - 1; }

	int YardCents(int era) { return cfg::kStoneCents[EraIndex(era)]; }

	int BuyPrice(Shop shop, const std::string& id, int era)
	{
		const cfg::Block block = BlockNamed(id);
		if (block != cfg::Block::Air)
		{
			// Ore at the trading post (Camp), the store and the assayer (a little more); stone
			// only in the yard; the strange watches aren't for sale.
			if (block == cfg::Block::Stone || block == cfg::Block::StrangeWatch) return 0;
			if (shop != Shop::TradingPost && shop != Shop::GeneralStore && shop != Shop::Assayer) return 0;
			const float factor = cfg::kOreEraFactor[EraIndex(era)] * (shop == Shop::Assayer ? 1.1f : 1.0f);
			return static_cast<int>(std::lround(cfg::Info(block).basePrice * 100.0f * factor));
		}
		const Item* item = FindItem(id);
		if (!item) return 0;
		// Crops and food at the kitchen; logs and goods at the stores; nobody buys upgrades back.
		const bool crop = id == "potato" || id == "wheat" || id == "pumpkin" || id == "apple";
		if (shop == Shop::Kitchen && (crop || item->kind == Kind::Food)) return std::max(1, static_cast<int>(item->cents * cfg::kShopBuyBack));
		if ((shop == Shop::TradingPost || shop == Shop::GeneralStore) && (item->kind == Kind::Material || crop))
			return std::max(1, static_cast<int>(item->cents * cfg::kShopBuyBack));
		return 0;
	}

	void Purse::Add(const std::string& id, int n)
	{
		int& count = goods[id];
		count = std::max(0, count + n);
		if (count == 0) goods.erase(id);
	}

	static Verdict CheckNeeds(const std::vector<std::pair<const char*, int>>& needs, const std::function<int(const std::string&)>& have, std::string missing)
	{
		for (const auto& [id, count] : needs)
		{
			const int got = have(id);
			if (got < count)
			{
				if (!missing.empty()) missing += ", ";
				missing += std::to_string(count - got) + " more " + NameOf(id);
			}
		}
		return { missing.empty(), missing };
	}

	Verdict CanBuy(const Offer& offer, int cents, const std::function<int(const std::string&)>& have)
	{
		const int price = OfferCents(offer);
		std::string missing;
		if (cents < price) missing = Money(price - cents) + " more";
		return CheckNeeds(offer.needs, have, missing);
	}

	const std::vector<Project>& Projects()
	{
		static const std::vector<Project> projects = {
			{ "camp_hearth", "A stone hearth for the camp", 1, 300, 1500, "The camp has a stone hearth now. Mae says it's the first thing here that'll outlast her." },
			{ "post_floor", "A stone floor for the trading post", 1, 600, 2500, "Agnes has a stone floor. No more mud on the counter." },
			{ "tower_base", "The clock tower's base", 2, 2500, 8000, "The clock tower stands on your stone." },
			{ "bridge_piers", "Piers for the river bridge", 2, 1500, 6000, "The bridge has stone piers: it'll stand the spring floods." },
			{ "bank_vault", "The bank's vault", 3, 3000, 15000, "The bank's vault is built of your stone. Money rots, the vault doesn't." },
			{ "engine_house", "The engine house at the mine", 3, 2000, 10000, "The engine house is up. The winding tower can come next." },
			{ "chapel_tower", "The chapel's bell tower", 4, 4000, 20000, "The chapel has its bell tower. You can hear it in the mine." },
		};
		return projects;
	}

	const std::vector<Recipe>& Recipes()
	{
		static const std::vector<Recipe> recipes = {
			{ "timber", 4, { { "logs", 1 } } },
			{ "prop", 1, { { "timber", 2 } } },
			{ "ladder", 1, { { "timber", 3 } } },
			{ "chute", 1, { { "timber", 2 } } },
			{ "torch", 2, { { "timber", 1 }, { "oil", 1 } } },
		};
		return recipes;
	}

	const std::vector<Recipe>& CookRecipes()
	{
		static const std::vector<Recipe> recipes = {
			{ "baked_potato", 1, { { "potato", 1 } } },
			{ "pumpkin_soup", 2, { { "pumpkin", 1 } } },
			{ "bread", 1, { { "wheat", 3 } } },
			{ "apple_pie", 1, { { "apple", 3 }, { "wheat", 2 } } },
		};
		return recipes;
	}

	std::string BestFood(const std::function<int(const std::string&)>& have)
	{
		const Item* best = nullptr;
		for (const Item& item : Items())
			if (item.kind == Kind::Food && have(item.id) > 0 && (!best || item.food > best->food)) best = &item;
		return best ? best->id : std::string();
	}

	Verdict CanMake(const Recipe& recipe, const std::function<int(const std::string&)>& have)
	{
		return CheckNeeds(recipe.needs, have, "");
	}
}
