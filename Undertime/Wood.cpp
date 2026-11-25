// Wood (Docs/Hauling.md H6): the axe (key 4, from the smith) fells the valley's trees into logs
// (your bench saws them into timber; the stores buy them). A felled tree leaves a stump that
// grows back from the day it was felled: a sapling after kStumpDays, full grown after
// kRegrowDays of outside time, so long dives regrow the forest. Turning time back past the
// day a tree was felled brings it back (the logs stay yours).
#include "UndertimeModule.h"
#include "ModelKit.h"
#include "Growth.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
	VoxelModel StumpModel()
	{
		kit::Builder b(2, 2, 2);
		const uint8_t bark = b.Colour(0x5a4030), barkDark = b.Colour(0x4a3424), ring = b.Colour(0xc8a070);
		b.Speckle(0, 0, 0, 2, 1, 2, { bark, barkDark });
		b.Box(0, 1, 0, 2, 2, 2, ring);
		return b.model;
	}
}

void UndertimeModule::StartWood(Engine& engine)
{
	stumpMesh = kit::Bake(engine, StumpModel(), VoxelPivot::Corner);
	ResetWood(engine);
}

void UndertimeModule::ResetWood(Engine& engine)
{
	for (const auto& [tree, day] : felled) scenery.SetTreeGrowth(engine.GetScene(), tree, 1.0f);
	for (const auto& [tree, e] : stumps) if (engine.GetScene().IsAlive(e)) engine.GetScene().DestroyEntity(e);
	felled.clear();
	stumps.clear();
	chopTree = -1;
	chops = 0;
}

void UndertimeModule::StopWood(Engine& engine)
{
	ResetWood(engine);
	if (stumpMesh.IsValid()) engine.GetRenderer().DestroyMesh(stumpMesh);
	stumpMesh = {};
}

int UndertimeModule::AimedTree() const
{
	// The trunk nearest the view ray, within reach (trunks are 0.5 m across, ~3.2 m tall).
	const XMFLOAT3 eye = player.EyePosition(), dir = player.LookDirection();
	const auto& trees = scenery.Trees();
	int best = -1;
	float bestAlong = cfg::kAxeReach;
	for (size_t i = 0; i < trees.size(); ++i)
	{
		const ValleyScenery::TreePlace& t = trees[i];
		if (t.scale < 0.5f) continue;
		const float cx = t.at.x + 0.125f, cz = t.at.z + 0.125f;
		const float dx = cx - eye.x, dz = cz - eye.z;
		if (dx * dx + dz * dz > (cfg::kAxeReach + 1.0f) * (cfg::kAxeReach + 1.0f)) continue;
		// Closest approach of the ray to the trunk's axis, in the ground plane.
		const float flat = dir.x * dir.x + dir.z * dir.z;
		if (flat < 1e-4f) continue;
		const float along = (dx * dir.x + dz * dir.z) / flat;
		if (along < 0.0f || along > bestAlong) continue;
		const float px = eye.x + dir.x * along - cx, pz = eye.z + dir.z * along - cz;
		const float y = eye.y + dir.y * along;
		if (px * px + pz * pz > 0.4f * 0.4f || y < t.at.y - 0.2f || y > t.at.y + 3.4f) continue;
		best = static_cast<int>(i);
		bestAlong = along;
	}
	if (best >= 0)
		for (const auto& u : cfg::kUncuttable)
		{
			const float dx = trees[best].at.x - u[0], dz = trees[best].at.z - u[1];
			if (dx * dx + dz * dz < u[2] * u[2]) return -2;   // can't be felled
		}
	return best;
}

