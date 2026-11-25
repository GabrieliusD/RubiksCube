// Things you place in the world with the hammer (Docs/Hauling.md H4b), anywhere, not only on
// your claim: timber props hold the cave roof (a region touching one never caves in), ladders
// climb walls, wooden chutes let blocks slide down, torches light the dark for a couple of
// hours of their own time. Machines (Machines.cpp) are placed the same way. Pick one of your
// things from the hammer's list (right mouse), aim, left mouse places it; X held picks it up.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include "Structure.h"
#include "ColorUtil.h"
#include <Core/Log.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr float kFine = 0.125f;   // ladders and torches are finer than the world's blocks

	VoxelModel PropModel(int heightVoxels)
	{
		kit::Builder b(3, std::max(heightVoxels, 2), 1);
		const uint8_t wood = b.Colour(kit::colour::kWood), dark = b.Colour(kit::colour::kWoodDark);
		b.Speckle(1, 0, 0, 2, heightVoxels - 1, 1, { wood, dark });
		b.Box(0, heightVoxels - 1, 0, 3, heightVoxels, 1, dark);   // the cap under the roof
		return b.model;
	}

	VoxelModel LadderModel(int heightVoxels)
	{
		kit::Builder b(6, heightVoxels, 1);
		const uint8_t wood = b.Colour(kit::colour::kWood), light = b.Colour(kit::colour::kWoodLight);
		b.Box(0, 0, 0, 1, heightVoxels, 1, wood);
		b.Box(5, 0, 0, 6, heightVoxels, 1, wood);
		for (int y = 2; y < heightVoxels; y += 3) b.Box(1, y, 0, 5, y + 1, 1, light);
		return b.model;
	}

	VoxelModel ChuteModel()
	{
		// 0.75 m wide, 1 m long, dropping 1 m towards +z (a staircase look; the collision is a smooth ramp).
		kit::Builder b(3, 5, 4);
		const uint8_t wood = b.Colour(kit::colour::kWood), light = b.Colour(kit::colour::kWoodLight), dark = b.Colour(kit::colour::kWoodDark);
		for (int z = 0; z < 4; ++z)
		{
			const int floor = 3 - z;
			b.Speckle(0, floor, z, 3, floor + 1, z + 1, { wood, light });
			b.Box(0, floor + 1, z, 1, floor + 2, z + 1, dark);
			b.Box(2, floor + 1, z, 3, floor + 2, z + 1, dark);
			b.Box(1, 0, z, 2, floor, z + 1, dark);   // the trestle under it
		}
		return b.model;
	}

	VoxelModel TorchModel(bool burning)
	{
		kit::Builder b(1, 5, 1);
		b.Box(0, 0, 0, 1, 4, 1, b.Colour(kit::colour::kWoodDark));
		b.Box(0, 4, 0, 1, 5, 1, burning ? b.Colour(0xffb050, 4.0f) : b.Colour(0x2a2420));
		return b.model;
	}
}

MeshHandle UndertimeModule::PlacedMeshFor(Engine& engine, const std::string& key, const VoxelModel& model)
{
	auto it = placedMeshes.find(key);
	if (it != placedMeshes.end()) return it->second;
	return placedMeshes[key] = kit::Bake(engine, model, VoxelPivot::BottomCenter);
}

double UndertimeModule::OwnRate(const XMFLOAT3& at) const
{
	// Outside seconds per real second where you are, over the same where it stands.
	const float there = OutsideClock::Rate(InCave(at), at.z, ZoneStarts());
	return there > 0.0f ? timeRate / there : 1.0;
}

MeshHandle UndertimeModule::PlacedMesh(Engine& engine, const std::string& item, float height)
{
	const int h = static_cast<int>(std::lround(height / cfg::kVoxelSize));
	const std::string key = item + "#" + std::to_string(h);
	auto it = placedMeshes.find(key);
	if (it != placedMeshes.end()) return it->second;
	MeshHandle mesh;
	if (item == "prop") mesh = kit::Bake(engine, PropModel(std::max(h, 2)), VoxelPivot::BottomCenter);
	else if (item == "ladder") mesh = kit::Bake(engine, LadderModel(std::max(h * 2, 4)), VoxelPivot::BottomCenter, kFine);
	else if (item == "chute") mesh = kit::Bake(engine, ChuteModel(), VoxelPivot::BottomCenter);
	else if (item == "torch") mesh = kit::Bake(engine, TorchModel(true), VoxelPivot::BottomCenter, kFine);
	else if (item == "torch_out") mesh = kit::Bake(engine, TorchModel(false), VoxelPivot::BottomCenter, kFine);
	else mesh = MachineMesh(engine, item);
	placedMeshes[key] = mesh;
	return mesh;
}

