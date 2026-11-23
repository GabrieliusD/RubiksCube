#include "NpcWorld.h"
#include "NpcLog.h"
#include "Config.h"
#include <Core/Assets.h>
#include <Core/Log.h>
#include <Voxel/VoxelModel.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
	float WrapAngle(float a)
	{
		while (a > XM_PI) a -= XM_2PI;
		while (a < -XM_PI) a += XM_2PI;
		return a;
	}
	float Fraction(float x) { return x - std::floor(x); }
}

// =============================================================================
// Setup
// =============================================================================

NpcWorld::Parts NpcWorld::LoadParts(Engine& engine, const npc::NpcDef& npc)
{
	Parts parts;
	Renderer& renderer = engine.GetRenderer();
	VoxelBakeOptions options;
	options.voxelSize = cfg::kNpcVoxelSize;
	const float s = cfg::kNpcVoxelSize;
	MeshHandle* meshes[4] = { &parts.head, &parts.torso, &parts.arm, &parts.leg };
	for (int i = 0; i < 4; ++i)
	{
		VoxelModel model;
		if (!engine.GetAssets().ReadVoxelModel(npc::PartModel(npc, npc::kParts[i]), model) || model.SolidCount() == 0)
		{
			CHERRY_LOG("UNDERTIME: %s has no %s (run UndertimePlaceholders to write the placeholder people)", npc.name, npc::PartModel(npc, npc::kParts[i]).c_str());
			return parts;
		}
		MeshData data = BakeVoxelMesh(model, options);
		const float h = model.size.y * s;
		// Arms and legs hang from their joint: the model's top centre is the shoulder or hip.
		if (i >= 2)
			for (Vertex& v : data.vertices) v.position.y -= h;
		*meshes[i] = renderer.CreateMesh(data);
		switch (i)
		{
		case 0: parts.headH = h; break;
		case 1: parts.torsoH = h; parts.torsoW = model.size.x * s; parts.torsoD = model.size.z * s; break;
		case 2: parts.armW = model.size.x * s; break;
		case 3: parts.legH = h; parts.legW = model.size.x * s; break;
		}
	}
	parts.valid = true;
	return parts;
}

NpcWorld::Puppet NpcWorld::MakePuppet(Scene& scene, const Parts& parts, MaterialHandle material)
{
	Puppet p;
	auto make = [&](Entity parent, MeshHandle mesh)
	{
		const Entity e = scene.CreateEntity();   // no EntityInfo: never saved
		scene.Add(e, Transform{});
		if (mesh.IsValid()) scene.Add(e, MeshRenderer{ mesh, material });
		scene.SetParent(e, parent, false);
		return e;
	};
	p.root = make(mGroup, MeshHandle{});
	p.torso = make(p.root, parts.torso);
	p.head = make(p.torso, parts.head);
	for (int side = 0; side < 2; ++side)
	{
		p.arm[side] = make(p.torso, parts.arm);
		p.leg[side] = make(p.root, parts.leg);
	}
	return p;
}

void NpcWorld::Create(Engine& engine, Scene& scene, const Valley& valley)
{
	// Places on props: the props' world matrices by name (they're already on the ground).
	mTown.Build(valley, [&](const char* entity, XMFLOAT4X4& world)
	{
		bool found = false;
		scene.Each<EntityInfo>([&](Entity e, EntityInfo& info)
		{
			if (found || info.name != entity || !scene.Has<Transform>(e)) return;
			XMStoreFloat4x4(&world, scene.WorldMatrix(e));
			found = true;
		});
		return found;
	});
	int missing = 0;
	for (size_t i = 0; i < mTown.PlaceCount(); ++i)
		if (!mTown.PlaceAt(i).resolved)
		{
			CHERRY_LOG("UNDERTIME: place %s not found (its prop isn't in the scene, or no dry river bank)", npc::Places()[i].id);
			++missing;
		}

	mGroup = scene.CreateEntity();
	scene.Add(mGroup, Transform{});
	const MaterialHandle material = engine.GetAssets().GetMaterial("Materials/Town.mat");
	mMaterial = material;
	mTalking = -1;
	mPeople.clear();
	const std::vector<npc::NpcDef>& cast = npc::Cast();
	for (size_t i = 0; i < cast.size(); ++i)
	{
		Person person;
		person.parts = LoadParts(engine, cast[i]);
		person.seed = static_cast<float>(i) * 2.17f + 0.5f;
		if (person.parts.valid)
			for (int c = 0; c <= cfg::kNpcEchoes; ++c) person.copies.push_back(MakePuppet(scene, person.parts, material));
		mPeople.push_back(std::move(person));
	}
	CHERRY_LOG("UNDERTIME: %zu people, %zu places (%d missing), trails: %zu points, %zu links, %d piece(s)", cast.size(), mTown.PlaceCount(), missing,
		mTown.Graph().Nodes().size(), mTown.Graph().Edges().size(), mTown.Graph().Components());
}

