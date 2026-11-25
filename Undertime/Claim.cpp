// Your claim (Docs/Hauling.md H4): a flattened meadow east of the mine with a tent and a
// starter bench, where you build with the hammer (key 3): right mouse picks a piece, a ghost
// shows where it goes (R turns it), left mouse sets it down as a site and builds it up swing
// by swing from your materials, X held takes a piece down (everything back, stone as loose
// blocks). E uses furniture: the bench, a crate, a fire, a bed, a door. The claim is shielded
// from turning time back: nothing here is ever undone.
#include "UndertimeModule.h"
#include "ModelKit.h"
#include <Core/Log.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr float kCell = 1.0f;
	constexpr float kVox = cfg::kVoxelSize;

	VoxelModel TentModel()
	{
		// An A-frame of canvas, 2 m wide and 2.5 m long, open at the front (+z).
		kit::Builder b(9, 7, 10);
		const uint8_t cloth = b.Colour(kit::colour::kCloth), clothDark = b.Colour(0xbcae90), pole = b.Colour(kit::colour::kWoodDark);
		for (int y = 0; y < 6; ++y)
		{
			const int inset = y * 4 / 6;
			b.Speckle(inset, y, 0, inset + 1, y + 1, 10, { cloth, clothDark }, 1);
			b.Speckle(8 - inset, y, 0, 9 - inset, y + 1, 10, { cloth, clothDark }, 2);
		}
		b.Box(4, 5, 0, 5, 7, 10, pole);
		b.Speckle(1, 0, 0, 8, 5, 1, { cloth, clothDark }, 3);   // the back
		b.Box(4, 0, 9, 5, 7, 10, pole);
		return b.model;
	}

	// Triangles for static collision, from the model's boxes.
	std::vector<XMFLOAT3> BoxTriangles(const std::vector<PhysicsBox>& boxes, const XMFLOAT3& offset)
	{
		std::vector<XMFLOAT3> tris;
		tris.reserve(boxes.size() * 36);
		for (const PhysicsBox& b : boxes)
		{
			const XMFLOAT3 c{ b.center.x + offset.x, b.center.y + offset.y, b.center.z + offset.z };
			const XMFLOAT3& h = b.halfExtents;
			XMFLOAT3 v[8];
			for (int i = 0; i < 8; ++i) v[i] = { c.x + (i & 1 ? h.x : -h.x), c.y + (i & 2 ? h.y : -h.y), c.z + (i & 4 ? h.z : -h.z) };
			static const int kFaces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
			for (const auto& f : kFaces)
			{
				tris.push_back(v[f[0]]); tris.push_back(v[f[1]]); tris.push_back(v[f[2]]);
				tris.push_back(v[f[0]]); tris.push_back(v[f[2]]); tris.push_back(v[f[3]]);
			}
		}
		return tris;
	}
}

bool UndertimeModule::OnClaim(const XMFLOAT3& p, float margin) const
{
	const float size = cfg::kClaimCells * kCell;
	return p.x >= claim.origin.x - margin && p.z >= claim.origin.z - margin && p.x <= claim.origin.x + size + margin && p.z <= claim.origin.z + size + margin;
}

void UndertimeModule::StartClaim(Engine& engine)
{
	const float half = cfg::kClaimCells * kCell * 0.5f;
	const float ground = valley ? valley->Height(cfg::kClaimCenter[0], cfg::kClaimCenter[1]) : cfg::kGroundY;
	claim.origin = { cfg::kClaimCenter[0] - half, ground, cfg::kClaimCenter[1] - half };
	// The tent, just south of the plot (so it never gets in the way of building).
	claim.tentMesh = kit::Bake(engine, TentModel(), VoxelPivot::BottomCenter);
	const XMFLOAT3 tentAt{ claim.origin.x + 3.0f, valley ? valley->Height(claim.origin.x + 3.0f, claim.origin.z - 2.0f) : ground, claim.origin.z - 2.0f };
	claim.tent = kit::Spawn(engine, claim.tentMesh, engine.GetAssets().GetMaterial("Materials/Town.mat"), tentAt, XM_PI);
	claim.tentBody = engine.GetPhysics().CreateStaticBox({ tentAt.x, tentAt.y + 0.7f, tentAt.z }, { 1.1f, 0.7f, 1.25f });
	ResetClaim(engine);
}