void UndertimeModule::SpawnPlaced(Engine& engine, Placed& p)
{
	Scene& scene = engine.GetScene();
	PhysicsWorld& physics = engine.GetPhysics();
	const MaterialHandle material = engine.GetAssets().GetMaterial("Materials/Town.mat");
	const bool burntOut = p.item == "torch" && p.ownTime >= cfg::kTorchHours * 3600.0;
	p.entity = kit::Spawn(engine, PlacedMesh(engine, burntOut ? "torch_out" : p.item, p.height), material, p.at, p.yaw);
	const XMFLOAT3 f{ std::sin(p.yaw), 0.0f, std::cos(p.yaw) };
	if (p.item == "prop")
	{
		p.body = physics.CreateStaticBox({ p.at.x, p.at.y + p.height * 0.5f, p.at.z }, { 0.125f, p.height * 0.5f, 0.125f });
		const float s = cfg::kVoxelSize;
		const int vx = static_cast<int>(std::floor(p.at.x / s)), vy = static_cast<int>(std::floor((p.at.y + p.height + 0.05f) / s)), vz = static_cast<int>(std::floor(p.at.z / s));
		if (VoxelWorld::InGrid(vx, vy, vz)) { p.anchor = SupportKey(vx, vy, vz); propHeld.insert(p.anchor); }
	}
	else if (p.item == "chute")
	{
		// A smooth ramp with low sides: the floor drops 1 m over its length (towards +z, turned).
		const float w = 0.375f, l = 0.5f;
		auto at = [&](float x, float y, float z) { return XMFLOAT3{ p.at.x + f.z * x + f.x * z, p.at.y + y, p.at.z - f.x * x + f.z * z }; };
		std::vector<XMFLOAT3> tris;
		auto quad = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d) { tris.insert(tris.end(), { a, b, c, a, c, d }); tris.insert(tris.end(), { a, c, b, a, d, c }); };
		quad(at(-w, 1.0f, -l), at(w, 1.0f, -l), at(w, 0.0f, l), at(-w, 0.0f, l));
		quad(at(-w, 1.0f, -l), at(-w, 1.3f, -l), at(-w, 0.3f, l), at(-w, 0.0f, l));
		quad(at(w, 1.0f, -l), at(w, 1.3f, -l), at(w, 0.3f, l), at(w, 0.0f, l));
		p.body = physics.CreateStaticMesh(tris);
	}
	else if (p.item == "torch" && !burntOut)
	{
		p.light = scene.CreateEntity();
		Transform t;
		t.position = { p.at.x, p.at.y + 0.7f, p.at.z };
		scene.Add(p.light, t);
		PointLightComponent light;
		const XMFLOAT4 c = color::WithAlpha(color::Linear(cfg::kTorchColor));
		light.color = { c.x, c.y, c.z };
		light.intensity = cfg::kTorchIntensity;
		light.range = cfg::kTorchRange;
		scene.Add(p.light, light);
	}
	else if (IsMachine(p.item)) SpawnMachine(engine, p);
}

void UndertimeModule::DespawnPlaced(Engine& engine, Placed& p)
{
	Scene& scene = engine.GetScene();
	for (Entity* e : { &p.entity, &p.light, &p.extra })
	{
		if (scene.IsAlive(*e)) scene.DestroyEntity(*e);
		*e = INVALID_ENTITY;
	}
	if (p.body != kInvalidBody) engine.GetPhysics().RemoveBody(p.body);
	p.body = kInvalidBody;
	if (p.anchor != UINT32_MAX)
	{
		// Another prop may hold the same voxel.
		bool other = false;
		for (const Placed& q : placed) other = other || (&q != &p && q.anchor == p.anchor);
		if (!other) propHeld.erase(p.anchor);
		p.anchor = UINT32_MAX;
	}
}