void NpcWorld::Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics)
{
	Renderer& renderer = engine.GetRenderer();
	for (Person& person : mPeople)
	{
		for (const Puppet& p : person.copies)
		{
			for (Entity e : { p.head, p.arm[0], p.arm[1], p.torso, p.leg[0], p.leg[1], p.root })
				if (scene.IsAlive(e)) scene.DestroyEntity(e);
		}
		for (MeshHandle m : { person.parts.head, person.parts.torso, person.parts.arm, person.parts.leg })
			if (m.IsValid()) renderer.DestroyMesh(m);
		if (physics && person.blocker != kInvalidBody) physics->RemoveBody(person.blocker);
	}
	mPeople.clear();
	if (scene.IsAlive(mGroup)) scene.DestroyEntity(mGroup);
	mGroup = INVALID_ENTITY;
}

// =============================================================================
// Posing
// =============================================================================

void NpcWorld::SetVisible(Scene& scene, const Puppet& puppet, bool visible)
{
	for (Entity e : { puppet.torso, puppet.head, puppet.arm[0], puppet.arm[1], puppet.leg[0], puppet.leg[1] })
		if (scene.Has<MeshRenderer>(e)) scene.Get<MeshRenderer>(e).visible = visible;
}

void NpcWorld::PosePuppet(Scene& scene, const Puppet& puppet, const Person& person, const npc::Pose& pose, float y, float headYaw, float dither, bool shadows) const
{
	using npc::Activity;
	const Parts& parts = person.parts;
	const float t = mTime + person.seed;
	const float s = cfg::kNpcVoxelSize;

	float hip = parts.legH, lean = 0.0f, bob = 0.0f;
	float arm[2] = { 0.0f, 0.0f }, leg[2] = { 0.0f, 0.0f };
	const float idle = 0.05f * std::sin(t * 1.1f);
	switch (pose.activity)
	{
	case Activity::Walk:
	{
		const float phase = pose.walked / cfg::kNpcStride * XM_2PI;
		leg[0] = 0.55f * std::sin(phase);
		leg[1] = -leg[0];
		arm[0] = -0.45f * std::sin(phase);
		arm[1] = -arm[0];
		bob = 0.025f * std::fabs(std::cos(phase));
		break;
	}
	case Activity::Work:
		arm[0] = -0.75f + 0.08f * std::sin(t * 2.3f);
		arm[1] = -0.75f + 0.08f * std::sin(t * 2.3f + 1.7f);
		lean = 0.12f;
		break;
	case Activity::Hammer:
	{
		// Raise slowly, strike fast, rest a moment.
		const float u = Fraction(t / 1.1f);
		const float up = u < 0.65f ? u / 0.65f : (u < 0.75f ? 1.0f - (u - 0.65f) / 0.1f : 0.0f);
		arm[0] = -0.7f;
		arm[1] = -0.6f - 1.9f * up;
		lean = 0.15f;
		break;
	}
	case Activity::Chop:
	{
		const float u = Fraction(t / 1.6f);
		const float up = u < 0.6f ? u / 0.6f : (u < 0.7f ? 1.0f - (u - 0.6f) / 0.1f : 0.0f);
		arm[0] = arm[1] = -0.6f - 2.4f * up;
		lean = 0.1f + 0.15f * (1.0f - up);
		break;
	}
	case Activity::Cook:
		arm[0] = -0.85f;
		arm[1] = -0.9f + 0.22f * std::sin(t * 2.5f);
		lean = 0.18f;
		break;
	case Activity::Sit:
	case Activity::Pan:
		hip = parts.legW * 0.5f + 0.01f;
		leg[0] = -1.45f;
		leg[1] = -1.5f;
		arm[0] = arm[1] = -0.55f + idle;
		if (pose.activity == Activity::Pan)
		{
			arm[0] = -1.15f + 0.12f * std::sin(t * 3.0f);
			arm[1] = -1.15f + 0.12f * std::sin(t * 3.0f + 0.6f);
			lean = 0.3f;
		}
		break;
	case Activity::Sleep:
		break;
	case Activity::Stand:
	default:
		arm[0] = idle;
		arm[1] = -idle;
		break;
	}

	// The root: feet on the ground facing the way they look, or lying with the head where the
	// place faces (pitched back onto the back, so the face looks up).
	Transform& root = scene.Get<Transform>(puppet.root);
	const float scale = person.scale;
	if (pose.age > cfg::kOldAge && pose.activity != Activity::Sleep) lean += 0.2f;   // the old stoop
	root.scale = { scale, scale, scale };
	if (pose.activity == Activity::Sleep)
	{
		const float height = (parts.legH + parts.torsoH + parts.headH) * scale;
		root.SetRotationEuler(-XM_PIDIV2, pose.yaw + XM_PI, 0.0f);
		root.position = { pose.position.x - std::sin(pose.yaw) * height * 0.5f, y + parts.torsoD * 0.5f * scale,
			pose.position.z - std::cos(pose.yaw) * height * 0.5f };
	}
	else
	{
		root.SetRotationEuler(0.0f, pose.yaw, 0.0f);
		root.position = { pose.position.x, y, pose.position.z };
	}

	Transform& torso = scene.Get<Transform>(puppet.torso);
	torso.position = { 0.0f, hip + bob, 0.0f };
	torso.SetRotationEuler(lean, 0.0f, 0.0f);
	Transform& head = scene.Get<Transform>(puppet.head);
	head.position = { 0.0f, parts.torsoH, 0.0f };
	head.SetRotationEuler(-lean * 0.5f, headYaw, 0.0f);
	for (int side = 0; side < 2; ++side)
	{
		const float x = (side == 0 ? -1.0f : 1.0f);
		Transform& a = scene.Get<Transform>(puppet.arm[side]);
		a.position = { x * (parts.torsoW + parts.armW) * 0.5f, parts.torsoH - s, 0.0f };
		a.SetRotationEuler(arm[side], 0.0f, x * 0.04f);
		Transform& l = scene.Get<Transform>(puppet.leg[side]);
		l.position = { x * (parts.legW * 0.5f + s * 0.5f), hip, 0.0f };
		l.SetRotationEuler(leg[side], 0.0f, 0.0f);
	}

	for (Entity e : { puppet.torso, puppet.head, puppet.arm[0], puppet.arm[1], puppet.leg[0], puppet.leg[1] })
	{
		MeshRenderer& r = scene.Get<MeshRenderer>(e);
		r.visible = dither > 0.02f;
		r.dither = dither;
		r.castShadows = shadows;
	}
}

