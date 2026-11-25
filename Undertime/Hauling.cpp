// Money, shops, the stone yard (Docs/Hauling.md H1); the wheelbarrow and the rest come in
// the files next to it. UndertimeModule methods, split out of UndertimeModule.cpp.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include "Life.h"
#include <Core/Log.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;
using cfg::Block;

namespace
{
	// The fence around the yard, open on the trail side (+x) for the barrow.
	VoxelModel YardFence(int sizeX, int sizeZ)
	{
		kit::Builder b(sizeX, 5, sizeZ);
		const uint8_t wood = b.Colour(kit::colour::kWood), dark = b.Colour(kit::colour::kWoodDark), light = b.Colour(kit::colour::kWoodLight);
		auto rail = [&](int x0, int z0, int x1, int z1)
		{
			b.Speckle(x0, 2, z0, x1, 3, z1, { wood, light });
			b.Speckle(x0, 4, z0, x1, 5, z1, { wood, light });
		};
		rail(0, 0, sizeX, 1);                 // south
		rail(0, sizeZ - 1, sizeX, sizeZ);     // north
		rail(0, 0, 1, sizeZ);                 // west
		const int gate0 = sizeZ / 2 - 5, gate1 = sizeZ / 2 + 5;
		rail(sizeX - 1, 0, sizeX, gate0);     // east, either side of the gate
		rail(sizeX - 1, gate1, sizeX, sizeZ);
		for (int x = 0; x < sizeX; x += 4)
			for (int z : { 0, sizeZ - 1 }) b.Box(x, 0, z, x + 1, 5, z + 1, dark);
		for (int z = 0; z < sizeZ; z += 4)
		{
			b.Box(0, 0, z, 1, 5, z + 1, dark);
			if (z < gate0 || z >= gate1) b.Box(sizeX - 1, 0, z, sizeX, 5, z + 1, dark);
		}
		for (int z : { gate0 - 1, gate1 }) b.Box(sizeX - 1, 0, z, sizeX, 5, z + 1, dark);
		b.Box(sizeX - 1, 0, sizeZ - 1, sizeX, 5, sizeZ, dark);
		return b.model;
	}
}

void UndertimeModule::StartHauling(Engine& engine)
{
	const float minX = cfg::kYardCenter[0] - cfg::kYardHalf[0], maxX = cfg::kYardCenter[0] + cfg::kYardHalf[0];
	const float minZ = cfg::kYardCenter[1] - cfg::kYardHalf[1], maxZ = cfg::kYardCenter[1] + cfg::kYardHalf[1];
	blocks.SetYard(minX, minZ, maxX, maxZ);
	yardGround = valley ? valley->Height(cfg::kYardCenter[0], cfg::kYardCenter[1]) : cfg::kGroundY;
	const int sx = static_cast<int>(std::lround((maxX - minX) / cfg::kVoxelSize)), sz = static_cast<int>(std::lround((maxZ - minZ) / cfg::kVoxelSize));
	yardMesh = kit::Bake(engine, YardFence(sx, sz), VoxelPivot::Corner);
	yardEntity = kit::Spawn(engine, yardMesh, engine.GetAssets().GetMaterial("Materials/Town.mat"), { minX, yardGround, minZ });
	// Low walls on three sides and either side of the gate, so tipped stone stays in.
	PhysicsWorld& physics = engine.GetPhysics();
	const float h = 0.6f, t = 0.125f, y = yardGround + h;
	const float gate = 1.25f;
	yardBodies = {
		physics.CreateStaticBox({ (minX + maxX) * 0.5f, y, minZ + t }, { (maxX - minX) * 0.5f, h, t }),
		physics.CreateStaticBox({ (minX + maxX) * 0.5f, y, maxZ - t }, { (maxX - minX) * 0.5f, h, t }),
		physics.CreateStaticBox({ minX + t, y, (minZ + maxZ) * 0.5f }, { t, h, (maxZ - minZ) * 0.5f }),
		physics.CreateStaticBox({ maxX - t, y, (minZ + cfg::kYardCenter[1] - gate) * 0.5f }, { t, h, (cfg::kYardCenter[1] - gate - minZ) * 0.5f }),
		physics.CreateStaticBox({ maxX - t, y, (maxZ + cfg::kYardCenter[1] + gate) * 0.5f }, { t, h, (maxZ - cfg::kYardCenter[1] - gate) * 0.5f }),
	};
	purse = {};
	purse.cents = cfg::kStartingCents;
	packLevel = 0;
	shopOpen = pendingShop = econ::Shop::Count;
	yardTimer = 0.0f;
	yardCents = yardBlocks = 0;
	hunger = 1.0f;
	StartBarrow(engine);
	StartClaim(engine);
	StartWood(engine);
}