void UndertimeModule::RemovePlacedAt(Engine& engine, int index, bool giveBack)
{
	Placed p = placed[index];
	DespawnPlaced(engine, placed[index]);
	placed.erase(placed.begin() + index);
	if (!giveBack) return;
	const bool spent = p.item == "torch" && p.ownTime >= cfg::kTorchHours * 3600.0;
	if (!spent) purse.Add(p.item, 1);
	// What a machine held comes out too.
	for (int b = 1; b < static_cast<int>(Block::Count); ++b) if (p.hopper[b] > 0) backpack[b] += p.hopper[b];
	if (p.fuel >= 1.0f) purse.Add("coal", static_cast<int>(p.fuel));
	messages.Show(spent ? "The burnt-out torch crumbles." : std::string("Picked up the ") + econ::NameOf(p.item) + ".", 1.5f);
}

void UndertimeModule::StopPlaced(Engine& engine)
{
	for (Placed& p : placed) DespawnPlaced(engine, p);
	placed.clear();
	propHeld.clear();
	for (auto& [key, mesh] : placedMeshes) if (mesh.IsValid()) engine.GetRenderer().DestroyMesh(mesh);
	placedMeshes.clear();
	climbLadder = -1;
	player.climbing = false;
}

void UndertimeModule::UpdatePlacing(Engine& engine, float dt)
{
	Input& input = engine.GetInput();
	if (purse.Count(placeItem) <= 0)
	{
		placeItem.clear();
		return;
	}
	const XMFLOAT3 eye = player.EyePosition(), dir = player.LookDirection();
	const float start = RayStartOutsidePlayer();
	const XMFLOAT3 from{ eye.x + dir.x * start, eye.y + dir.y * start, eye.z + dir.z * start };
	const auto hit = engine.GetPhysics().Raycast(from, dir, cfg::kPlaceReach - start);
	claim.candidateValid = false;
	claim.why = "aim at the ground or a wall";
	if (!hit) return;
	const XMFLOAT3 p = hit->point, n = hit->normal;
	Placed thing;
	thing.item = placeItem;
	std::string why;
	const float s = cfg::kVoxelSize;
	auto snap = [s](float v) { return (std::floor(v / s) + 0.5f) * s; };
	const bool floor = n.y > 0.7f, wall = std::fabs(n.y) < 0.35f;
	if (placeItem == "prop")
	{
		if (!floor) why = "props stand on the floor";
		else
		{
			thing.at = { snap(p.x), p.y, snap(p.z) };
			const VoxelWorld::RayHit up = world.Raycast({ thing.at.x, p.y + 0.05f, thing.at.z }, { 0, 1, 0 }, cfg::kPropMaxHeight);
			if (!up.hit || !up.inGrid) why = "no rock roof above to hold";
			else thing.height = up.distance + 0.05f;
		}
	}
	else if (placeItem == "ladder")
	{
		if (!wall) why = "ladders go against a wall";
		else
		{
			const XMFLOAT3 out{ p.x + n.x * 0.12f, p.y, p.z + n.z * 0.12f };
			const auto down = engine.GetPhysics().Raycast({ out.x, p.y + 0.1f, out.z }, { 0, -1, 0 }, 4.0f);
			const float base = down ? down->point.y : p.y - 1.0f;
			const VoxelWorld::RayHit up = world.Raycast({ out.x, base + 0.1f, out.z }, { 0, 1, 0 }, cfg::kLadderHeight);
			thing.at = { out.x, base, out.z };
			thing.height = up.hit ? std::max(1.0f, up.distance - 0.1f) : cfg::kLadderHeight;
			thing.yaw = std::atan2(n.x, n.z);
		}
	}
	else if (placeItem == "chute")
	{
		if (!floor) why = "chutes stand on the floor";
		else
		{
			thing.at = { snap(p.x), p.y, snap(p.z) };
			thing.yaw = std::round(player.yaw / XM_PIDIV2) * XM_PIDIV2;
		}
	}
	else if (placeItem == "torch")
	{
		if (!floor && !wall) why = "torches go on the floor or a wall";
		else
		{
			thing.at = { p.x + n.x * 0.06f, floor ? p.y : p.y - 0.3f, p.z + n.z * 0.06f };
			if (wall) thing.yaw = std::atan2(n.x, n.z);
		}
	}
	else if (IsMachine(placeItem)) PlaceMachine(engine, placeItem, p, n, thing, why);
	else why = "can't be placed";
	claim.why = why;
	claim.candidateValid = why.empty();
	engine.GetRenderer().Debug().Box(BoundingBox({ thing.at.x, thing.at.y + std::max(thing.height, 0.5f) * 0.5f, thing.at.z }, { 0.3f, std::max(thing.height, 0.5f) * 0.5f, 0.3f }),
		claim.candidateValid ? DebugDraw::Color(0.6f, 1.0f, 0.6f) : DebugDraw::Color(0.95f, 0.35f, 0.25f));
	if (claim.candidateValid && input.WasMousePressed(MouseButton::Left) && !waitForRelease)
	{
		purse.Add(placeItem, -1);
		placed.push_back(thing);
		SpawnPlaced(engine, placed.back());
		messages.Show(std::string(econ::NameOf(placeItem)) + " placed.", 1.2f);
		saveRequested = true;
	}
	(void)dt;
}