void UndertimeModule::ResetClaim(Engine& engine)
{
	// A new claim: the starter bench by the tent, ready to use.
	claimPlan = build::Plan(cfg::kClaimCells, cfg::kClaimCells, cfg::kClaimLevels);
	build::Piece bench;
	bench.type = build::PieceIndex("workbench");
	bench.x = 3;
	bench.z = 1;
	bench.turn = 2;
	bench.swings = bench.Def().swings;
	claimPlan.Place(bench);
	claim.store.clear();
	claim.dirty = true;
	claim.ghostKey = -1;
	RebuildClaim(engine);
}

void UndertimeModule::StopClaim(Engine& engine)
{
	Scene& scene = engine.GetScene();
	for (Entity e : { claim.entity, claim.ghost, claim.tent })
		if (scene.IsAlive(e)) scene.DestroyEntity(e);
	for (Entity e : claim.lamps) if (scene.IsAlive(e)) scene.DestroyEntity(e);
	claim.lamps.clear();
	claim.entity = claim.ghost = claim.tent = INVALID_ENTITY;
	Renderer& renderer = engine.GetRenderer();
	for (MeshHandle* m : { &claim.mesh, &claim.ghostMesh, &claim.tentMesh })
	{
		if (m->IsValid()) renderer.DestroyMesh(*m);
		*m = {};
	}
	PhysicsWorld& physics = engine.GetPhysics();
	for (BodyId* b : { &claim.body, &claim.tentBody })
	{
		if (*b != kInvalidBody) physics.RemoveBody(*b);
		*b = kInvalidBody;
	}
}

void UndertimeModule::RebuildClaim(Engine& engine)
{
	if (!claim.dirty) return;
	claim.dirty = false;
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();
	PhysicsWorld& physics = engine.GetPhysics();
	const VoxelModel model = claimPlan.Model(true);
	if (claim.mesh.IsValid()) renderer.DestroyMesh(claim.mesh);
	claim.mesh = {};
	if (claim.body != kInvalidBody) physics.RemoveBody(claim.body);
	claim.body = kInvalidBody;
	if (model.SolidCount() > 0)
	{
		VoxelBakeOptions options;
		options.pivot = VoxelPivot::Corner;
		claim.mesh = renderer.CreateMesh(BakeVoxelMesh(model, options));
		claim.body = physics.CreateStaticMesh(BoxTriangles(BakeVoxelBoxes(model, options), claim.origin));
	}
	const MaterialHandle material = engine.GetAssets().GetMaterial("Materials/Town.mat");
	if (!scene.IsAlive(claim.entity)) claim.entity = kit::Spawn(engine, claim.mesh, material, claim.origin);
	MeshRenderer& mr = scene.Get<MeshRenderer>(claim.entity);
	mr.mesh = claim.mesh;
	mr.visible = claim.mesh.IsValid();

	// A light for each lamp.
	for (Entity e : claim.lamps) if (scene.IsAlive(e)) scene.DestroyEntity(e);
	claim.lamps.clear();
	for (const build::Piece& p : claimPlan.Pieces())
	{
		if (!p.Built() || p.Def().use != build::Use::Lamp) continue;
		const Entity e = scene.CreateEntity();
		Transform t;
		t.position = { claim.origin.x + (p.x + 0.5f) * kCell, claim.origin.y + p.y * kCell + 0.85f, claim.origin.z + (p.z + 0.5f) * kCell };
		scene.Add(e, t);
		PointLightComponent light;
		light.color = { 1.0f, 0.78f, 0.5f };
		light.intensity = 2.2f;
		light.range = 7.0f;
		scene.Add(e, light);
		claim.lamps.push_back(e);
	}
}