void NpcWorld::UpdateBlocker(PhysicsWorld& physics, Person& person, const View& view, bool wanted)
{
	const npc::Pose& pose = person.pose;
	const float scale = person.scale;
	const bool sitting = pose.activity == npc::Activity::Sit || pose.activity == npc::Activity::Pan;
	const float halfY = (sitting ? 0.4f : 0.85f) * scale;
	const XMFLOAT3 at{ pose.position.x + (sitting ? std::sin(pose.yaw) * 0.3f : 0.0f), person.shownY + halfY, pose.position.z + (sitting ? std::cos(pose.yaw) * 0.3f : 0.0f) };
	const float toPlayer = std::hypot(view.feet.x - at.x, view.feet.z - at.z);
	// Never put a box where the player stands (they'd be stuck inside it).
	if (wanted && person.blocker == kInvalidBody && toPlayer < 0.7f) wanted = false;
	const bool moved = person.blocker != kInvalidBody && std::hypot(at.x - person.blockerAt.x, at.z - person.blockerAt.z) + std::fabs(at.y - person.blockerAt.y) > 0.05f;
	if (person.blocker != kInvalidBody && (!wanted || moved))
	{
		physics.RemoveBody(person.blocker);
		person.blocker = kInvalidBody;
	}
	if (wanted && person.blocker == kInvalidBody && toPlayer >= 0.7f)
	{
		person.blocker = physics.CreateStaticBox(at, { 0.24f * scale, halfY, 0.24f * scale });
		person.blockerAt = at;
	}
}

