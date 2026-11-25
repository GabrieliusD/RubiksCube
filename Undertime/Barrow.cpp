// The wheelbarrow (Docs/Hauling.md H2): bought at the trading post, it waits by the yard's
// gate. E takes the handles (it rolls along in front of you and its load slows you), E again
// lets go. Parked within reach, the shovel fills it instead of the pack (B switches), R empties
// the pack's stone into it, and Q, while you hold it, tips the load out in front: the blocks
// come out live and settle, so tipped in the yard they're paid for.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include <Core/Log.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr float kVoxel = 0.125f;   // the barrow is finer than the world's blocks
	constexpr int kSX = 9, kSY = 10, kSZ = 12;
	// Tray interior, in the model's voxels.
	constexpr int kInX0 = 2, kInX1 = 7, kInZ0 = 4, kInZ1 = 9, kRim = 5;

	VoxelModel BarrowModel()
	{
		kit::Builder b(kSX, kSY, kSZ);
		const uint8_t wood = b.Colour(kit::colour::kWood), dark = b.Colour(kit::colour::kWoodDark), light = b.Colour(kit::colour::kWoodLight);
		const uint8_t iron = b.Colour(kit::colour::kIron), ironDark = b.Colour(kit::colour::kIronDark);
		// The wheel at the front, under the tray's nose.
		b.Box(4, 1, 9, 5, 4, 12, ironDark);
		b.Box(4, 0, 10, 5, 5, 11, ironDark);
		b.Box(4, 2, 10, 5, 3, 11, iron);
		// Legs at the back.
		b.Box(1, 0, 3, 2, 3, 4, dark);
		b.Box(7, 0, 3, 8, 3, 4, dark);
		// The tray: floor and walls.
		b.Speckle(1, 2, 3, 8, 3, 10, { wood, light });
		b.Speckle(1, 3, 3, 2, kRim, 10, { wood, light }, 1);
		b.Speckle(7, 3, 3, 8, kRim, 10, { wood, light }, 2);
		b.Speckle(1, 3, 3, 8, kRim, 4, { wood, light }, 3);
		b.Speckle(1, 3, 9, 8, kRim, 10, { wood, light }, 4);
		b.Box(1, kRim - 1, 3, 8, kRim, 4, iron);   // iron lip at the back
		// Handles out the back.
		b.Box(1, 3, 0, 2, 4, 3, dark);
		b.Box(7, 3, 0, 8, 4, 3, dark);
		return b.model;
	}

	// The load heaped in the tray: a mound that rises with the fill, coloured by what's in it.
	VoxelModel LoadModel(const LooseBlocks::Counts& load, int capacity)
	{
		kit::Builder b(kSX, kSY, kSZ);
		int total = 0;
		for (int n : load) total += n;
		if (total <= 0) return b.model;
		// The colours, in proportion to the load (stone in three shades).
		std::vector<uint8_t> mix;
		const uint8_t stone[3] = { b.Colour(0x6a5c4c), b.Colour(0x5a4e40), b.Colour(0x786856) };   // as mined stone near the mouth
		for (int k = 0; k < 12; ++k)
			for (int t = 1; t < static_cast<int>(Block::Count); ++t)
			{
				if (load[t] == 0) continue;
				const int share = std::max(1, load[t] * 12 / total);
				if (k >= share) continue;
				mix.push_back(static_cast<Block>(t) == Block::Stone ? stone[k % 3] : b.Colour(cfg::Info(static_cast<Block>(t)).color));
			}
		const float fill = std::min(1.0f, static_cast<float>(total) / capacity);
		const float peak = 0.5f + fill * 3.0f;   // voxels above the tray floor at the middle
		const float cx = (kInX0 + kInX1 - 1) * 0.5f, cz = (kInZ0 + kInZ1 - 1) * 0.5f;
		for (int x = kInX0; x < kInX1; ++x)
			for (int z = kInZ0; z < kInZ1; ++z)
			{
				const float d = std::sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz)) / 3.2f;
				const int top = 3 + static_cast<int>(std::lround(std::max(0.0f, peak * (1.0f - d * d * 0.45f) + fill * 1.0f)));
				b.Speckle(x, 3, z, x + 1, std::min(top, kSY), z + 1, mix, static_cast<uint32_t>(total));
			}
		return b.model;
	}

	XMFLOAT3 Forward(float yaw) { return { std::sin(yaw), 0.0f, std::cos(yaw) }; }
}

void UndertimeModule::StartBarrow(Engine& engine)
{
	barrow.mesh = kit::Bake(engine, BarrowModel(), VoxelPivot::BottomCenter, kVoxel);
	barrow.owned = false;
	barrow.held = false;
	barrow.load = {};
	barrow.loadShown = -1;
}

