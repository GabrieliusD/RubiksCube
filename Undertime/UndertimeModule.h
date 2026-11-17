#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include "Config.h"
#include "VoxelWorld.h"
#include "Player.h"
#include "Hud.h"
#include "WorldCollision.h"
#include "Fragments.h"
#include "CaveGenerator.h"
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
	void OnStartup(Engine& engine) override;
	void OnUpdate(Engine& engine, float deltaTime) override;
	void OnImGui(Engine& engine) override;
	void OnShutdown(Engine& engine) override;

private:
	enum class State { Title, Playing, Paused };
	enum class WorldRenderer { RayMarched, Meshes };

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
	void SetWorldRenderer(Engine& engine, WorldRenderer mode);
	void CreateScenery(Engine& engine);
	void CreateViewModels(Engine& engine);
	void RebuildChunk(Engine& engine, int chunk);

	// Per frame
	void UpdateState(Engine& engine);
	void UpdateMining(Engine& engine, float deltaTime);
	void Swing(Engine& engine);
	void SpawnDebris(Engine& engine, const VoxelWorld::CarveResult& carve);
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
	bool IsInside() const { return player.position.z > 0.0f; }

	VoxelWorld world;
	uint32_t seed = 0;
	CaveLayout layout;
	int teleportZone = -1;
	char seedInput[16] = "";
	float generationMs = 0.0f;
	uint32_t pendingSeed = 0;
	bool regenerateRequested = false;
	PlayerController player;
	WorldCollision collision;
	FragmentSystem fragments;
	std::vector<VoxelWorld::Box> changedScratch;
	int detachedCount = 0;
	int lastSupportVisited = 0;
	State state = State::Title;
	MessageLog messages;

	// Rendering
	MaterialHandle vertexColorMaterial;
	WorldRenderer worldRenderer = cfg::kStartRayMarched ? WorldRenderer::RayMarched : WorldRenderer::Meshes;
	VoxelVolumeHandle voxelVolume;
	Entity voxelEntity = INVALID_ENTITY;
	std::vector<Entity> chunkEntities;
	std::vector<MeshHandle> chunkMeshes;
	std::vector<MeshHandle> sceneryMeshes;
	std::vector<Entity> sceneryEntities;
	Entity mountainEntity = INVALID_ENTITY;
	Entity pickEntity = INVALID_ENTITY;
	Entity lanternEntity = INVALID_ENTITY;
	MeshHandle pickMesh, lanternMesh;

	// Mining
	Target target;
	float swingTimer = 0.0f;        // 0..1 through the current swing; the pick lands at 1
	bool waitForRelease = false;    // the click that captured the mouse doesn't mine
	float swingAmount = 0.0f;       // 0..1, eases in while mining
	uint32_t swingCount = 0;        // varies the jagged edge of each bite
	int voxelsMined = 0;

	// Gear (upgrade levels, 0-based; trading arrives in milestone 2)
	int pickLevel = 0;
	int lanternLevel = 0;

	DirectX::XMFLOAT3 lanternWorld{ 0, 0, 0 };
	float time = 0.0f;
};
