#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include "Config.h"
#include "VoxelWorld.h"
#include "Player.h"
#include "Hud.h"
#include "WorldCollision.h"
#include "Fragments.h"
#include "LooseBlocks.h"
#include "CaveGenerator.h"
#include "CaveMap.h"
#include "SceneComponents.h"
#include "Valley.h"
#include "ValleyScenery.h"
#include "ValleyProps.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

// UNDERTIME: first-person mining in a mountain where time runs slower the deeper you go.
//
// Milestone 1: fine voxel world (ray-marched by the engine, or exposed-face meshes; V
// toggles), first-person controller with voxel collision, lantern light, and swing-based
// mining that carves a chunk of voxels per hit.
class UndertimeModule : public IGameModule
{
public:
	EngineConfig GetConfig() const override;
	void RegisterTypes(GameTypes& types) override;
	void OnStartup(Engine& engine) override;
	void OnUpdate(Engine& engine, float deltaTime) override;
	void OnImGui(Engine& engine) override;
	void OnShutdown(Engine& engine) override;
	// Hot reload while playing keeps the mountain (seed) and where the player stands.
	std::string SaveState(Engine& engine) override;
	void LoadState(Engine& engine, const std::string& state) override;

private:
	enum class State { Title, Playing, Paused };
	enum class WorldRenderer { RayMarched, Meshes };
	enum class Tool { Pick, Shovel };

	struct Target
	{
		bool valid = false;
		int x = 0, y = 0, z = 0;              // voxel
		cfg::Block block = cfg::Block::Air;
		DirectX::XMFLOAT3 point{ 0, 0, 0 };   // where the ray hit (m)
		int normal[3] = { 0, 0, 0 };          // face of the voxel that was hit
		BodyId fragment = kInvalidBody;       // set when aiming at a loose chunk instead of the wall
	};

	// Setup
	void CreateWorldRendering(Engine& engine);
	// Generates a new mountain for `seed` and rebuilds everything that depends on it.
	void NewWorld(Engine& engine, uint32_t seed);
	static uint32_t RandomSeed();
	// "v0.4 (mining feel)  Release  built 18 Nov 19:05", from cfg::kGameVersion and the
	// game module's own file time.
	static std::string BuildStamp();
	void SetWorldRenderer(Engine& engine, WorldRenderer mode);
	void CreateScenery(Engine& engine);
	void CreateViewModels(Engine& engine);
	void RebuildChunk(Engine& engine, int chunk);

	// Per frame
	void UpdateState(Engine& engine);
	void UpdateMining(Engine& engine, float deltaTime);
	void Swing(Engine& engine);
	// How far along the view ray it leaves the player's capsule: physics rays start there, or
	// they hit the player's own body (it is a physics body, so falling boulders hit you).
	float RayStartOutsidePlayer() const;
	// Voxels the pick knocked out of the wall: one loose block each, tumbling out of the face.
	void SpawnBlocksFromFace(Engine& engine, const std::vector<VoxelWorld::RemovedVoxel>& voxels, const Target& hit);
	// Voxels knocked off (or left of) a boulder: one loose block each, where they were.
	void SpawnBlocksFromBoulder(Engine& engine, const ChipResult& chip, const DirectX::XMFLOAT3& out);
	void UpdateShovel(Engine& engine, float deltaTime);
	int BackpackCount() const;
	// Pushes a world edit to the GPU volume, the collision slabs and (in mesh mode) the chunk
	// meshes; with `checkSupport`, rock left hanging free breaks off and falls.
	void ApplyWorldChange(Engine& engine, const VoxelWorld::Box& box, bool checkSupport);
	void DebugDropRock(Engine& engine);
	void DebugCaveIn(Engine& engine);
	void DebugTeleportToNextZone(Engine& engine);
	void UpdateCamera(Engine& engine);
	void UpdateViewModels(Engine& engine, float deltaTime);
	void UpdateLighting(Engine& engine);