int UndertimeModule::BuildHave(const std::string& id) const
{
	int n = Have(id);
	const Block block = econ::BlockNamed(id);
	if (block != Block::Air && barrow.owned && !barrow.held)
	{
		const float dx = barrow.position.x - player.position.x, dz = barrow.position.z - player.position.z;
		if (dx * dx + dz * dz < cfg::kSourceReach * cfg::kSourceReach) n += barrow.load[static_cast<int>(block)];
	}
	if (OnClaim(player.position, 2.0f))
	{
		auto it = claim.store.find(id);
		if (it != claim.store.end()) n += it->second;
	}
	return n;
}

void UndertimeModule::BuildTake(const std::string& id, int count)
{
	// The pack first, then the barrow, then the crates.
	const int fromPack = std::min(count, Have(id));
	Take(id, fromPack);
	count -= fromPack;
	const Block block = econ::BlockNamed(id);
	if (count > 0 && block != Block::Air && barrow.owned)
	{
		int& load = barrow.load[static_cast<int>(block)];
		const int fromBarrow = std::min(count, load);
		load -= fromBarrow;
		count -= fromBarrow;
	}
	if (count > 0)
	{
		auto it = claim.store.find(id);
		if (it != claim.store.end())
		{
			it->second -= std::min(count, it->second);
			if (it->second <= 0) claim.store.erase(it);
		}
	}
}