void NpcWorld::Update(Engine& engine, Scene& scene, PhysicsWorld& physics, const View& view, float dt)
{
	mTime += dt;
	if (!mTown.Built()) return;

	// Time racing: figures go faint (with echoes trailing them), then vanish.
	const float speedUp = view.timeRate / cfg::kSurfaceRate;
	const float lapse = std::clamp((speedUp - cfg::kNpcLapseStart) / (cfg::kNpcLapseFull - cfg::kNpcLapseStart), 0.0f, 1.0f);
	const float gone = std::clamp((speedUp - cfg::kNpcLapseVanishFrom) / (cfg::kNpcLapseVanish - cfg::kNpcLapseVanishFrom), 0.0f, 1.0f);
	const float mainDither = (1.0f - lapse * (1.0f - cfg::kNpcLapseFaint)) * (1.0f - gone);
	const bool shadows = lapse < 0.05f;
	mMainDither = mainDither;

	for (size_t i = 0; i < mPeople.size(); ++i)
	{
		Person& person = mPeople[i];
		if (person.copies.empty()) continue;
		person.pose = mTown.Evaluate(i, view.days, view.era, view.log);
		// Children are smaller; grown by kGrownAge.
		const float grown = std::clamp(static_cast<float>(person.pose.age) / cfg::kGrownAge, 0.0f, 1.0f);
		person.scale = npc::Cast()[i].look.height * (cfg::kChildScale + (1.0f - cfg::kChildScale) * grown);
		// Talking to you: they stop what they're doing and face you (sitting, they only turn their heads).
		const bool talking = static_cast<int>(i) == mTalking && person.pose.present;
		if (talking)
		{
			npc::Pose& p = person.pose;
			const bool seated = p.activity == npc::Activity::Sit || p.activity == npc::Activity::Pan || p.activity == npc::Activity::Sleep;
			if (!seated)
			{
				p.yaw = std::atan2(view.eye.x - p.position.x, view.eye.z - p.position.z);
				p.activity = npc::Activity::Stand;
			}
		}
		const npc::Pose& pose = person.pose;
		person.distance = std::hypot(pose.position.x - view.eye.x, pose.position.y - view.eye.y, pose.position.z - view.eye.z);
		const bool shown = pose.present && !view.hidden && person.distance < cfg::kNpcDrawDistance && mainDither > 0.02f;
		if (!shown)
		{
			for (const Puppet& p : person.copies) SetVisible(scene, p, false);
			UpdateBlocker(physics, person, view, false);
			person.placed = false;
			person.shown = false;
			continue;
		}

		// The feet ease over the terrain's 0.25 m steps (a jump means a new place: snap).
		if (!person.placed || std::fabs(pose.position.y - person.shownY) > 1.5f) person.shownY = pose.position.y;
		else person.shownY += (pose.position.y - person.shownY) * std::min(1.0f, dt * 12.0f);
		person.placed = true;
		person.shown = true;

		// Close by, they turn their heads to you.
		float lookTarget = 0.0f;
		const bool nearby = person.distance < cfg::kNpcNearDistance;
		const float toPlayer = std::hypot(view.eye.x - pose.position.x, view.eye.z - pose.position.z);
		if ((nearby && toPlayer < cfg::kNpcLookDistance && pose.activity != npc::Activity::Sleep && lapse < 0.05f) || talking)
		{
			const float bearing = std::atan2(view.eye.x - pose.position.x, view.eye.z - pose.position.z);
			lookTarget = std::clamp(WrapAngle(bearing - pose.yaw), -1.05f, 1.05f);
		}
		person.headYaw += (lookTarget - person.headYaw) * std::min(1.0f, dt * 4.0f);

		PosePuppet(scene, person.copies[0], person, pose, person.shownY, person.headYaw, mainDither, shadows);

		// Echoes: where they were a moment ago in outside time, fainter each.
		for (int k = 1; k <= cfg::kNpcEchoes && k < static_cast<int>(person.copies.size()); ++k)
		{
			const Puppet& echo = person.copies[k];
			if (lapse < 0.02f)
			{
				SetVisible(scene, echo, false);
				continue;
			}
			const double earlier = view.days - k * cfg::kNpcEchoSeconds * view.timeRate / 86400.0;
			const npc::Pose past = mTown.Evaluate(i, earlier, view.era, view.log);
			const float apart = std::hypot(past.position.x - pose.position.x, past.position.z - pose.position.z);
			if (!past.present || apart < 0.08f)   // standing still: an echo would only sit on top
			{
				SetVisible(scene, echo, false);
				continue;
			}
			const float dither = mainDither * lapse * (1.0f - static_cast<float>(k) / (cfg::kNpcEchoes + 1));
			PosePuppet(scene, echo, person, past, past.position.y, 0.0f, dither, false);
		}

		const bool still = pose.activity != npc::Activity::Walk && pose.activity != npc::Activity::Sleep;
		UpdateBlocker(physics, person, view, nearby && still && lapse < 0.05f && toPlayer < cfg::kNpcBlockerDistance);
	}

	if (mShowRoutes)
	{
		DebugDraw& debug = engine.GetRenderer().Debug();
		const auto& nodes = mTown.Graph().Nodes();
		const uint32_t trail = DebugDraw::Color(0.6f, 0.6f, 0.6f), walk = DebugDraw::Color(1.0f, 0.85f, 0.2f), place = DebugDraw::Color(0.3f, 0.9f, 1.0f);
		for (const auto& [a, b] : mTown.Graph().Edges())
		{
			const XMFLOAT3 pa{ nodes[a].x, mTown.GroundY(nodes[a].x, nodes[a].y) + 0.3f, nodes[a].y };
			const XMFLOAT3 pb{ nodes[b].x, mTown.GroundY(nodes[b].x, nodes[b].y) + 0.3f, nodes[b].y };
			debug.Line(pa, pb, trail);
		}
		for (size_t i = 0; i < mTown.PlaceCount(); ++i)
		{
			const npc::Place& p = mTown.PlaceAt(i);
			if (!p.resolved) continue;
			const float y = p.at.ground ? mTown.GroundY(p.at.x, p.at.z) : p.at.y;
			debug.Line({ p.at.x, y, p.at.z }, { p.at.x, y + 2.2f, p.at.z }, place);
			debug.Line({ p.at.x, y + 0.1f, p.at.z }, { p.at.x + std::sin(p.yaw) * 0.6f, y + 0.1f, p.at.z + std::cos(p.yaw) * 0.6f }, place);
		}
		for (const Person& person : mPeople)
		{
			if (!person.pose.present || person.pose.from < 0) continue;
			const npc::Town::Route& route = mTown.RouteBetween(person.pose.from, person.pose.place);
			for (size_t k = 1; k < route.points.size(); ++k)
			{
				const npc::RoutePoint& p = route.points[k - 1];
				const npc::RoutePoint& q = route.points[k];
				const float py = (p.ground ? mTown.GroundY(p.x, p.z) : p.y) + 0.4f, qy = (q.ground ? mTown.GroundY(q.x, q.z) : q.y) + 0.4f;
				debug.Line({ p.x, py, p.z }, { q.x, qy, q.z }, walk);
			}
		}
	}
}