void UndertimeModule::UpdatePlaced(Engine& engine, float dt)
{
	Scene& scene = engine.GetScene();
	Input& input = engine.GetInput();
	// What the crosshair is on (for E and X): the nearest placed thing along the view.
	placedAimed = -1;
	if (state == State::Playing)
	{
		const XMFLOAT3 eye = player.EyePosition(), dir = player.LookDirection();
		float best = cfg::kPlaceReach;
		for (size_t i = 0; i < placed.size(); ++i)
		{
			const Placed& p = placed[i];
			const float h = std::max(p.height, IsMachine(p.item) ? 1.2f : 0.8f);
			const float r = IsMachine(p.item) ? 0.8f : 0.4f;
			float distance = 0.0f;
			if (BoundingBox({ p.at.x, p.at.y + h * 0.5f, p.at.z }, { r, h * 0.5f, r }).Intersects(XMLoadFloat3(&eye), XMLoadFloat3(&dir), distance) && distance < best)
			{
				best = distance;
				placedAimed = static_cast<int>(i);
			}
		}
		// X held picks it up (with the hammer).
		if (tool == Tool::Hammer && placedAimed >= 0 && input.IsKeyDown('X'))
		{
			placedTakeDown += dt;
			if (placedTakeDown >= cfg::kTakeDownTime)
			{
				placedTakeDown = 0.0f;
				RemovePlacedAt(engine, placedAimed, true);
				placedAimed = -1;
				saveRequested = true;
			}
		}
		else placedTakeDown = 0.0f;
	}

	// Torches burn in their own time, and go out.
	for (Placed& p : placed)
	{
		if (p.item != "torch" || p.ownTime >= cfg::kTorchHours * 3600.0) continue;
		p.ownTime += dt * OwnRate(p.at);
		if (scene.IsAlive(p.light))
		{
			const double left = 1.0 - p.ownTime / (cfg::kTorchHours * 3600.0);
			scene.Get<PointLightComponent>(p.light).intensity = cfg::kTorchIntensity * static_cast<float>(std::min(1.0, left * 8.0)) * (0.92f + 0.08f * std::sin(time * 13.0f + p.at.x));
		}
		if (p.ownTime >= cfg::kTorchHours * 3600.0)
		{
			DespawnPlaced(engine, p);
			SpawnPlaced(engine, p);   // the burnt-out look
		}
	}

	// Ladders: walking into one while facing it, W climbs; at the top you step off onto the ledge.
	if (state == State::Playing && !player.flying)
	{
		if (climbLadder >= static_cast<int>(placed.size())) climbLadder = -1;
		if (climbLadder < 0)
		{
			for (size_t i = 0; i < placed.size(); ++i)
			{
				const Placed& p = placed[i];
				if (p.item != "ladder") continue;
				const float dx = player.position.x - p.at.x, dz = player.position.z - p.at.z;
				const XMFLOAT3 look = player.LookDirection();
				const float facing = -(look.x * std::sin(p.yaw) + look.z * std::cos(p.yaw));
				if (dx * dx + dz * dz < 0.55f * 0.55f && player.position.y > p.at.y - 0.3f && player.position.y < p.at.y + p.height - 0.5f && facing > 0.3f && input.IsKeyDown('W'))
				{
					climbLadder = static_cast<int>(i);
					player.climbing = true;
				}
			}
		}
		else
		{
			const Placed& p = placed[climbLadder];
			if (player.position.y >= p.at.y + p.height - 0.2f)
			{
				// Off the top, onto the ledge behind it, if there's room.
				const XMFLOAT3 onto{ p.at.x - std::sin(p.yaw) * 0.6f, p.at.y + p.height + 0.05f, p.at.z - std::cos(p.yaw) * 0.6f };
				const float s = cfg::kVoxelSize;
				const bool room = !world.IsSolid(static_cast<int>(onto.x / s), static_cast<int>((onto.y + 0.3f) / s), static_cast<int>(onto.z / s));
				player.climbing = false;
				climbLadder = -1;
				if (room) player.Teleport(onto, player.yaw);
			}
			else if (player.position.y < p.at.y - 0.1f || input.WasKeyPressed(VK_SPACE))
			{
				player.climbing = false;
				climbLadder = -1;
			}
		}
	}

	UpdateMachines(engine, dt);
}

