#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include "Config.h"
#include "VoxelWorld.h"
#include "Player.h"
#include "Hud.h"
#include <vector>

// UNDERTIME: first-person mining in a mountain where time runs slower the deeper you go.
//
// Milestone 1: voxel grid with exposed-face chunk meshes, first-person controller with
// voxel collision, lantern light, and DDA ray-cast mining with a progress ring.
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

	struct Target
	{
		bool valid = false;
		int x = 0, y = 0, z = 0;
		cfg::Block block = cfg::Block::Air;
	};

	// Setup
	void CreateWorldMeshes(Engine& engine);
	void CreateScenery(Engine& engine);
	void CreateViewModels(Engine& engine);
	void RebuildChunk(Engine& engine, int chunk);

	// Per frame
	void UpdateState(Engine& engine);
	void UpdateMining(Engine& engine, float deltaTime);
	void BreakCell(Engine& engine, const Target& target);
	void UpdateCamera(Engine& engine);
	void UpdateViewModels(Engine& engine, float deltaTime);
	void UpdateLighting(Engine& engine);

	float Depth() const { return player.position.z; }   // cells (= metres / 10 on the HUD)
	bool IsInside() const { return player.position.z > 0.0f; }

	VoxelWorld world;
	PlayerController player;
	State state = State::Title;
	MessageLog messages;

	// Rendering
	MaterialHandle vertexColorMaterial;
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
	float mineProgress = 0.0f;
	bool waitForRelease = false;    // the click that captured the mouse doesn't mine
	float swingPhase = 0.0f;
	float swingAmount = 0.0f;       // 0..1, eases in while mining

	// Gear (upgrade levels, 0-based; trading arrives in milestone 2)
	int pickLevel = 0;
	int lanternLevel = 0;

	DirectX::XMFLOAT3 lanternWorld{ 0, 0, 0 };
	float time = 0.0f;
};