// =============================================================================
// Talking
// =============================================================================

bool NpcWorld::Shown(int npc) const
{
	return npc >= 0 && npc < static_cast<int>(mPeople.size()) && mPeople[npc].shown && mMainDither > 0.98f;
}

XMFLOAT3 NpcWorld::HeadPosition(int npc) const
{
	const Person& person = mPeople[npc];
	const float scale = person.scale;
	const npc::Activity a = person.pose.activity;
	const float body = person.parts.legH + person.parts.torsoH + person.parts.headH * 0.5f;
	XMFLOAT3 p{ person.pose.position.x, person.shownY, person.pose.position.z };
	if (a == npc::Activity::Sleep)
	{
		// Lying: the place is the body's middle, the head towards where they face.
		p.y += person.parts.torsoD * scale;
		p.x += std::sin(person.pose.yaw) * body * scale * 0.5f;
		p.z += std::cos(person.pose.yaw) * body * scale * 0.5f;
	}
	else if (a == npc::Activity::Sit || a == npc::Activity::Pan) p.y += (body - person.parts.legH + person.parts.legW) * scale;
	else p.y += body * scale;
	return p;
}

int NpcWorld::TalkTarget(const XMFLOAT3& eye, const XMFLOAT3& look) const
{
	int best = -1;
	float bestDistance = cfg::kTalkDistance;
	for (size_t i = 0; i < mPeople.size(); ++i)
	{
		if (!Shown(static_cast<int>(i))) continue;
		const XMFLOAT3 head = HeadPosition(static_cast<int>(i));
		const XMVECTOR to = XMVectorSubtract(XMLoadFloat3(&head), XMLoadFloat3(&eye));
		const float distance = XMVectorGetX(XMVector3Length(to));
		if (distance > bestDistance || distance < 1e-3f) continue;
		// Aiming at the body counts too: the cone widens close up.
		const float aim = XMVectorGetX(XMVector3Dot(XMVectorScale(to, 1.0f / distance), XMLoadFloat3(&look)));
		if (aim < cfg::kTalkAimCos - 0.25f / std::max(distance, 0.5f)) continue;
		best = static_cast<int>(i);
		bestDistance = distance;
	}
	return best;
}