void UndertimeModule::UpdateClaim(Engine& engine, float dt)
{
	Scene& scene = engine.GetScene();
	Input& input = engine.GetInput();
	const auto& pieces = build::Pieces();
	claim.aimed = -1;
	claim.candidateValid = false;
	claim.why.clear();
	bool showGhost = false;
	if (tool == Tool::Hammer && state == State::Playing && !barrow.held)
	{
		if (input.WasMousePressed(MouseButton::Right))
		{
			OpenMenu(engine, Menu::Pieces);
			return;
		}
		if (input.WasKeyPressed('R')) claim.turn = (claim.turn + 1) & 3;
		if (waitForRelease && !input.IsMouseDown(MouseButton::Left)) waitForRelease = false;
		// One of your things (props, ladders, machines) goes anywhere, not on the claim's grid.
		if (!placeItem.empty())
		{
			UpdatePlacing(engine, dt);
			if (scene.IsAlive(claim.ghost)) scene.Get<MeshRenderer>(claim.ghost).visible = false;
			RebuildClaim(engine);
			return;
		}

		// What the hammer points at.
		const XMFLOAT3 eye = player.EyePosition(), dir = player.LookDirection();
		const float start = RayStartOutsidePlayer();
		const XMFLOAT3 from{ eye.x + dir.x * start, eye.y + dir.y * start, eye.z + dir.z * start };
		const auto hit = engine.GetPhysics().Raycast(from, dir, cfg::kHammerReach - start);
		if (hit && OnClaim(hit->point, 0.6f))
		{
			const XMFLOAT3 p = hit->point, n = hit->normal;
			// The piece under the crosshair: the voxel just inside the surface.
			const XMFLOAT3 in{ p.x - n.x * 0.05f - claim.origin.x, p.y - n.y * 0.05f - claim.origin.y, p.z - n.z * 0.05f - claim.origin.z };
			if (hit->body == claim.body)
				claim.aimed = claimPlan.PieceAt(static_cast<int>(std::floor(in.x / kVox)), static_cast<int>(std::floor(in.y / kVox)), static_cast<int>(std::floor(in.z / kVox)));
			// Where the selected piece would go: the cell just outside the surface.
			const XMFLOAT3 q{ p.x + n.x * 0.05f - claim.origin.x, p.y + n.y * 0.05f - claim.origin.y, p.z + n.z * 0.05f - claim.origin.z };
			build::Piece c;
			c.type = claim.selected;
			c.x = static_cast<int>(std::floor(q.x / kCell));
			c.z = static_cast<int>(std::floor(q.z / kCell));
			const build::PieceDef& def = pieces[claim.selected];
			c.y = std::max(0, static_cast<int>(std::floor((q.y - (def.slot == build::Slot::Floor ? -0.1f : 0.1f)) / kCell)));
			if (def.slot == build::Slot::Edge)
			{
				// The cell's edge nearest the aim.
				const float fx = q.x / kCell - c.x, fz = q.z / kCell - c.z;
				const float d[4] = { fz, 1.0f - fx, 1.0f - fz, fx };   // -z, +x, +z, -x
				c.turn = static_cast<int>(std::min_element(d, d + 4) - d);
			}
			else
			{
				// Facing you, plus R's quarter turns.
				const int facing = static_cast<int>(std::lround(player.yaw / XM_PIDIV2)) + 2;
				c.turn = ((facing + claim.turn) % 4 + 4) % 4;
			}
			claim.candidate = c;
			claim.why = claimPlan.CanPlace(c);
			if (def.eraFrom > era) claim.why = std::string("not until the ") + era::Name(def.eraFrom);
			claim.candidateValid = claim.why.empty();
			const bool aimingAtSite = claim.aimed >= 0 && !claimPlan.Pieces()[claim.aimed].Built();
			showGhost = claim.candidateValid && !aimingAtSite;

			// Outlines: the site you'd build up, or where the new piece goes.
			const float s = kVox;
			auto outline = [&](const build::Piece& piece, uint32_t colour)
			{
				const VoxelModel m = build::Plan::PieceModel(piece);
				const std::array<int, 3> o = build::Plan::PieceOrigin(piece);
				const XMFLOAT3 lo{ claim.origin.x + o[0] * s, claim.origin.y + o[1] * s, claim.origin.z + o[2] * s };
				const XMFLOAT3 size{ m.size.x * s, m.size.y * s, m.size.z * s };
				engine.GetRenderer().Debug().Box(BoundingBox({ lo.x + size.x * 0.5f, lo.y + size.y * 0.5f, lo.z + size.z * 0.5f }, { size.x * 0.5f + 0.01f, size.y * 0.5f + 0.01f, size.z * 0.5f + 0.01f }), colour);
			};
			if (aimingAtSite) outline(claimPlan.Pieces()[claim.aimed], DebugDraw::Color(1.0f, 0.85f, 0.3f));
			else if (claim.aimed >= 0 && input.IsKeyDown('X')) outline(claimPlan.Pieces()[claim.aimed], DebugDraw::Color(0.95f, 0.35f, 0.25f));
			else outline(c, claim.candidateValid ? DebugDraw::Color(0.6f, 1.0f, 0.6f) : DebugDraw::Color(0.95f, 0.35f, 0.25f));

			// Building: a swing every kHammerPeriod while the button is held.
			if (!waitForRelease && input.IsMouseDown(MouseButton::Left))
			{
				claim.swingTimer += dt / cfg::kHammerPeriod;
				if (input.WasMousePressed(MouseButton::Left)) claim.swingTimer = 1.0f;
				if (claim.swingTimer >= 1.0f)
				{
					claim.swingTimer = 0.0f;
					int index = aimingAtSite ? claim.aimed : -1;
					if (index < 0 && claim.candidateValid) index = claimPlan.Place(c);
					else if (index < 0) messages.Show("Can't build there: " + claim.why + ".", 1.5f);
					if (index >= 0)
					{
						build::Piece& site = claimPlan.Pieces()[index];
						const auto cost = build::SwingCost(site.Def(), site.swings);
						std::string missing;
						for (const auto& [id, n] : cost)
							if (BuildHave(id) < n) missing += (missing.empty() ? "" : ", ") + std::to_string(n - BuildHave(id)) + " more " + econ::NameOf(id);
						if (!missing.empty())
						{
							messages.Show("Needs " + missing + ".", 2.0f);
							if (site.swings == 0) claimPlan.Remove(index);   // never started: no site left behind
						}
						else
						{
							for (const auto& [id, n] : cost) BuildTake(id, n);
							++site.swings;
							hitFeedback = cfg::kHitFeedbackTime * 0.6f;
							hitStrength = 0.6f;
							if (site.Built()) messages.Show(std::string(site.Def().name) + " built.", 1.2f);
							claim.dirty = true;
							saveRequested = site.Built();
						}
					}
				}
			}
			else claim.swingTimer = 0.0f;

			// Taking down: X held on a piece.
			if (claim.aimed >= 0 && input.IsKeyDown('X'))
			{
				claim.takeDown += dt;
				if (claim.takeDown >= cfg::kTakeDownTime)
				{
					claim.takeDown = 0.0f;
					const std::string why = claimPlan.CanRemove(claim.aimed);
					if (!why.empty()) messages.Show("Can't take it down: " + why + ".", 1.8f);
					else
					{
						const build::Piece gone = claimPlan.Remove(claim.aimed);
						// Everything comes back: stone as loose blocks where it stood.
						const float cx = claim.origin.x + (gone.x + 0.5f) * kCell, cz = claim.origin.z + (gone.z + 0.5f) * kCell;
						const float cy = claim.origin.y + gone.y * kCell + 0.6f;
						for (const auto& [id, n] : build::PaidFor(gone.Def(), gone.swings))
						{
							const Block block = econ::BlockNamed(id);
							if (block == Block::Air) { purse.Add(id, n); continue; }
							for (int i = 0; i < n; ++i)
							{
								const float ox = ((i % 4) - 1.5f) * 0.26f, oz = (((i / 4) % 4) - 1.5f) * 0.26f, oy = (i / 16) * 0.26f;
								blocks.SpawnFree(engine, block, cfg::kVoxX / 2, 4, 2, { cx + ox, cy + oy, cz + oz }, { 0, 0.5f, 0 });
							}
						}
						messages.Show(std::string(gone.Def().name) + " taken down.", 1.2f);
						claim.dirty = true;
						claim.aimed = -1;
						saveRequested = true;
					}
				}
			}
			else claim.takeDown = 0.0f;
		}
	}

	// The ghost: the selected piece where it would go, half there.
	if (showGhost)
	{
		const int key = claim.candidate.type * 8 + claim.candidate.turn;
		if (key != claim.ghostKey)
		{
			claim.ghostKey = key;
			if (claim.ghostMesh.IsValid()) engine.GetRenderer().DestroyMesh(claim.ghostMesh);
			claim.ghostMesh = kit::Bake(engine, build::Plan::PieceModel(claim.candidate), VoxelPivot::Corner);
		}
		if (!scene.IsAlive(claim.ghost)) claim.ghost = kit::Spawn(engine, claim.ghostMesh, engine.GetAssets().GetMaterial("Materials/Town.mat"), {});
		MeshRenderer& mr = scene.Get<MeshRenderer>(claim.ghost);
		mr.mesh = claim.ghostMesh;
		mr.visible = true;
		mr.dither = 0.45f;
		mr.castShadows = false;
		const std::array<int, 3> o = build::Plan::PieceOrigin(claim.candidate);
		scene.Get<Transform>(claim.ghost).position = { claim.origin.x + o[0] * kVox, claim.origin.y + o[1] * kVox, claim.origin.z + o[2] * kVox };
	}
	else if (scene.IsAlive(claim.ghost)) scene.Get<MeshRenderer>(claim.ghost).visible = false;

	RebuildClaim(engine);
}

