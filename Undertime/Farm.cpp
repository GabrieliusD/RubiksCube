// Farming (Docs/Hauling.md H7): farm plots are building pieces on your claim (dug with the
// hammer). E on an empty plot plants the first seed you carry; crops grow in outside days from
// the day they were planted, stay ripe for a while, then go to seed. Pick the dive to come up
// at harvest. Apple saplings take years, then fruit every autumn. The claim is shielded from
// turning time back, so crops are too: they only ever grow.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include "Growth.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
	int PlotKey(int x, int y, int z) { return (y * 256 + z) * 256 + x; }

	// A plot's crop at a stage: 0..3 growing, 4 ripe, 5 spoiled.
	VoxelModel CropModel(int crop, int look)
	{
		const bool tree = crop == 3;
		kit::Builder b(4, tree ? 10 : 4, 4);
		const uint8_t leaf = b.Colour(0x5a8a3a), leafDark = b.Colour(0x46702c), dry = b.Colour(0x8a7a4a), dead = b.Colour(0x6a5a3a);
		if (tree)
		{
			const int h = 2 + std::min(look, 4) * 2;
			b.Box(1, 0, 1, 2, h, 2, b.Colour(0x5a4030));
			if (look >= 2) b.Speckle(0, h - 2, 0, 4, h + 1, 4, { leaf, leafDark });
			if (look == 4) { const uint8_t red = b.Colour(0xc83a2a); b.Box(0, h - 1, 0, 1, h, 1, red); b.Box(3, h - 2, 2, 4, h - 1, 3, red); b.Box(2, h, 3, 3, h + 1, 4, red); }
			return b.model;
		}
		const int height = std::min(look, 3) + 1;
		for (int x = 0; x < 4; x += 2)
		{
			if (look == 5) { b.Box(x, 0, 0, x + 1, 1, 4, dead); continue; }
			b.Speckle(x, 0, 0, x + 1, height, 4, look >= 4 && crop == 1 ? std::vector<uint8_t>{ dry, b.Colour(0xd8b858) } : std::vector<uint8_t>{ leaf, leafDark });
			if (look == 4)
			{
				if (crop == 0) b.Box(x, 0, 1, x + 1, 1, 2, b.Colour(0x9a7a4a)), b.Box(x, 0, 3, x + 1, 1, 4, b.Colour(0x9a7a4a));   // potatoes peeking out
				if (crop == 2) b.Box(x, 0, 1, x + 2, 2, 3, b.Colour(0xe0802a));                                                  // a pumpkin
			}
		}
		return b.model;
	}
}

void UndertimeModule::StopFarm(Engine& engine)
{
	Scene& scene = engine.GetScene();
	for (auto& [key, p] : plantings) if (scene.IsAlive(p.entity)) scene.DestroyEntity(p.entity);
	plantings.clear();
	for (auto& [key, m] : cropMeshes) if (m.IsValid()) engine.GetRenderer().DestroyMesh(m);
	cropMeshes.clear();
}

void UndertimeModule::UpdateFarm(Engine& engine, float dt)
{
	farmTimer -= dt;
	if (farmTimer > 0.0f) return;
	farmTimer = 0.25f;
	Scene& scene = engine.GetScene();
	const double now = clock.Days();
	for (auto it = plantings.begin(); it != plantings.end();)
	{
		Planting& p = it->second;
		// The plot was taken down: the crop goes with it.
		const int x = it->first % 256, z = (it->first / 256) % 256, y = it->first / 65536;
		bool plot = false;
		for (const build::Piece& piece : claimPlan.Pieces())
			plot = plot || (piece.x == x && piece.y == y && piece.z == z && piece.Built() && std::string(piece.Def().id) == "farm_plot");
		if (!plot)
		{
			if (scene.IsAlive(p.entity)) scene.DestroyEntity(p.entity);
			it = plantings.erase(it);
			continue;
		}
		const grow::CropState s = grow::CropAt(p.crop, now - p.day, clock.DayOfYear());
		const bool harvestedThisYear = p.harvestedYear == clock.Year();
		int look = s.stage == grow::CropStage::Spoiled ? 5 : s.stage == grow::CropStage::Ripe && !harvestedThisYear ? 4 : std::min(3, static_cast<int>(s.growth * 4.0f));
		const int meshKey = p.crop * 8 + look;
		if (!cropMeshes.count(meshKey)) cropMeshes[meshKey] = kit::Bake(engine, CropModel(p.crop, look), VoxelPivot::Corner);
		if (!scene.IsAlive(p.entity))
			p.entity = kit::Spawn(engine, cropMeshes[meshKey], engine.GetAssets().GetMaterial("Materials/Foliage.mat"),
				{ claim.origin.x + x, claim.origin.y + y + cfg::kVoxelSize, claim.origin.z + z });
		if (p.shown != meshKey)
		{
			p.shown = meshKey;
			scene.Get<MeshRenderer>(p.entity).mesh = cropMeshes[meshKey];
		}
		++it;
	}
}