void UndertimeModule::StopBarrow(Engine& engine)
{
	Scene& scene = engine.GetScene();
	for (Entity e : { barrow.entity, barrow.loadEntity })
		if (scene.IsAlive(e)) scene.DestroyEntity(e);
	barrow.entity = barrow.loadEntity = INVALID_ENTITY;
	Renderer& renderer = engine.GetRenderer();
	if (barrow.mesh.IsValid()) renderer.DestroyMesh(barrow.mesh);
	if (barrow.loadMesh.IsValid()) renderer.DestroyMesh(barrow.loadMesh);
	barrow.mesh = barrow.loadMesh = {};
	if (barrow.body != kInvalidBody) engine.GetPhysics().RemoveBody(barrow.body);
	barrow.body = kInvalidBody;
}

void UndertimeModule::ResetBarrow(Engine& engine)
{
	Scene& scene = engine.GetScene();
	for (Entity e : { barrow.entity, barrow.loadEntity })
		if (scene.IsAlive(e)) scene.DestroyEntity(e);
	barrow.entity = barrow.loadEntity = INVALID_ENTITY;
	if (barrow.body != kInvalidBody) engine.GetPhysics().RemoveBody(barrow.body);
	barrow.body = kInvalidBody;
	barrow.owned = barrow.held = false;
	barrow.load = {};
	barrow.tipping = -1.0f;
	barrow.loadShown = -1;
}

void UndertimeModule::PlaceBarrow(Engine& engine, const XMFLOAT3& at, float yaw)
{
	barrow.owned = true;
	barrow.position = at;
	barrow.yaw = yaw;
	Scene& scene = engine.GetScene();
	const MaterialHandle material = engine.GetAssets().GetMaterial("Materials/Town.mat");
	if (!scene.IsAlive(barrow.entity)) barrow.entity = kit::Spawn(engine, barrow.mesh, material, at, yaw);
	if (!scene.IsAlive(barrow.loadEntity))
	{
		barrow.loadEntity = kit::Spawn(engine, {}, material, at, yaw);
		scene.Get<MeshRenderer>(barrow.loadEntity).visible = false;
	}
	barrow.loadShown = -1;
	ParkBarrow(engine);
}

void UndertimeModule::BuyBarrow(Engine& engine)
{
	// It waits outside the yard's gate.
	const float x = cfg::kYardCenter[0] + cfg::kYardHalf[0] + 1.4f, z = cfg::kYardCenter[1];
	const float y = valley ? valley->Height(x, z) : cfg::kGroundY;
	PlaceBarrow(engine, { x, y, z }, -XM_PIDIV2);
	messages.Show("Your wheelbarrow is waiting by the yard gate.", 3.0f);
}

void UndertimeModule::ParkBarrow(Engine& engine)
{
	PhysicsWorld& physics = engine.GetPhysics();
	if (barrow.body != kInvalidBody) physics.RemoveBody(barrow.body);
	// An upright box around it (turned boxes aren't needed: it's small).
	const XMFLOAT3 f = Forward(barrow.yaw);
	const float hx = std::fabs(f.x) * 0.7f + std::fabs(f.z) * 0.5f, hz = std::fabs(f.z) * 0.7f + std::fabs(f.x) * 0.5f;
	barrow.body = physics.CreateStaticBox({ barrow.position.x, barrow.position.y + 0.32f, barrow.position.z }, { hx, 0.32f, hz });
	barrow.held = false;
}

int UndertimeModule::BarrowCount() const
{
	int n = 0;
	for (int c : barrow.load) n += c;
	return n;
}

float UndertimeModule::BarrowSpeedScale() const
{
	if (!barrow.held) return 1.0f;
	const float fill = std::min(1.0f, static_cast<float>(BarrowCount()) / cfg::kBarrowCapacity);
	return 1.0f - (1.0f - cfg::kBarrowSlowFull) * fill;
}

bool UndertimeModule::BarrowTakesScoops(XMFLOAT3& into) const
{
	if (!barrow.owned || barrow.held || barrow.fillPack || BarrowCount() >= cfg::kBarrowCapacity) return false;
	const float dx = barrow.position.x - player.position.x, dz = barrow.position.z - player.position.z;
	if (dx * dx + dz * dz > (cfg::kBarrowReach + 1.0f) * (cfg::kBarrowReach + 1.0f)) return false;
	into = { barrow.position.x, barrow.position.y + 0.6f, barrow.position.z };
	return true;
}

void UndertimeModule::AddToBarrow(const LooseBlocks::Counts& counts)
{
	for (size_t b = 0; b < counts.size(); ++b) barrow.load[b] += counts[b];
}