	float Depth() const { return player.position.z; }   // metres into the mountain (x10 on the HUD)
	// In the cave grid (the mountain over it is outside: you can climb it).
	bool IsInside() const
	{
		return player.position.z > 0.0f && player.position.x >= 0.0f && player.position.x <= cfg::kGridX && player.position.y < cfg::kGridY;
	}

	VoxelWorld world;
	uint32_t seed = 0;
	// From the scene: the CaveWorld's seed (0 = random) and rock material, and the PlayerStart, if any.
	uint32_t sceneSeed = 0;
	std::string sceneRockMaterial;
	bool hasSceneSpawn = false;
	DirectX::XMFLOAT3 sceneSpawn{ 0, 0, 0 };
	float sceneSpawnYaw = 0.0f;
	CaveLayout layout;
	CaveMap map;
	bool mapOpen = false;
	float mapRefreshTimer = 0.0f;
	int teleportZone = -1;
	char seedInput[16] = "";
	float generationMs = 0.0f;
	uint32_t pendingSeed = 0;
	bool regenerateRequested = false;
	PlayerController player;
	WorldCollision collision;
	FragmentSystem fragments;
	LooseBlocks blocks;
	int detachedCount = 0;
	int lastSupportVisited = 0;
	State state = State::Title;
	MessageLog messages;
	std::string buildStamp;

	// Rendering
	MaterialHandle vertexColorMaterial;
	WorldRenderer worldRenderer = cfg::kStartRayMarched ? WorldRenderer::RayMarched : WorldRenderer::Meshes;
	VoxelVolumeHandle voxelVolume;
	Entity voxelEntity = INVALID_ENTITY;
	std::vector<Entity> chunkEntities;
	std::vector<MeshHandle> chunkMeshes;
	std::unique_ptr<Valley> valley;
	ValleyScenery scenery;
	ValleyProps props;
	int era = 1;                     // which era's town stands (N cycles it, for now)
	DirectX::XMFLOAT3 devSun{ 0, 0, 0 };   // UNDERTIME_DEV_SUN, for checking shadows
	Entity pickEntity = INVALID_ENTITY;
	Entity shovelEntity = INVALID_ENTITY;
	Entity lanternEntity = INVALID_ENTITY;
	MeshHandle pickMesh, shovelMesh, lanternMesh;

	// Mining
	Tool tool = Tool::Pick;
	Target target;
	float swingTimer = 0.0f;        // 0..1 through the current swing; the pick lands at 1
	bool waitForRelease = false;    // the click that captured the mouse doesn't mine
	float swingAmount = 0.0f;       // 0..1, eases in while mining
	uint32_t swingCount = 0;        // varies the jagged edge of each bite
	bool wasMining = false;
	float hitStop = 0.0f;           // seconds the pick stays frozen in the rock after a hit
	float hitFeedback = 0.0f;       // seconds left of the impact jolt / shake / pick recoil
	float hitStrength = 0.0f;       // 1 for a clean bite, less for a hollow hit
	int voxelsMined = 0;

	// Shovel
	std::vector<BodyId> scoopPreview;   // the loose blocks the next scoop takes
	float shovelTimer = 0.0f;           // 0..1 through the current scoop; it takes the blocks at 1
	float shovelDip = 0.0f;             // s left of the scoop animation
	bool wasShovelling = false;
	float fullWarning = 0.0f;

	// Backpack: blocks per type (selling arrives in milestone 2).
	LooseBlocks::Counts backpack{};
	LooseBlocks::Counts recentPickup{};   // shown as "+3 Copper"
	float pickupToast = 0.0f;

	// Gear (upgrade levels, 0-based; trading arrives in milestone 2)
	int pickLevel = 0;
	int lanternLevel = 0;

	DirectX::XMFLOAT3 lanternWorld{ 0, 0, 0 };
	float time = 0.0f;
};