void UndertimeModule::StopHauling(Engine& engine)
{
	StopPlaced(engine);
	StopFarm(engine);
	StopWood(engine);
	StopClaim(engine);
	StopBarrow(engine);
	Scene& scene = engine.GetScene();
	if (scene.IsAlive(yardEntity)) scene.DestroyEntity(yardEntity);
	yardEntity = INVALID_ENTITY;
	if (yardMesh.IsValid()) engine.GetRenderer().DestroyMesh(yardMesh);
	yardMesh = {};
	for (BodyId body : yardBodies) if (body != kInvalidBody) engine.GetPhysics().RemoveBody(body);
	yardBodies.clear();
}

void UndertimeModule::ResetHauling(Engine& engine)
{
	purse = {};
	purse.cents = cfg::kStartingCents;
	packLevel = 0;
	hunger = 1.0f;
	ResetBarrow(engine);
	ResetClaim(engine);
	ResetWood(engine);
	StopFarm(engine);
	StopPlaced(engine);
	heldCart = -1;
	placeItem.clear();
	projectStone.clear();
	projectDone.clear();
	projectClock = -1.0;
}

// =============================================================================
// Town projects: the town builds with the stone in its yard
// =============================================================================

void UndertimeModule::UpdateProjects(Engine& engine)
{
	if (state == State::Title) return;
	const double now = clock.Days();
	if (projectClock < 0.0 || now < projectClock) { projectClock = now; return; }   // (time turned back: the town just carries on)
	const double days = now - projectClock;
	if (days < 0.05) return;
	projectClock = now;
	// This era's first unfinished project (earlier eras' unfinished ones stay as they were).
	const econ::Project* current = nullptr;
	for (const econ::Project& p : econ::Projects())
		if (p.era == era && !projectDone.count(p.id)) { current = &p; break; }
	if (!current || blocks.YardCount() <= 0) return;
	projectStoneDebt += static_cast<float>(days * cfg::kTownStonePerDay);
	const int want = std::min(static_cast<int>(projectStoneDebt), current->stone - projectStone[current->id]);
	if (want <= 0) return;
	const int took = blocks.TakeFromYard(engine, want);
	projectStoneDebt -= took;
	if (took < want) projectStoneDebt = 0.0f;   // the yard ran out: the town waits for more
	projectStone[current->id] += took;
	if (projectStone[current->id] >= current->stone)
	{
		projectDone[current->id] = now;
		purse.cents += current->rewardCents;
		messages.Show(std::string(current->done) + "\nThe town thanks you: +" + econ::Money(current->rewardCents) + ".", 5.0f);
		saveRequested = true;
	}
}

void UndertimeModule::DrawProjects()
{
	const ImVec4 gold(1.0f, 0.77f, 0.47f, 1.0f);
	ImGui::TextColored(gold, "Town projects");
	ImGui::SameLine();
	ImGui::TextDisabled("built with the stone in the yard (%d there now)", blocks.YardCount());
	for (const econ::Project& p : econ::Projects())
	{
		if (p.era > era) continue;
		const int used = projectStone.count(p.id) ? projectStone.at(p.id) : 0;
		if (projectDone.count(p.id)) ImGui::TextDisabled("  %s: finished.", p.name);
		else if (p.era < era) ImGui::TextDisabled("  %s: never finished (%d of %d stone).", p.name, used, p.stone);
		else ImGui::Text("  %s: %d of %d stone   (the town pays %s when it's done)", p.name, used, p.stone, econ::Money(p.rewardCents).c_str());
	}
}