void UndertimeModule::RebuildBarrowLoad(Engine& engine)
{
	const int count = BarrowCount();
	// Rebuilt when the heap visibly changes (every few blocks), not on every scoop.
	const int shown = count == 0 ? 0 : 1 + count * 24 / cfg::kBarrowCapacity;
	if (shown == barrow.loadShown) return;
	barrow.loadShown = shown;
	Renderer& renderer = engine.GetRenderer();
	if (barrow.loadMesh.IsValid()) renderer.DestroyMesh(barrow.loadMesh);
	barrow.loadMesh = {};
	if (count > 0) barrow.loadMesh = kit::Bake(engine, LoadModel(barrow.load, cfg::kBarrowCapacity), VoxelPivot::BottomCenter, kVoxel);
	Scene& scene = engine.GetScene();
	if (scene.IsAlive(barrow.loadEntity))
	{
		MeshRenderer& mr = scene.Get<MeshRenderer>(barrow.loadEntity);
		mr.mesh = barrow.loadMesh;
		mr.visible = barrow.loadMesh.IsValid();
	}
}

void UndertimeModule::UpdateBarrow(Engine& engine, float dt)
{
	if (!barrow.owned) return;
	Input& input = engine.GetInput();
	const float dx = barrow.position.x - player.position.x, dz = barrow.position.z - player.position.z;
	const bool nearBarrow = dx * dx + dz * dz < (cfg::kBarrowReach + 1.0f) * (cfg::kBarrowReach + 1.0f);
	if (nearBarrow && !barrow.held && input.WasKeyPressed('B'))
	{
		barrow.fillPack = !barrow.fillPack;
		messages.Show(barrow.fillPack ? "The shovel fills your pack." : "The shovel fills the barrow.", 1.5f);
	}
	if (nearBarrow && input.WasKeyPressed('R'))
	{
		// The pack's stone into the barrow, as much as fits.
		const int room = cfg::kBarrowCapacity - BarrowCount();
		const int moved = std::min(room, backpack[static_cast<int>(Block::Stone)]);
		if (moved > 0)
		{
			backpack[static_cast<int>(Block::Stone)] -= moved;
			barrow.load[static_cast<int>(Block::Stone)] += moved;
			messages.Show(std::to_string(moved) + " stone into the barrow.", 1.5f);
		}
		else messages.Show(room <= 0 ? "The barrow is full." : "No stone in your pack.", 1.5f);
	}
	if (barrow.held && barrow.tipping < 0.0f && input.WasKeyPressed('Q'))
	{
		if (BarrowCount() > 0) { barrow.tipping = 0.0f; barrow.tipped = 0; }
		else messages.Show("The barrow is empty.", 1.2f);
	}

	if (barrow.held)
	{
		// Rolls along in front of you, on whatever ground is there.
		const XMFLOAT3 f = Forward(player.yaw);
		XMFLOAT3 at{ player.position.x + f.x * cfg::kBarrowHold, player.position.y, player.position.z + f.z * cfg::kBarrowHold };
		if (const auto ground = engine.GetPhysics().Raycast({ at.x, at.y + 1.2f, at.z }, { 0, -1, 0 }, 4.0f)) at.y = ground->point.y;
		barrow.position = at;
		barrow.yaw = player.yaw;
	}

	// Tipping out: the load pours from the nose over kBarrowTipTime.
	if (barrow.tipping >= 0.0f)
	{
		barrow.tipping += dt;
		const int total = BarrowCount() + barrow.tipped;
		const int due = std::min(total, static_cast<int>(std::ceil(total * std::min(1.0f, barrow.tipping / cfg::kBarrowTipTime))));
		const XMFLOAT3 f = Forward(barrow.yaw);
		uint32_t h = static_cast<uint32_t>(barrow.tipped) * 2654435761u + 77u;
		auto random = [&h]() { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return (h & 0xFFFF) / 32767.5f - 1.0f; };
		while (barrow.tipped < due)
		{
			// Takes one block of whatever there's most of.
			int type = 0;
			for (int t = 1; t < static_cast<int>(Block::Count); ++t) if (barrow.load[t] > barrow.load[type]) type = t;
			if (barrow.load[type] <= 0) break;
			--barrow.load[type];
			++barrow.tipped;
			const XMFLOAT3 c{ barrow.position.x + f.x * 1.05f + random() * 0.35f, barrow.position.y + 0.55f + random() * 0.2f + 0.25f, barrow.position.z + f.z * 1.05f + random() * 0.35f };
			const XMFLOAT3 v{ f.x * 1.6f + random() * 0.6f, 0.4f + random() * 0.3f, f.z * 1.6f + random() * 0.6f };
			blocks.SpawnFree(engine, static_cast<Block>(type), cfg::kVoxX / 2, 4, 2, c, v);
		}
		if (barrow.tipping >= cfg::kBarrowTipTime || BarrowCount() == 0)
		{
			barrow.tipping = -1.0f;
			messages.Show("Tipped out " + std::to_string(barrow.tipped) + " blocks.", 1.5f);
		}
	}
}