// =============================================================================
// Debug
// =============================================================================

bool NpcWorld::DebugGui(Engine&, const View& view, npc::PlayerLog& log, XMFLOAT3& goTo, float& goYaw)
{
	bool go = false;
	if (!ImGui::TreeNode("People")) return false;
	ImGui::Checkbox("Show trails, places and routes", &mShowRoutes);
	const float speedUp = view.timeRate / cfg::kSurfaceRate;
	ImGui::Text("Outside time x%.1f the surface rate%s", speedUp, speedUp >= cfg::kNpcLapseVanish ? " (people too fast to see)" : speedUp > cfg::kNpcLapseStart ? " (time-lapse)" : "");
	const std::vector<npc::NpcDef>& cast = npc::Cast();
	const std::vector<npc::PlaceDef>& places = npc::Places();
	if (ImGui::BeginTable("people", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		for (size_t i = 0; i < mPeople.size(); ++i)
		{
			const Person& person = mPeople[i];
			const npc::Pose& pose = person.pose;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s", cast[i].name);
			if (pose.present && !cast[i].ageless) { ImGui::SameLine(); ImGui::TextDisabled("%.0f", pose.age); }
			ImGui::TableNextColumn();
			if (person.copies.empty()) ImGui::TextDisabled("no models");
			else if (!pose.present)
			{
				const npc::LifeState life = npc::LifeAt(static_cast<int>(i), view.days, &log);
				ImGui::TextDisabled("%s", !life.born ? "not born yet" : !life.alive ? "died" : view.days < cast[i].arrives * 365.0 ? "not here yet"
					: view.days >= life.leavesDay ? "left the valley" : "no town for them in this era");
			}
			else if (pose.activity == npc::Activity::Walk)
				ImGui::Text("walking %s -> %s", places[pose.from].id, places[pose.place].id);
			else
				ImGui::Text("%s at %s%s", npc::ActivityName(pose.activity), places[pose.place].id, pose.rain ? " (rain)" : "");
			ImGui::TableNextColumn();
			if (pose.present) ImGui::Text("%4.0f m", person.distance);
			// What they think of you (debug: nudge it, as a log entry).
			const npc::Standing standing = log.StandingOf(static_cast<int>(i), view.days);
			ImGui::TableNextColumn();
			ImGui::Text("%s %+.0f%s", npc::TierName(standing.tier), standing.regard, standing.met ? "" : " (never met)");
			ImGui::TableNextColumn();
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::SmallButton("-10")) log.Add({ view.days, static_cast<int>(i), npc::EventKind::Regard, -10.0f, "debug" });
			ImGui::SameLine();
			if (ImGui::SmallButton("+10")) log.Add({ view.days, static_cast<int>(i), npc::EventKind::Regard, 10.0f, "debug" });
			ImGui::TableNextColumn();
			if (pose.present && ImGui::SmallButton("Go to"))
			{
				// A couple of metres in front of them, where they face.
				goTo = { pose.position.x + std::sin(pose.yaw) * 2.5f, pose.position.y + 0.5f, pose.position.z + std::cos(pose.yaw) * 2.5f };
				goYaw = pose.yaw + XM_PI;
				go = true;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::TextDisabled("T skips ahead, ] and [ speed outside time up and down, N changes the era.");
	ImGui::TreePop();
	return go;
}