void UndertimeModule::Sleep(Engine& engine)
{
	const float hour = static_cast<float>(std::fmod(clock.Days(), 1.0) * 24.0);
	if (hour < 20.0f && hour >= 5.0f)
	{
		messages.Show("You aren't tired yet (beds are for nights, from 20:00).", 2.0f);
		return;
	}
	const double until = std::floor(clock.Days() + (hour >= 20.0f ? 1.0 : 0.0)) + cfg::kSleepUntilHour / 24.0;
	const double slept = until - clock.Days();
	clock.SetDays(until);
	daysAtEntry = clock.Days();
	messages.Show("You slept " + std::to_string(static_cast<int>(std::lround(slept * 24.0))) + " hours. " + clock.DateText() + ".", 3.0f);
	OnSlept(engine, slept);
	saveRequested = true;
}

bool UndertimeModule::ClaimInteract(Engine& engine)
{
	// The tent.
	if (engine.GetScene().IsAlive(claim.tent))
	{
		const XMFLOAT3 t = engine.GetScene().Get<Transform>(claim.tent).position;
		if (AimedAt({ t.x, t.y + 0.8f, t.z }, 3.5f)) { Sleep(engine); return true; }
	}
	const int i = claim.aimed >= 0 ? claim.aimed : -1;
	if (i < 0) return false;
	build::Piece& p = claimPlan.Pieces()[i];
	if (!p.Built()) return false;
	switch (p.Def().use)
	{
	case build::Use::Door:
		p.open = !p.open;
		claim.dirty = true;
		return true;
	case build::Use::Workbench: OpenMenu(engine, Menu::Bench); return true;
	case build::Use::Crate: OpenMenu(engine, Menu::Store); return true;
	case build::Use::FirePit: OpenMenu(engine, Menu::Cook); return true;
	case build::Use::Bed: Sleep(engine); return true;
	default: return false;
	}
}

