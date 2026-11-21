#include "ValleyProps.h"
#include "SceneComponents.h"
#include "Config.h"
#include <Core/Assets.h>
#include <Voxel/VoxelModel.h>
#include <algorithm>
#include <cmath>

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

void ValleyProps::ApplyEra(Engine& engine, Scene& scene, int era, PhysicsWorld* physics)
{
	if (!scene.IsRegistered<ValleyProp>()) return;
	scene.Each<ValleyProp>([&](Entity entity, ValleyProp& prop)
	{
		const bool shown = era >= prop.eraFrom && era <= prop.eraTo;
		if (scene.Has<MeshRenderer>(entity)) scene.Get<MeshRenderer>(entity).visible = shown;
		if (!physics) return;

		if (scene.Has<PointLightComponent>(entity))
		{
			PointLightComponent& light = scene.Get<PointLightComponent>(entity);
			auto stored = mLightIntensity.find(entity);
			if (stored == mLightIntensity.end()) stored = mLightIntensity.emplace(entity, light.intensity).first;
			light.intensity = shown ? stored->second : 0.0f;
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

void ValleyProps::Clear(PhysicsWorld* physics)
{
	if (physics)
	{
		for (auto& [entity, bodies] : mBodies)
			for (BodyId body : bodies) physics->RemoveBody(body);
	}
	mBodies.clear();
	mLightIntensity.clear();
}