bool UndertimeModule::PlacedInteract(Engine& engine)
{
	if (placedAimed < 0) return false;
	Placed& p = placed[placedAimed];
	if (IsMachine(p.item)) return MachineInteract(engine, p);
	return false;
}

std::string UndertimeModule::PlacedPrompt() const
{
	if (tool == Tool::Hammer && !placeItem.empty())
		return econ::NameOf(placeItem) + " (" + std::to_string(purse.Count(placeItem)) + ")   " + (claim.candidateValid ? "Left mouse  Place" : claim.why) + "      Right mouse  Pieces";
	if (placedAimed < 0) return {};
	const Placed& p = placed[placedAimed];
	std::string text;
	if (IsMachine(p.item)) text = MachinePrompt(p);
	else if (p.item == "torch") text = p.ownTime >= cfg::kTorchHours * 3600.0 ? "A burnt-out torch" : "Torch: " + OutsideClock::SpanText((cfg::kTorchHours * 3600.0 - p.ownTime) / 86400.0) + " left (its own time)";
	else if (p.item == "ladder") text = "Ladder: walk into it and hold W";
	else if (p.item == "prop") text = "Timber prop: holds the roof above it";
	if (tool == Tool::Hammer) text += (text.empty() ? "" : "      ") + std::string("Hold X  Pick up");
	return text;
}

nlohmann::json UndertimeModule::SavePlaced() const
{
	nlohmann::json j = nlohmann::json::array();
	for (const Placed& p : placed)
	{
		nlohmann::json e;
		e["item"] = p.item;
		e["at"] = { p.at.x, p.at.y, p.at.z };
		e["yaw"] = p.yaw;
		e["height"] = p.height;
		e["time"] = p.ownTime;
		if (p.fuel > 0.0f) e["fuel"] = p.fuel;
		if (p.wear > 0.0) e["wear"] = p.wear;
		if (p.progress > 0.0) e["progress"] = p.progress;
		if (!p.on) e["off"] = true;
		std::map<std::string, int> hopper;
		for (int b = 1; b < static_cast<int>(Block::Count); ++b) if (p.hopper[b] > 0) hopper[cfg::Info(static_cast<Block>(b)).name] += p.hopper[b];
		if (!hopper.empty()) e["hopper"] = hopper;
		j.push_back(e);
	}
	return j;
}

void UndertimeModule::LoadPlaced(Engine& engine, const nlohmann::json& j)
{
	StopPlaced(engine);
	if (!j.is_array()) return;
	for (const auto& e : j)
	{
		Placed p;
		p.item = e.value("item", std::string());
		if (p.item.empty() || !econ::FindItem(p.item)) continue;
		if (e.contains("at") && e["at"].size() == 3) p.at = { e["at"][0].get<float>(), e["at"][1].get<float>(), e["at"][2].get<float>() };
		p.yaw = e.value("yaw", 0.0f);
		p.height = e.value("height", 0.0f);
		p.ownTime = e.value("time", 0.0);
		p.fuel = e.value("fuel", 0.0f);
		p.wear = e.value("wear", 0.0);
		p.progress = e.value("progress", 0.0);
		p.on = !e.value("off", false);
		if (e.contains("hopper"))
			for (const auto& [name, n] : e["hopper"].get<std::map<std::string, int>>())
			{
				const Block b = econ::BlockNamed(name);
				if (b != Block::Air) p.hopper[static_cast<int>(b)] += n;
			}
		placed.push_back(p);
	}
	for (Placed& p : placed) SpawnPlaced(engine, p);
}
