#pragma once
// The valley's terrain, water and forest as scene entities: built for play (with collision)
// and for the editor preview (without). The entities have no EntityInfo, so they're never
// saved; the meshes stay with this object, so the entities can be put back if the scene
// loses them (editor undo and scene reloads rebuild the edit scene).
#include "Valley.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <Scene/Components.h>
#include <vector>

class ValleyScenery
{
public:
	// `physics`: where terrain and tree-trunk collision goes, or nullptr for none.
	void Build(Engine& engine, Scene& scene, const Valley& valley, PhysicsWorld* physics);
	void Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics);
	bool Built() const { return !mItems.empty(); }

	// Puts the entities back if the scene no longer has them (true if it had to).
	bool EnsureEntities(Scene& scene);
	void SetVisible(Scene& scene, bool visible);
	float BuildMs() const { return mBuildMs; }
	// All of them sit under one root entity, so the hierarchy shows a single (folded) row.
	Entity Root() const { return mRoot; }
	const std::vector<Entity>& Entities() const { return mEntities; }

private:
	struct Item
	{
		MeshHandle mesh;
		MaterialHandle material;
		Transform transform;
	};
	void Spawn(Scene& scene);
	// The water's flow map (RGBA8, one texel per metre over [x0, x1) x [z0, z1)).
	static std::vector<uint8_t> BuildFlowMap(const Valley& valley, float& x0, float& z0, float& x1, float& z1);
	static std::vector<DirectX::XMFLOAT3> FallSprays(const Valley& valley);

	std::vector<Item> mItems;
	Entity mRoot = INVALID_ENTITY;
	std::vector<Entity> mEntities;
	std::vector<MeshHandle> mOwnedMeshes;   // terrain, water, far trees (tree models belong to the assets)
	std::vector<BodyId> mBodies;
	MaterialHandle mWaterMaterial;
	TextureHandle mFlowMap;                  // the water's flow, depth and foam (Shaders/Water.hlsl)
	std::vector<DirectX::XMFLOAT3> mSprays;   // where spray rises at the falls
	bool mVisible = true;
	float mBuildMs = 0.0f;
};