int UndertimeModule::Have(const std::string& id) const
{
	const Block block = econ::BlockNamed(id);
	if (block != Block::Air) return backpack[static_cast<int>(block)];
	return purse.Count(id);
}

void UndertimeModule::Take(const std::string& id, int count)
{
	const Block block = econ::BlockNamed(id);
	if (block != Block::Air) backpack[static_cast<int>(block)] = std::max(0, backpack[static_cast<int>(block)] - count);
	else purse.Add(id, -count);
}

void UndertimeModule::Give(Engine& engine, const std::string& id, int count)
{
	const Block block = econ::BlockNamed(id);
	if (block != Block::Air)
	{
		backpack[static_cast<int>(block)] += count;
		return;
	}
	const econ::Item* item = econ::FindItem(id);
	if (item && item->kind == econ::Kind::Upgrade)
	{
		switch (item->gear)
		{
		case econ::Gear::Pick: pickLevel = std::max(pickLevel, item->level); break;
		case econ::Gear::Pack: packLevel = std::max(packLevel, item->level); break;
		case econ::Gear::Lantern: lanternLevel = std::max(lanternLevel, item->level); break;
		default: break;
		}
		return;
	}
	if (id == "wheelbarrow")
	{
		BuyBarrow(engine);
		return;
	}
	purse.Add(id, count);
}

// =============================================================================
// Shops
// =============================================================================

void UndertimeModule::ShopTopics(const npc::DialogContext& c, std::deque<npc::Topic>& out)
{
	if (c.npc < 0) return;
	const npc::LifeState life = npc::LifeAt(c.npc, c.days, c.log);
	const econ::Shop shop = econ::ShopForRole(life.role ? life.role : "");
	if (shop == econ::Shop::Count) return;
	out.emplace_back();
	npc::Topic& t = out.back();
	t.id = "trade";
	t.title = shop == econ::Shop::Kitchen ? "Something to eat?" : "Let's trade.";
	t.priority = 5;
	npc::Step step;
	step.say = shop == econ::Shop::Kitchen ? "Sit yourself down." : shop == econ::Shop::Smith ? "What'll it be?" : "Have a look.";
	npc::Effect open;
	open.kind = npc::Effect::Kind::Shop;
	open.name = std::to_string(static_cast<int>(shop));
	npc::Effect end;
	end.kind = npc::Effect::Kind::End;
	step.effects = { open, end };
	t.steps.push_back(step);
}

void UndertimeModule::OpenShop(Engine& engine, econ::Shop shop)
{
	shopOpen = shop;
	shopNote.clear();
	state = State::Shop;
	engine.GetInput().SetMouseCaptured(false);
}

void UndertimeModule::CloseShop(Engine& engine)
{
	shopOpen = econ::Shop::Count;
	state = State::Playing;
	engine.GetInput().SetMouseCaptured(true);
	waitForRelease = true;
	saveRequested = true;
}