std::string UndertimeModule::ClaimPrompt() const
{
	if (tool != Tool::Hammer && claim.aimed < 0) return {};
	if (tool == Tool::Hammer && !placeItem.empty()) return {};
	if (claim.aimed >= 0)
	{
		const build::Piece& p = claimPlan.Pieces()[claim.aimed];
		if (!p.Built()) return std::string("Hold left mouse  Build the ") + p.Def().name + " (" + std::to_string(p.swings) + " / " + std::to_string(p.Def().swings) + ")";
		std::string use;
		switch (p.Def().use)
		{
		case build::Use::Door: use = p.open ? "E  Close" : "E  Open"; break;
		case build::Use::Workbench: use = "E  Use the workbench"; break;
		case build::Use::Crate: use = "E  Open the crate"; break;
		case build::Use::FirePit: use = "E  Cook"; break;
		case build::Use::Bed: use = "E  Sleep"; break;
		default: break;
		}
		if (tool == Tool::Hammer) use += (use.empty() ? "" : "      ") + std::string("Hold X  Take down");
		if (!use.empty()) return use;
	}
	if (tool != Tool::Hammer) return {};
	const build::PieceDef& def = build::Pieces()[claim.selected];
	std::string cost;
	for (const auto& [id, n] : def.cost) cost += (cost.empty() ? "" : ", ") + std::to_string(n) + " " + econ::NameOf(id);
	if (cost.empty()) cost = "free";
	return std::string(def.name) + " (" + cost + ")   " + (claim.candidateValid ? "Left mouse  Build" : (claim.why.empty() ? "Aim at your claim" : claim.why)) +
		"      R  Turn      Right mouse  Pieces";
}

void UndertimeModule::ClaimHud(std::vector<std::string>& lines) const
{
	if (!OnClaim(player.position, 2.0f) || claim.store.empty()) return;
	int n = 0;
	for (const auto& [id, count] : claim.store) n += count;
	lines.push_back("Crates  " + std::to_string(n));
}

// =============================================================================
// Menus: the hammer's pieces, a crate, the bench, a fire
// =============================================================================

void UndertimeModule::OpenMenu(Engine& engine, Menu which)
{
	menu = which;
	state = State::Menu;
	engine.GetInput().SetMouseCaptured(false);
}

void UndertimeModule::CloseMenu(Engine& engine)
{
	menu = Menu::None;
	state = State::Playing;
	engine.GetInput().SetMouseCaptured(true);
	waitForRelease = true;
	saveRequested = true;
}

