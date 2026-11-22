#include "ValleyProps.h"
#include "SceneComponents.h"
#include "Config.h"
#include <Core/Assets.h>
#include <Voxel/VoxelModel.h>
#include <algorithm>
#include <cmath>
#include <climits>

using namespace DirectX;

const char* era::Name(int era)
{
	static const char* const kNames[] = { "Camp", "Settlement", "Boomtown", "Industry", "Abandoned", "Gone" };
	return era >= kFirst && era <= kLast ? kNames[era - 1] : "?";
}

int ValleyProps::SnapToGround(Scene& scene, const Valley& valley)
{
	if (!scene.IsRegistered<ValleyProp>()) return 0;
	int moved = 0;
	scene.Each<ValleyProp>([&](Entity entity, ValleyProp& prop)
	{
		if (!prop.snapToGround || !scene.Has<Transform>(entity) || scene.Has<Parent>(entity)) return;
		Transform& t = scene.Get<Transform>(entity);
		const float ground = std::round(valley.Height(t.position.x, t.position.z) / cfg::kVoxelSize) * cfg::kVoxelSize + prop.groundOffset;
		if (std::fabs(t.position.y - ground) > 1e-3f)
		{
			t.position.y = ground;
			++moved;
		}
	});
	return moved;
}

const std::vector<PhysicsBox>* ValleyProps::BoxesFor(Engine& engine, const std::string& model)
{
	auto it = mBoxes.find(model);
	if (it == mBoxes.end())
	{
		VoxelModel voxels;
		std::vector<PhysicsBox> boxes;
		if (engine.GetAssets().ReadVoxelModel(model, voxels)) boxes = BakeVoxelBoxes(voxels);
		it = mBoxes.emplace(model, std::move(boxes)).first;
	}
	return &it->second;
}

const ValleyProps::ModelRoom& ValleyProps::RoomFor(Engine& engine, const std::string& model)
{
	auto it = mModelRooms.find(model);
	if (it != mModelRooms.end()) return it->second;
	ModelRoom room;
	VoxelModel voxels;
	if (engine.GetAssets().ReadVoxelModel(model, voxels) && HasGlass(voxels))
	{
		int lo[3] = { INT_MAX, INT_MAX, INT_MAX }, hi[3] = { -1, -1, -1 };
		for (uint32_t z = 0; z < voxels.size.z; ++z)
			for (uint32_t y = 0; y < voxels.size.y; ++y)
				for (uint32_t x = 0; x < voxels.size.x; ++x)
				{
					const uint8_t v = voxels.At(x, y, z);
					if (!v || voxels.materials[v].glass <= 0.0f) continue;
					const int c[3] = { static_cast<int>(x), static_cast<int>(y), static_cast<int>(z) };
					for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], c[a]); hi[a] = std::max(hi[a], c[a]); }
				}
		// x: between the side walls' windows; z: from the front windows to the rear wall
		// (side windows stop short of it); y: the floor to a little over the top windows.
		const XMFLOAT3 pivot = VoxelPivotOffset(voxels, VoxelBakeOptions{});
		const float s = cfg::kVoxelSize;
		const int zBack = std::min(hi[2] + 4, static_cast<int>(voxels.size.z) - 1);
		room.valid = hi[0] > lo[0] + 2;
		room.min = { lo[0] * s + pivot.x, pivot.y, lo[2] * s + pivot.z };
		room.max = { (hi[0] + 1) * s + pivot.x, (hi[1] + 4) * s + pivot.y, (zBack + 1) * s + pivot.z };
	}
	return mModelRooms.emplace(model, room).first->second;
}

void ValleyProps::UpdateRooms(Scene& scene)
{
	mRooms.clear();
	for (const auto& [entity, local] : mPropRooms)
	{
		if (!scene.Has<ValleyProp>(entity) || !scene.Has<MeshRenderer>(entity) || !scene.Get<MeshRenderer>(entity).visible) continue;
		const XMMATRIX world = scene.WorldMatrix(entity);
		const XMVECTOR c = XMVectorSet((local.min.x + local.max.x) * 0.5f, (local.min.y + local.max.y) * 0.5f, (local.min.z + local.max.z) * 0.5f, 1.0f);
		const XMVECTOR ax = XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), world), az = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), world);
		const XMVECTOR ay = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), world);
		RoomSettings room;
		XMStoreFloat3(&room.center, XMVector3TransformCoord(c, world));
		XMStoreFloat3(&room.axisX, XMVector3Normalize(ax));
		XMStoreFloat3(&room.axisZ, XMVector3Normalize(az));
		room.halfExtents = { (local.max.x - local.min.x) * 0.5f * XMVectorGetX(XMVector3Length(ax)),
			(local.max.y - local.min.y) * 0.5f * XMVectorGetX(XMVector3Length(ay)),
			(local.max.z - local.min.z) * 0.5f * XMVectorGetX(XMVector3Length(az)) };
		room.ambientScale = cfg::kRoomAmbient;
		room.fade = cfg::kRoomFade;
		mRooms.push_back(room);
	}
}