void UndertimeModule::DrawShop(Engine& engine)
{
	if (shopOpen == econ::Shop::Count) return;
	const ViewRect view = engine.GameViewRect();
	const float scale = ImGui::GetFontSize() / 13.0f;
	ImGui::SetNextWindowPos(ImVec2(view.x + view.width * 0.5f, view.y + view.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(view.width * 0.8f, 860.0f * scale), std::min(view.height * 0.8f, 620.0f * scale)), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);
	ImGui::Begin("##shop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	const ImVec4 gold(1.0f, 0.77f, 0.47f, 1.0f);
	ImGui::TextColored(gold, "%s", econ::ShopName(shopOpen));
	if (!shopKeeper.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("- %s", shopKeeper.c_str());
	}
	ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f * scale);
	ImGui::Text("You have %s", econ::Money(purse.cents).c_str());
	ImGui::Separator();
	const auto have = [this](const std::string& id) { return Have(id); };

	if (ImGui::BeginTable("##shoptable", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV, ImVec2(0, -40.0f * scale)))
	{
		ImGui::TableNextColumn();
		ImGui::TextColored(gold, "Buy");
		ImGui::BeginChild("##buy", ImVec2(0, 0));
		const std::vector<const econ::Offer*> offers = econ::OffersAt(shopOpen, era);
		if (offers.empty()) ImGui::TextDisabled("Nothing for sale.");
		for (size_t i = 0; i < offers.size(); ++i)
		{
			const econ::Offer& o = *offers[i];
			const econ::Item* item = econ::FindItem(o.item);
			if (!item) continue;
			ImGui::PushID(static_cast<int>(i));
			// Upgrades you already have (or better) aren't offered again.
			bool owned = false;
			if (item->kind == econ::Kind::Upgrade)
				owned = (item->gear == econ::Gear::Pick && pickLevel >= item->level) || (item->gear == econ::Gear::Pack && packLevel >= item->level) ||
					(item->gear == econ::Gear::Lantern && lanternLevel >= item->level);
			if (std::string(o.item) == "wheelbarrow") owned = HasBarrow();
			std::string price = econ::Money(econ::OfferCents(o));
			for (const auto& [need, n] : o.needs) price += " + " + std::to_string(n) + " " + econ::NameOf(need);
			const econ::Verdict check = econ::CanBuy(o, purse.cents, have);
			ImGui::BeginDisabled(owned || !check.ok);
			if (ImGui::Button(owned ? "Have it" : "Buy"))
			{
				purse.cents -= econ::OfferCents(o);
				for (const auto& [need, n] : o.needs) Take(need, n);
				Give(engine, o.item, 1);
				shopNote = std::string("Bought ") + item->name + " for " + price + ".";
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::Text("%s", item->name);
			ImGui::SameLine();
			ImGui::TextDisabled("%s", price.c_str());
			if (item->kind != econ::Kind::Upgrade && item->kind != econ::Kind::Tool && Have(o.item) > 0)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(have %d)", Have(o.item));
			}
			if (*item->about)
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled("    %s%s", item->about, owned || check.ok ? "" : ("  Needs " + check.missing + ".").c_str());
				ImGui::PopTextWrapPos();
			}
			else if (!owned && !check.ok) ImGui::TextDisabled("    Needs %s.", check.missing.c_str());
			ImGui::PopID();
		}
		ImGui::EndChild();

		ImGui::TableNextColumn();
		ImGui::TextColored(gold, "Sell");
		ImGui::BeginChild("##sell", ImVec2(0, 0));
		int listed = 0;
		auto sellRow = [&](const std::string& id, int count)
		{
			const int each = econ::BuyPrice(shopOpen, id, era);
			if (each <= 0 || count <= 0) return;
			++listed;
			ImGui::PushID(id.c_str());
			if (ImGui::Button("Sell 1")) { Take(id, 1); purse.cents += each; shopNote = "Sold 1 " + econ::NameOf(id) + " for " + econ::Money(each) + "."; }
			ImGui::SameLine();
			if (ImGui::Button("All"))
			{
				Take(id, count);
				purse.cents += each * count;
				shopNote = "Sold " + std::to_string(count) + " " + econ::NameOf(id) + " for " + econ::Money(each * count) + ".";
			}
			ImGui::SameLine();
			ImGui::Text("%s  x%d", econ::NameOf(id).c_str(), count);
			ImGui::SameLine();
			ImGui::TextDisabled("%s each", econ::Money(each).c_str());
			ImGui::PopID();
		};
		for (int b = 1; b < static_cast<int>(Block::Count); ++b) sellRow(cfg::Info(static_cast<Block>(b)).name, backpack[b]);
		const econ::Goods goods = purse.goods;   // a copy: selling changes it
		for (const auto& [id, n] : goods) sellRow(id, n);
		if (listed == 0) ImGui::TextDisabled(shopOpen == econ::Shop::Smith ? "The smith doesn't buy." : "Nothing they'd buy.");
		if (backpack[static_cast<int>(Block::Stone)] > 0 && shopOpen == econ::Shop::TradingPost)
		{
			ImGui::Spacing();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("Stone: tip it in the yard north of here, %s a block.", econ::Money(econ::YardCents(era)).c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::EndChild();
		ImGui::EndTable();
	}
	ImGui::Separator();
	if (ImGui::Button("Leave  (Esc)") || engine.GetInput().WasKeyPressed(VK_ESCAPE)) CloseShop(engine);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", shopNote.c_str());
	ImGui::End();
}

// =============================================================================
// Every frame
// =============================================================================

void UndertimeModule::UpdateHauling(Engine& engine, float dt)
{
	UpdateBarrow(engine, dt);
	UpdateHunger(engine, dt);
	if (tool != Tool::Hammer) claim.swingTimer = 0.0f;
}

void UndertimeModule::AfterHauling(Engine& engine, float dt)
{
	// Stone that came to rest in the yard: paid for, once. One toast sums up a tip-out.
	const LooseBlocks::Counts paid = blocks.TakeYardPaid();
	int cents = 0, count = 0;
	for (int b = 1; b < static_cast<int>(Block::Count); ++b)
	{
		if (paid[b] == 0) continue;
		const Block block = static_cast<Block>(b);
		const int each = block == Block::Stone ? econ::YardCents(era) : econ::BuyPrice(econ::Shop::TradingPost, cfg::Info(block).name, era);
		cents += each * paid[b];
		count += paid[b];
	}
	if (count > 0)
	{
		purse.cents += cents;
		yardCents += cents;
		yardBlocks += count;
		yardTimer = cfg::kYardPayDelay;
	}
	if (yardTimer > 0.0f)
	{
		yardTimer -= dt;
		if (yardTimer <= 0.0f && yardBlocks > 0)
		{
			messages.Show("The yard: " + std::to_string(yardBlocks) + " block" + (yardBlocks == 1 ? "" : "s") + ", +" + econ::Money(yardCents), 2.5f);
			yardBlocks = yardCents = 0;
		}
	}
	UpdateProjects(engine);
	if (yardRestore > 0)
	{
		blocks.FillYard(engine, Block::Stone, yardRestore, yardGround);
		yardRestore = 0;
	}
	AfterBarrow(engine, dt);
	// The claim (pieces under the crosshair, for E, whatever the tool), the forest and crops.
	UpdateClaim(engine, dt);
	UpdateWood(engine, dt);
	UpdateFarm(engine, dt);
	UpdatePlaced(engine, dt);
}

bool UndertimeModule::HaulingInteract(Engine& engine)
{
	if (barrow.held) return BarrowInteract(engine);
	return PlacedInteract(engine) || FarmInteract(engine) || ClaimInteract(engine) || BarrowInteract(engine);
}

std::string UndertimeModule::HaulingPrompt() const
{
	if (barrow.held) return BarrowPrompt();
	for (const std::string& p : { PlacedPrompt(), WoodPrompt(), FarmPrompt(), ClaimPrompt(), BarrowPrompt() })
		if (!p.empty()) return p;
	return {};
}

void UndertimeModule::HaulingHud(std::vector<std::string>& bag) const
{
	bag.insert(bag.begin(), "Money  " + econ::Money(purse.cents));
	{
		// Hunger: a bar of ten.
		const int full = static_cast<int>(std::ceil(hunger * 10.0f - 0.01f));
		bag.insert(bag.begin() + 1, "Food   " + std::string(full, '#') + std::string(10 - full, '.') + (hunger <= 0.0f ? "  weak" : hunger <= cfg::kHungerWarn ? "  hungry" : ""));
	}
	BarrowHud(bag);
	ClaimHud(bag);
	for (const auto& [id, n] : purse.goods)
	{
		char line[64];
		std::snprintf(line, sizeof(line), "%-14s %d", econ::NameOf(id).c_str(), n);
		bag.push_back(line);
	}
}

// =============================================================================
// The save game
// =============================================================================

nlohmann::json UndertimeModule::SaveHauling() const
{
	nlohmann::json j;
	j["cents"] = purse.cents;
	j["goods"] = purse.goods;
	j["packLevel"] = packLevel;
	j["yardStone"] = blocks.YardCount();
	j["barrow"] = SaveBarrow();
	j["claim"] = SaveClaim();
	j["hunger"] = hunger;
	j["felled"] = SaveWood();
	j["crops"] = SaveFarm();
	j["placed"] = SavePlaced();
	j["projectStone"] = projectStone;
	j["projectDone"] = projectDone;
	return j;
}

void UndertimeModule::LoadHauling(Engine& engine, const nlohmann::json& j)
{
	ResetHauling(engine);
	if (!j.is_object()) return;
	purse.cents = j.value("cents", cfg::kStartingCents);
	if (j.contains("goods")) purse.goods = j["goods"].get<econ::Goods>();
	packLevel = j.value("packLevel", 0);
	yardRestore = std::max(0, j.value("yardStone", 0) - blocks.YardCount());
	if (j.contains("barrow")) LoadBarrow(engine, j["barrow"]);
	if (j.contains("claim")) LoadClaim(engine, j["claim"]);
	hunger = j.value("hunger", 1.0f);
	if (j.contains("felled")) LoadWood(engine, j["felled"]);
	if (j.contains("crops")) LoadFarm(engine, j["crops"]);
	if (j.contains("placed")) LoadPlaced(engine, j["placed"]);
	projectStone = j.contains("projectStone") ? j["projectStone"].get<std::map<std::string, int>>() : std::map<std::string, int>{};
	projectDone = j.contains("projectDone") ? j["projectDone"].get<std::map<std::string, double>>() : std::map<std::string, double>{};
	projectClock = -1.0;
}

// =============================================================================
// Development: UNDERTIME_DEV_HAUL="words" sets up a scene for screenshots and tests
// (development runs never save):
//   cents N      money
//   barrow N     a barrow beside you, with N stone in it (held: "hold"; tipping: "tip")
//   yard N       N blocks of the town's stone in the yard
//   shop N       open shop N (econ::Shop: 0 trading post, 1 store, 2 smith, 3 kitchen, 4 assayer)
//   give id N    goods
// =============================================================================

void UndertimeModule::DevHauling(Engine& engine)
{
	char* text = nullptr;
	size_t length = 0;
	if (_dupenv_s(&text, &length, "UNDERTIME_DEV_HAUL") != 0 || !text) return;
	std::istringstream in(text);
	std::free(text);
	savingAllowed = false;
	if (state == State::Title) state = State::Playing;
	for (std::string word; in >> word;)
	{
		int n = 0;
		if (word == "cents" && in >> n) purse.cents = n;
		else if (word == "yard" && in >> n) yardRestore += n;
		else if (word == "barrow" && in >> n)
		{
			const XMFLOAT3 f{ std::sin(player.yaw), 0.0f, std::cos(player.yaw) };
			XMFLOAT3 at{ player.position.x + f.x * 1.6f, player.position.y, player.position.z + f.z * 1.6f };
			if (valley && !IsInside()) at.y = valley->Height(at.x, at.z);
			PlaceBarrow(engine, at, player.yaw);
			barrow.load[static_cast<int>(Block::Stone)] = std::min(n, cfg::kBarrowCapacity);
		}
		else if (word == "hold" && barrow.owned) { BarrowInteract(engine); barrow.held = true; }
		else if (word == "tip" && barrow.owned) { barrow.held = true; barrow.tipping = 0.0f; barrow.tipped = 0; }
		else if (word == "shop" && in >> n) OpenShop(engine, static_cast<econ::Shop>(std::clamp(n, 0, static_cast<int>(econ::Shop::Count) - 1)));
		else if (word == "claim")
		{
			// Stand at the claim's south-west corner, looking in.
			player.Teleport({ claim.origin.x + 4.0f, claim.origin.y + 0.5f, claim.origin.z + 1.5f }, XMConvertToRadians(40.0f));
			player.pitch = XMConvertToRadians(8.0f);
		}
		else if (word == "house")
		{
			// A sample house and a field (screenshots, and to see what the pieces make).
			auto put = [&](const char* id, int x, int y, int z, int turn = 0)
			{
				build::Piece p;
				p.type = build::PieceIndex(id);
				p.x = x; p.y = y; p.z = z; p.turn = turn;
				p.swings = p.Def().swings;
				if (claimPlan.CanPlace(p).empty()) claimPlan.Place(p);
				else CHERRY_LOG("UNDERTIME: sample house: %s at %d %d %d: %s", id, x, y, z, claimPlan.CanPlace(p).c_str());
			};
			for (int x = 8; x <= 11; ++x) for (int z = 8; z <= 10; ++z) put("foundation", x, 0, z);
			for (int y = 0; y <= 1; ++y)
			{
				for (int x = 8; x <= 11; ++x)
				{
					if (x != 9) put(y == 0 ? "stone_wall" : "plank_wall", x, y, 8, 0);
					put(y == 0 ? "stone_wall" : (x == 10 ? "window_hole" : "plank_wall"), x, y, 10, 2);
				}
				for (int z = 8; z <= 10; ++z)
				{
					put(y == 0 ? "stone_wall" : "plank_wall", 8, y, z, 3);
					put(y == 0 ? "stone_wall" : (z == 9 ? "window_hole" : "plank_wall"), 11, y, z, 1);
				}
			}
			put("door", 9, 0, 8, 0);
			for (int x = 8; x <= 11; ++x) { put("roof", x, 2, 8, 2); put("roof_ridge", x, 2, 9, 0); put("roof", x, 2, 10, 0); }
			put("fire_pit", 10, 0, 9); put("bed", 8, 0, 10, 2); put("crate", 11, 0, 10); put("lamp", 9, 0, 9);
			for (int x = 14; x <= 17; ++x) for (int z = 8; z <= 10; ++z) put("farm_plot", x, 0, z);
			for (int x = 13; x <= 18; ++x) { put("fence", x, 0, 7, 0); put("fence", x, 0, 11, 2); }
			claim.dirty = true;
			RebuildClaim(engine);
			// Crops at different stages.
			const double now = clock.Days();
			const double ages[] = { 0.5, 1.5, 3.5, 9.0 };
			for (int x = 14; x <= 17; ++x)
				for (int z = 8; z <= 10; ++z)
				{
					Planting p;
					p.crop = z == 10 ? 2 : z == 9 ? 1 : 0;
					p.day = now - ages[x - 14] * (p.crop == 0 ? 1.0 : p.crop == 1 ? 3.0 : 10.0);
					plantings[(0 * 256 + z) * 256 + x] = p;
				}
			farmTimer = 0.0f;
		}
		else if (word == "tool" && in >> n) tool = static_cast<Tool>(std::clamp(n, 1, 4) - 1);
		else if (word == "menu")
		{
			std::string which;
			in >> which;
			OpenMenu(engine, which == "bench" ? Menu::Bench : which == "store" ? Menu::Store : which == "cook" ? Menu::Cook : Menu::Pieces);
		}
		else if (word == "fast" && in >> n) debugTimeScale = std::clamp(n, 0, cfg::kDebugTimeScaleMax);
		else if (word == "place")
		{
			// place item x y z dx dz: from that point along that direction, at what it hits.
			std::string item;
			XMFLOAT3 from{}, along{};
			if (!(in >> item >> from.x >> from.y >> from.z >> along.x >> along.z)) continue;
			const auto hit = engine.GetPhysics().Raycast(from, along, 12.0f);
			if (!hit) { CHERRY_LOG("UNDERTIME: dev place %s: nothing hit", item.c_str()); continue; }
			Placed thing;
			thing.item = item;
			std::string why;
			if (IsMachine(item)) PlaceMachine(engine, item, hit->point, hit->normal, thing, why);
			else { thing.at = hit->point; thing.yaw = std::atan2(along.x, along.z); }
			if (!why.empty()) { CHERRY_LOG("UNDERTIME: dev place %s: %s", item.c_str(), why.c_str()); continue; }
			thing.fuel = item == "steam_drill" || item == "steam_engine" ? cfg::kFuelTank : 0.0f;
			placed.push_back(thing);
			SpawnPlaced(engine, placed.back());
			CHERRY_LOG("UNDERTIME: dev placed %s at %.2f %.2f %.2f yaw %.2f", item.c_str(), thing.at.x, thing.at.y, thing.at.z, thing.yaw);
		}
		else if (word == "give")
		{
			std::string id;
			if (in >> id >> n) Give(engine, id, n);
		}
	}
}
