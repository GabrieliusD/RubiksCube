#pragma once
// The valley's terrain, water and forest as scene entities: built for play (with collision)
// and for the editor preview (without). The entities have no EntityInfo, so they're never
// saved; the meshes stay with this object, so the entities can be put back if the scene
// loses them (editor undo and scene reloads rebuild the edit scene).
//
// Level of detail: everything is 0.25 m blocks, but far away it's drawn coarser. The terrain
// is drawn in regions (cfg::kTerrainRegion x kTerrainRegion chunks), each at 0.25, 0.5 or
// 1 m blocks by its distance from the camera (cfg::kLodDistance), rebuilt on worker threads
// as the camera moves and swapped in when ready. Trees and mountain crags have a 0.5 m
// version (their model halved) for far away. Collision is 0.25 m everywhere, but only exists
// around the player (cfg::kCollisionRadius), built on the worker threads as you go.
#include "Valley.h"
#include "Config.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <Scene/Components.h>
#include <memory>
#include <vector>

class ValleyScenery
{
public:
	ValleyScenery();
	~ValleyScenery();

	// `physics`: where terrain, tree-trunk and crag collision goes, or nullptr for none.
	// `focus`: where the camera starts (the first level of detail and collision are built
	// around it before this returns).
	void Build(Engine& engine, Scene& scene, const Valley& valley, PhysicsWorld* physics, const DirectX::XMFLOAT3& focus);
	// Every frame: the level of detail follows `eye` (the camera), and so does collision.
	void Update(Engine& engine, Scene& scene, const DirectX::XMFLOAT3& eye);
	void Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics);
	bool Built() const { return !mItems.empty(); }

	// Puts the entities back if the scene no longer has them (true if it had to).
	bool EnsureEntities(Scene& scene);
	void SetVisible(Scene& scene, bool visible);
	float BuildMs() const { return mBuildMs; }
	// All of them sit under one root entity, so the hierarchy shows a single (folded) row.
	Entity Root() const { return mRoot; }
	const std::vector<Entity>& Entities() const { return mEntities; }

	// How the valley is drawn (switchable while playing, in the Game panel).
	struct Settings
	{
		float lodDistance[3] = { cfg::kLodDistance[0], cfg::kLodDistance[1], cfg::kLodDistance[2] };
		bool fade = cfg::kLodFade;            // crossfade between levels (and near/far models)
		float fadeSeconds = cfg::kLodFadeSeconds;
		bool compact = cfg::kLodCompact;      // compact terrain vertices (changing it rebuilds every region)
	};
	const Settings& GetSettings() const { return mSettings; }
	void SetSettings(const Settings& settings);

	// For the Game panel.
	struct Stats
	{
		int regions[4]{};          // terrain regions drawn at 0.25 m with ground cover, 0.25 m, 0.5 m and 1 m
		size_t triangles = 0;      // terrain and water triangles drawn
		size_t coverTriangles = 0; // ground cover triangles
		size_t meshBytes = 0;      // GPU memory of the terrain, water and ground cover meshes
		int fading = 0;            // regions and models crossfading
		int rebuilding = 0;        // regions being rebuilt
		int collisionChunks = 0;   // chunks with collision
		float regionMs = 0.0f;     // the last region rebuild (on a worker thread)
		float collisionMs = 0.0f;  // the last collision chunk: building on a worker, then
		float bodyMs = 0.0f;       // making its body (on the main thread)
	};
	Stats GetStats() const;

	// The forest's trees (felling them, Wood.cpp): where each stands and its model kind.
	struct TreePlace
	{
		DirectX::XMFLOAT3 at{ 0, 0, 0 };
		int kind = 0;
		size_t nearItem = SIZE_MAX, farItem = SIZE_MAX, model = SIZE_MAX;
		int chunk = 0;
		float scale = 1.0f;
	};
	const std::vector<TreePlace>& Trees() const { return mTrees; }
	// A tree's growth: 0 felled (gone; its trunk no longer blocks), up to 1 grown. Young trees
	// are drawn scaled; their trunks block from half grown.
	void SetTreeGrowth(Scene& scene, int tree, float scale);

private:
	struct Item
	{
		MeshHandle mesh;
		MaterialHandle material;
		Transform transform;
		bool shown = true;      // (and drawn only with a mesh, while the valley is visible)
		float dither = 1.0f;    // MeshRenderer::dither, ditherInvert (crossfading)
		bool invert = false;
	};
	struct Lod;   // regions, model swaps, collision chunks and the worker threads (ValleyScenery.cpp)
	void Spawn(Scene& scene);
	void ApplyItem(Scene& scene, size_t item);   // the item's state onto its entity
	// The water's flow map (RGBA8, one texel per metre over [x0, x1) x [z0, z1)).
	static std::vector<uint8_t> BuildFlowMap(const Valley& valley, float& x0, float& z0, float& x1, float& z1);
	static std::vector<DirectX::XMFLOAT3> FallSprays(const Valley& valley);

	std::vector<Item> mItems;
	Entity mRoot = INVALID_ENTITY;
	std::vector<Entity> mEntities;
	std::vector<MeshHandle> mOwnedMeshes;   // far trees and crags (the rest belongs to the assets or to mLod)
	std::vector<BodyId> mBodies;            // the shaft grates (the terrain's own are in mLod)
	std::vector<TreePlace> mTrees;
	std::unique_ptr<Lod> mLod;
	PhysicsWorld* mPhysics = nullptr;
	MaterialHandle mWaterMaterial;
	TextureHandle mFlowMap;                  // the water's flow, depth and foam (Shaders/Water.hlsl)
	std::vector<DirectX::XMFLOAT3> mSprays;   // where spray rises at the falls
	bool mVisible = true;
	float mBuildMs = 0.0f;
	Settings mSettings;
	bool mRebuildAll = false;   // the next Update rebuilds every region (compact switched)
};