void ValleyProps::ApplyEra(Engine& engine, Scene& scene, int era, PhysicsWorld* physics)
{
	if (!scene.IsRegistered<ValleyProp>()) return;
	mEra = era;
	scene.Each<ValleyProp>([&](Entity entity, ValleyProp& prop)
	{
		const bool shown = era >= prop.eraFrom && era <= prop.eraTo;
		if (scene.Has<MeshRenderer>(entity)) scene.Get<MeshRenderer>(entity).visible = shown;
		if (scene.Has<ParticleEmitter>(entity)) scene.Get<ParticleEmitter>(entity).enabled = shown;   // smoke, sparks
		if (!physics) return;

		// Buildings with windows get a room (indoor light).
		if (scene.Has<MeshRenderer>(entity) && mPropRooms.find(entity) == mPropRooms.end())
		{
			const ModelRoom& room = RoomFor(engine, scene.Get<MeshRenderer>(entity).meshAsset);
			if (room.valid) mPropRooms[entity] = room;
		}

		if (scene.Has<PointLightComponent>(entity))
		{
			PointLightComponent& light = scene.Get<PointLightComponent>(entity);
			auto stored = mLightIntensity.find(entity);
			if (stored == mLightIntensity.end()) stored = mLightIntensity.emplace(entity, light.intensity).first;
			light.intensity = shown ? stored->second * mLightLevel : 0.0f;
		}

		// Collision: boxes baked from the model, turned and moved like the prop (each box's
		// bounds after the turn, so quarter turns are exact).
		auto bodies = mBodies.find(entity);
		const bool wantBodies = shown && prop.collide && scene.Has<MeshRenderer>(entity);
		if (!wantBodies && bodies != mBodies.end())
		{
			for (BodyId body : bodies->second) physics->RemoveBody(body);
			mBodies.erase(bodies);
		}
		else if (wantBodies && bodies == mBodies.end())
		{
			const std::string& model = scene.Get<MeshRenderer>(entity).meshAsset;
			const std::vector<PhysicsBox>* boxes = BoxesFor(engine, model);
			std::vector<BodyId>& created = mBodies[entity];
			const XMMATRIX world = scene.WorldMatrix(entity);
			for (const PhysicsBox& box : *boxes)
			{
				BoundingBox local(box.center, box.halfExtents), placed;
				local.Transform(placed, world);
				created.push_back(physics->CreateStaticBox(placed.Center, placed.Extents));
			}
		}
	});
	if (physics) UpdateRooms(scene);
}

std::vector<GroundPad> ValleyProps::Pads(Engine& engine, Scene& scene, Valley& valley)
{
	std::vector<GroundPad> pads;
	valley.SetPads({});
	if (!scene.IsRegistered<ValleyProp>()) return pads;
	scene.Each<ValleyProp>([&](Entity entity, ValleyProp& prop)
	{
		if (!prop.collide || !prop.flattenGround || !scene.Has<MeshRenderer>(entity) || !scene.Has<Transform>(entity) || scene.Has<Parent>(entity)) return;
		const std::vector<PhysicsBox>* boxes = BoxesFor(engine, scene.Get<MeshRenderer>(entity).meshAsset);
		if (boxes->empty()) return;
		// The footprint: the model's boxes turned like the prop (ignoring its height).
		Transform t = scene.Get<Transform>(entity);
		t.position.y = 0.0f;
		const XMMATRIX world = t.Matrix();
		float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
		for (const PhysicsBox& box : *boxes)
		{
			BoundingBox placed;
			BoundingBox(box.center, box.halfExtents).Transform(placed, world);
			x0 = std::min(x0, placed.Center.x - placed.Extents.x);
			x1 = std::max(x1, placed.Center.x + placed.Extents.x);
			z0 = std::min(z0, placed.Center.z - placed.Extents.z);
			z1 = std::max(z1, placed.Center.z + placed.Extents.z);
		}
		GroundPad pad;
		pad.x = t.position.x;
		pad.z = t.position.z;
		pad.halfX = std::max(t.position.x - x0, x1 - t.position.x) + cfg::kPadMargin;
		pad.halfZ = std::max(t.position.z - z0, z1 - t.position.z) + cfg::kPadMargin;
		valley.SetPads(pads);   // the ground with the pads so far: a house in its plot gets the plot's height
		pad.height = std::round(valley.Height(pad.x, pad.z) / cfg::kVoxelSize) * cfg::kVoxelSize;
		pads.push_back(pad);
	});
	valley.SetPads(pads);
	return pads;
}

void ValleyProps::SetLightLevel(Scene& scene, float level)
{
	if (std::fabs(level - mLightLevel) < 1e-3f) return;
	mLightLevel = level;
	for (const auto& [entity, placed] : mLightIntensity)
	{
		if (!scene.Has<PointLightComponent>(entity) || !scene.Has<ValleyProp>(entity)) continue;
		const ValleyProp& prop = scene.Get<ValleyProp>(entity);
		const bool shown = mEra >= prop.eraFrom && mEra <= prop.eraTo;
		scene.Get<PointLightComponent>(entity).intensity = shown ? placed * level : 0.0f;
	}
}

void ValleyProps::Clear(PhysicsWorld* physics)
{
	if (physics)
	{
		for (auto& [entity, bodies] : mBodies)
			for (BodyId body : bodies) physics->RemoveBody(body);
	}
	mBodies.clear();
	mLightIntensity.clear();
	mLightLevel = 1.0f;
	mPropRooms.clear();
	mRooms.clear();
}