bool UndertimeModule::FarmInteract(Engine& engine)
{
	if (claim.aimed < 0) return false;
	const build::Piece& plot = claimPlan.Pieces()[claim.aimed];
	if (!plot.Built() || std::string(plot.Def().id) != "farm_plot") return false;
	const int key = PlotKey(plot.x, plot.y, plot.z);
	auto it = plantings.find(key);
	if (it == plantings.end())
	{
		// Plants the first seed you carry.
		for (int c = 0; c < static_cast<int>(std::size(cfg::kCrops)); ++c)
			if (Have(cfg::kCrops[c].seed) > 0)
			{
				Take(cfg::kCrops[c].seed, 1);
				Planting p;
				p.crop = c;
				p.day = clock.Days();
				plantings[key] = p;
				messages.Show(std::string("Planted ") + econ::NameOf(cfg::kCrops[c].seed) + ". Ripe in " + OutsideClock::SpanText(cfg::kCrops[c].ripe) + " of outside time.", 2.5f);
				farmTimer = 0.0f;
				saveRequested = true;
				return true;
			}
		messages.Show("No seeds. The trading post sells them.", 2.0f);
		return true;
	}
	Planting& p = it->second;
	const grow::CropState s = grow::CropAt(p.crop, clock.Days() - p.day, clock.DayOfYear());
	const cfg::Crop& crop = cfg::kCrops[p.crop];
	const bool appleTree = p.crop == 3;
	if (s.stage == grow::CropStage::Ripe && !(appleTree && p.harvestedYear == clock.Year()))
	{
		purse.Add(crop.produce, crop.yield);
		messages.Show("+" + std::to_string(crop.yield) + " " + econ::NameOf(crop.produce) + ".", 2.0f);
		if (appleTree) p.harvestedYear = clock.Year();
		else
		{
			if (engine.GetScene().IsAlive(p.entity)) engine.GetScene().DestroyEntity(p.entity);
			plantings.erase(it);
		}
		farmTimer = 0.0f;
		saveRequested = true;
		return true;
	}
	if (s.stage == grow::CropStage::Spoiled)
	{
		messages.Show("Gone to seed. You clear the plot.", 2.0f);
		if (engine.GetScene().IsAlive(p.entity)) engine.GetScene().DestroyEntity(p.entity);
		plantings.erase(it);
		return true;
	}
	messages.Show(std::string(econ::NameOf(crop.produce)) + ": ripe in " + OutsideClock::SpanText(s.ripeIn) + ".", 2.0f);
	return true;
}

std::string UndertimeModule::FarmPrompt() const
{
	if (claim.aimed < 0) return {};
	const build::Piece& plot = claimPlan.Pieces()[claim.aimed];
	if (!plot.Built() || std::string(plot.Def().id) != "farm_plot") return {};
	auto it = plantings.find(PlotKey(plot.x, plot.y, plot.z));
	if (it == plantings.end())
	{
		for (const cfg::Crop& c : cfg::kCrops)
			if (Have(c.seed) > 0) return std::string("E  Plant ") + econ::NameOf(c.seed);
		return "An empty plot (seeds from the trading post)";
	}
	const Planting& p = it->second;
	const grow::CropState s = grow::CropAt(p.crop, clock.Days() - p.day, clock.DayOfYear());
	const std::string name = econ::NameOf(cfg::kCrops[p.crop].produce);
	if (s.stage == grow::CropStage::Ripe && !(p.crop == 3 && p.harvestedYear == clock.Year()))
		return "E  Harvest " + name + " (spoils in " + OutsideClock::SpanText(s.ripeIn) + ")";
	if (s.stage == grow::CropStage::Spoiled) return "E  Clear the spoiled " + name;
	return name + ": ripe in " + OutsideClock::SpanText(s.ripeIn);
}

nlohmann::json UndertimeModule::SaveFarm() const
{
	nlohmann::json j = nlohmann::json::array();
	for (const auto& [key, p] : plantings) j.push_back({ key, p.crop, p.day, p.harvestedYear });
	return j;
}

void UndertimeModule::LoadFarm(Engine& engine, const nlohmann::json& j)
{
	Scene& scene = engine.GetScene();
	for (auto& [key, p] : plantings) if (scene.IsAlive(p.entity)) scene.DestroyEntity(p.entity);
	plantings.clear();
	if (!j.is_array()) return;
	for (const auto& e : j)
	{
		if (!e.is_array() || e.size() < 3) continue;
		Planting p;
		p.crop = std::clamp(e[1].get<int>(), 0, static_cast<int>(std::size(cfg::kCrops)) - 1);
		p.day = e[2].get<double>();
		if (e.size() > 3) p.harvestedYear = e[3].get<int>();
		plantings[e[0].get<int>()] = p;
	}
	farmTimer = 0.0f;
}