void UndertimeModule::UpdateWood(Engine& engine, float dt)
{
	Scene& scene = engine.GetScene();
	Input& input = engine.GetInput();
	axeTarget = -1;
	axeSwinging = false;
	if (tool == Tool::Axe && state == State::Playing && !barrow.held)
	{
		axeTarget = AimedTree();
		if (waitForRelease && !input.IsMouseDown(MouseButton::Left)) waitForRelease = false;
		if (!waitForRelease && input.IsMouseDown(MouseButton::Left))
		{
			axeSwinging = true;
			axeTimer += dt / cfg::kAxePeriod;
			if (input.WasMousePressed(MouseButton::Left)) axeTimer = std::max(axeTimer, 0.6f);
			if (axeTimer >= 1.0f)
			{
				axeTimer = 0.0f;
				if (axeTarget == -2) messages.Show("Not this one. It's the oldest tree in the valley.", 2.0f);
				else if (axeTarget >= 0)
				{
					if (chopTree != axeTarget) { chopTree = axeTarget; chops = 0; }
					++chops;
					hitFeedback = cfg::kHitFeedbackTime;
					hitStrength = 0.8f;
					const ValleyScenery::TreePlace& t = scenery.Trees()[axeTarget];
					const XMFLOAT3 eye = player.EyePosition();
					const XMFLOAT3 at{ t.at.x + 0.125f, std::clamp(eye.y - 0.3f, t.at.y + 0.3f, t.at.y + 2.0f), t.at.z + 0.125f };
					RockDust(engine, at, 10, { 0.55f, 0.4f, 0.25f, 1.0f }, { (eye.x - at.x) * 0.5f, 0.5f, (eye.z - at.z) * 0.5f }, 0.05f);
					const int needed = static_cast<int>(std::ceil(cfg::kAxeChops / Strength()));
					if (chops >= needed)
					{
						// Felled: logs into the pack, a stump left behind.
						felled[axeTarget] = clock.Days();
						scenery.SetTreeGrowth(scene, axeTarget, 0.0f);
						const int logs = cfg::kLogsPerTree[std::clamp(t.kind, 0, 3)];
						purse.Add("logs", logs);
						messages.Show("Timber! +" + std::to_string(logs) + " log" + (logs == 1 ? "" : "s") + ".", 2.0f);
						RockDust(engine, { at.x, t.at.y + 1.0f, at.z }, 30, { 0.4f, 0.5f, 0.3f, 1.0f }, { 0, 1.0f, 0 }, 0.08f);
						chopTree = -1;
						chops = 0;
						saveRequested = true;
					}
				}
			}
		}
		else axeTimer = 0.0f;
	}

	// The felled trees grow back with outside time (a few times a second is plenty).
	woodTimer -= dt;
	if (woodTimer > 0.0f) return;
	woodTimer = 0.25f;
	const double now = clock.Days();
	for (auto it = felled.begin(); it != felled.end();)
	{
		const int tree = it->first;
		const float growth = grow::TreeGrowth(now - it->second);
		const float shown = growth >= 1.0f ? 1.0f : std::floor(growth * 20.0f) / 20.0f;   // in steps, so entities change rarely
		scenery.SetTreeGrowth(scene, tree, shown);
		const bool stump = growth < 0.35f;
		auto s = stumps.find(tree);
		if (stump && s == stumps.end())
		{
			const ValleyScenery::TreePlace& t = scenery.Trees()[tree];
			stumps[tree] = kit::Spawn(engine, stumpMesh, engine.GetAssets().GetMaterial("Materials/Town.mat"), { t.at.x - 0.125f, t.at.y, t.at.z - 0.125f });
		}
		else if (!stump && s != stumps.end())
		{
			if (scene.IsAlive(s->second)) scene.DestroyEntity(s->second);
			stumps.erase(s);
		}
		if (growth >= 1.0f) it = felled.erase(it);   // grown back (or never felled: time was turned back)
		else ++it;
	}
}

std::string UndertimeModule::WoodPrompt() const
{
	if (tool != Tool::Axe) return {};
	if (axeTarget == -2) return "The big tree: not for felling.";
	if (axeTarget < 0) return {};
	const int needed = static_cast<int>(std::ceil(cfg::kAxeChops / Strength()));
	return "Hold left mouse  Chop the tree" + (chopTree == axeTarget && chops > 0 ? " (" + std::to_string(chops) + " / " + std::to_string(needed) + ")" : std::string());
}

nlohmann::json UndertimeModule::SaveWood() const
{
	nlohmann::json j = nlohmann::json::array();
	for (const auto& [tree, day] : felled) j.push_back({ tree, day });
	return j;
}

void UndertimeModule::LoadWood(Engine& engine, const nlohmann::json& j)
{
	ResetWood(engine);
	if (!j.is_array()) return;
	for (const auto& e : j)
		if (e.is_array() && e.size() == 2 && e[0].get<int>() >= 0 && e[0].get<int>() < static_cast<int>(scenery.Trees().size()))
			felled[e[0].get<int>()] = e[1].get<double>();
	woodTimer = 0.0f;
}