void UndertimeModule::AfterBarrow(Engine& engine, float)
{
	if (!barrow.owned) return;
	// Never lost: fallen out of the world, it's back at the mine mouth.
	if (barrow.position.y < cfg::kBarrowRescueY)
	{
		barrow.held = false;
		barrow.position = { cfg::kSpawnX + 2.0f, cfg::kGroundY, cfg::kSpawnZ - 2.0f };
		ParkBarrow(engine);
	}
	RebuildBarrowLoad(engine);
	Scene& scene = engine.GetScene();
	XMFLOAT4 rotation;
	// Held, the handles come up and the nose dips a little.
	XMStoreFloat4(&rotation, XMQuaternionRotationRollPitchYaw(barrow.held ? (barrow.tipping >= 0.0f ? 0.55f : 0.08f) : 0.0f, barrow.yaw, 0.0f));
	for (Entity e : { barrow.entity, barrow.loadEntity })
	{
		if (!scene.IsAlive(e)) continue;
		Transform& t = scene.Get<Transform>(e);
		t.position = barrow.position;
		t.rotation = rotation;
	}
}

bool UndertimeModule::BarrowInteract(Engine& engine)
{
	if (!barrow.owned) return false;
	if (barrow.held)
	{
		if (barrow.tipping >= 0.0f) return true;
		ParkBarrow(engine);
		return true;
	}
	const XMFLOAT3 handles{ barrow.position.x, barrow.position.y + 0.45f, barrow.position.z };
	if (!AimedAt(handles, cfg::kBarrowReach)) return false;
	if (barrow.body != kInvalidBody) engine.GetPhysics().RemoveBody(barrow.body);
	barrow.body = kInvalidBody;
	barrow.held = true;
	// Hands on the handles: the pick and shovel wait.
	swingTimer = shovelTimer = 0.0f;
	scoopPreview.clear();
	return true;
}

std::string UndertimeModule::BarrowPrompt() const
{
	if (!barrow.owned) return {};
	if (barrow.held) return barrow.tipping >= 0.0f ? "Tipping..." : "E  Let go      Q  Tip it out";
	const XMFLOAT3 handles{ barrow.position.x, barrow.position.y + 0.45f, barrow.position.z };
	if (!AimedAt(handles, cfg::kBarrowReach)) return {};
	return std::string("E  Push the barrow      R  Pack's stone into it      B  Shovel fills ") + (barrow.fillPack ? "the barrow" : "your pack");
}

void UndertimeModule::BarrowHud(std::vector<std::string>& bag) const
{
	if (!barrow.owned) return;
	char line[64];
	std::snprintf(line, sizeof(line), "Barrow  %d / %d%s", BarrowCount(), cfg::kBarrowCapacity, barrow.held ? "  (pushing)" : "");
	bag.push_back(line);
}

nlohmann::json UndertimeModule::SaveBarrow() const
{
	nlohmann::json j;
	j["owned"] = barrow.owned;
	if (!barrow.owned) return j;
	j["at"] = { barrow.position.x, barrow.position.y, barrow.position.z };
	j["yaw"] = barrow.yaw;
	std::map<std::string, int> load;
	for (int b = 1; b < static_cast<int>(Block::Count); ++b)
		if (barrow.load[b] > 0) load[cfg::Info(static_cast<Block>(b)).name] += barrow.load[b];
	j["load"] = load;
	return j;
}

void UndertimeModule::LoadBarrow(Engine& engine, const nlohmann::json& j)
{
	if (!j.is_object() || !j.value("owned", false)) return;
	XMFLOAT3 at{ cfg::kSpawnX + 2.0f, cfg::kGroundY, cfg::kSpawnZ - 2.0f };
	if (j.contains("at") && j["at"].is_array() && j["at"].size() == 3) at = { j["at"][0].get<float>(), j["at"][1].get<float>(), j["at"][2].get<float>() };
	barrow.load = {};
	if (j.contains("load"))
		for (const auto& [name, n] : j["load"].get<std::map<std::string, int>>())
		{
			const Block b = econ::BlockNamed(name);
			if (b != Block::Air) barrow.load[static_cast<int>(b)] += n;
		}
	PlaceBarrow(engine, at, j.value("yaw", 0.0f));
}