void UndertimeModule::DrawMenu(Engine& engine)
{
	const ViewRect view = engine.GameViewRect();
	const float scale = ImGui::GetFontSize() / 13.0f;
	ImGui::SetNextWindowPos(ImVec2(view.x + view.width * 0.5f, view.y + view.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(view.width * 0.7f, 700.0f * scale), std::min(view.height * 0.8f, 600.0f * scale)), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);
	ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	const ImVec4 gold(1.0f, 0.77f, 0.47f, 1.0f);
	const auto have = [this](const std::string& id) { return BuildHave(id); };
	ImGui::BeginChild("##menubody", ImVec2(0, -36.0f * scale));
	switch (menu)
	{
	case Menu::Pieces:
	{
		ImGui::TextColored(gold, "Build");
		ImGui::SameLine();
		ImGui::TextDisabled("pick a piece; costs come from your pack, a barrow close by, and your crates");
		ImGui::Separator();
		// Your things first: props, ladders, chutes, torches, machines (placed anywhere).
		bool things = false;
		for (const econ::Item& item : econ::Items())
		{
			if (item.kind != econ::Kind::Placeable || purse.Count(item.id) <= 0) continue;
			if (!things) { ImGui::TextColored(gold, "Your things (anywhere)"); things = true; }
			ImGui::PushID(item.id);
			if (ImGui::Selectable(item.name, placeItem == item.id, 0, ImVec2(220.0f * scale, 0)))
			{
				placeItem = item.id;
				CloseMenu(engine);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("x%d   %s", purse.Count(item.id), item.about);
			ImGui::PopID();
		}
		if (!things) ImGui::TextDisabled("Your things: make props, ladders, chutes and torches at your bench; buy machines in town.");
		std::string group;
		const auto& pieces = build::Pieces();
		for (size_t i = 0; i < pieces.size(); ++i)
		{
			const build::PieceDef& d = pieces[i];
			if (group != d.group)
			{
				group = d.group;
				ImGui::Spacing();
				ImGui::TextColored(gold, "%s", d.group);
			}
			const bool later = d.eraFrom > era;
			std::string cost;
			bool enough = true;
			for (const auto& [id, n] : d.cost)
			{
				cost += (cost.empty() ? "" : ", ") + std::to_string(n) + " " + econ::NameOf(id);
				enough = enough && BuildHave(id) >= n;
			}
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginDisabled(later);
			if (ImGui::Selectable(d.name, placeItem.empty() && claim.selected == static_cast<int>(i), 0, ImVec2(220.0f * scale, 0)))
			{
				claim.selected = static_cast<int>(i);
				placeItem.clear();
				claim.ghostKey = -1;
				CloseMenu(engine);
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (later) ImGui::TextDisabled("from the %s", era::Name(d.eraFrom));
			else ImGui::TextColored(enough ? ImVec4(0.8f, 0.8f, 0.75f, 1.0f) : ImVec4(0.9f, 0.45f, 0.35f, 1.0f), "%s", cost.empty() ? "free" : cost.c_str());
			ImGui::PopID();
		}
		break;
	}
	case Menu::Store:
	{
		ImGui::TextColored(gold, "Your crates");
		ImGui::SameLine();
		ImGui::TextDisabled("shared by all your crates; building takes from them too");
		ImGui::Separator();
		if (ImGui::Button("Put in all the stone from your pack"))
		{
			const int n = backpack[static_cast<int>(Block::Stone)];
			backpack[static_cast<int>(Block::Stone)] = 0;
			if (n > 0) claim.store["Stone"] += n;
		}
		if (barrow.owned && !barrow.held)
		{
			ImGui::SameLine();
			if (ImGui::Button("Empty the barrow in"))
			{
				for (int b = 1; b < static_cast<int>(Block::Count); ++b)
					if (barrow.load[b] > 0) { claim.store[cfg::Info(static_cast<Block>(b)).name] += barrow.load[b]; barrow.load[b] = 0; }
			}
		}
		if (ImGui::BeginTable("##store", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableNextColumn();
			ImGui::TextColored(gold, "In your pack");
			int row = 0;
			auto putRow = [&](const std::string& id, int n)
			{
				if (n <= 0) return;
				ImGui::PushID(row++);
				if (ImGui::SmallButton("Put in")) { Take(id, n); claim.store[id] += n; }
				ImGui::SameLine();
				ImGui::Text("%s  %d", econ::NameOf(id).c_str(), n);
				ImGui::PopID();
			};
			for (int b = 1; b < static_cast<int>(Block::Count); ++b) putRow(cfg::Info(static_cast<Block>(b)).name, backpack[b]);
			const econ::Goods goods = purse.goods;
			for (const auto& [id, n] : goods)
			{
				const econ::Item* item = econ::FindItem(id);
				if (item && item->kind == econ::Kind::Tool) continue;
				putRow(id, n);
			}
			ImGui::TableNextColumn();
			ImGui::TextColored(gold, "In the crates");
			const econ::Goods stored = claim.store;
			for (const auto& [id, n] : stored)
			{
				ImGui::PushID(row++);
				if (ImGui::SmallButton("Take"))
				{
					const Block block = econ::BlockNamed(id);
					const int room = block == Block::Air ? n : std::min(n, PackCapacity() - BackpackCount());
					if (room > 0)
					{
						Give(engine, id, room);
						claim.store[id] -= room;
						if (claim.store[id] <= 0) claim.store.erase(id);
					}
					else messages.Show("Your pack is full.", 1.5f);
				}
				ImGui::SameLine();
				ImGui::Text("%s  %d", econ::NameOf(id).c_str(), n);
				ImGui::PopID();
			}
			if (stored.empty()) ImGui::TextDisabled("Empty.");
			ImGui::EndTable();
		}
		break;
	}
	case Menu::Bench:
	{
		ImGui::TextColored(gold, "Workbench");
		ImGui::SameLine();
		ImGui::TextDisabled("basic things; tools and machines come from the smith and the store");
		ImGui::Separator();
		const auto& recipes = econ::Recipes();
		for (size_t i = 0; i < recipes.size(); ++i)
		{
			const econ::Recipe& r = recipes[i];
			const econ::Item* item = econ::FindItem(r.item);
			if (!item) continue;
			std::string needs;
			for (const auto& [id, n] : r.needs) needs += (needs.empty() ? "" : " + ") + std::to_string(n) + " " + econ::NameOf(id);
			const econ::Verdict v = econ::CanMake(r, have);
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginDisabled(!v.ok);
			if (ImGui::Button("Make"))
			{
				for (const auto& [id, n] : r.needs) BuildTake(id, n);
				purse.Add(r.item, r.makes);
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::Text("%s%s", r.makes > 1 ? (std::to_string(r.makes) + " x ").c_str() : "", item->name);
			ImGui::SameLine();
			ImGui::TextDisabled("from %s%s", needs.c_str(), purse.Count(r.item) > 0 ? ("   (have " + std::to_string(purse.Count(r.item)) + ")").c_str() : "");
			ImGui::TextDisabled("    %s", item->about);
			ImGui::PopID();
		}
		break;
	}
	case Menu::Cook:
		DrawCooking(engine);
		break;
	default:
		break;
	}
	ImGui::EndChild();
	ImGui::Separator();
	if (ImGui::Button("Close  (Esc)") || engine.GetInput().WasKeyPressed(VK_ESCAPE)) CloseMenu(engine);
	ImGui::End();
}

// =============================================================================
// The save game
// =============================================================================

nlohmann::json UndertimeModule::SaveClaim() const
{
	nlohmann::json j;
	nlohmann::json pieces = nlohmann::json::array();
	for (const build::Piece& p : claimPlan.Pieces())
		pieces.push_back({ p.Def().id, p.x, p.y, p.z, p.turn, p.swings, p.open ? 1 : 0 });
	j["pieces"] = pieces;
	j["store"] = claim.store;
	return j;
}

void UndertimeModule::LoadClaim(Engine& engine, const nlohmann::json& j)
{
	if (!j.is_object() || !j.contains("pieces")) return;
	claimPlan = build::Plan(cfg::kClaimCells, cfg::kClaimCells, cfg::kClaimLevels);
	for (const auto& e : j["pieces"])
	{
		if (!e.is_array() || e.size() < 6) continue;
		build::Piece p;
		p.type = build::PieceIndex(e[0].get<std::string>());
		if (p.type < 0) continue;
		p.x = e[1]; p.y = e[2]; p.z = e[3]; p.turn = e[4]; p.swings = e[5];
		p.open = e.size() > 6 && e[6].get<int>() != 0;
		claimPlan.Place(p);
	}
	claim.store = j.contains("store") ? j["store"].get<econ::Goods>() : econ::Goods{};
	claim.dirty = true;
	RebuildClaim(engine);
}
