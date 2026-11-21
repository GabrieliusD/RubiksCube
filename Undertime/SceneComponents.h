#pragma once
#include <Game/GameModule.h>
#include <Renderer/RenderTypes.h>
#include <cstdint>
#include <string>

// UNDERTIME's scene components (registered in UndertimeModule::RegisterTypes).

// The generated mountain. One per scene; its Transform is ignored (the voxel grid's
// corner is always at the world origin).
struct CaveWorld
{
	uint32_t seed = 0;   // 0: a new random mountain every play
	// Optional material for the rock (world volume and boulders), e.g. one using a shader graph
	// with Block Index / Local Position nodes. Empty: the built-in look.
	std::string rockMaterial;
};

// Where the player starts (feet position and facing from the Transform). Without one,
// the player starts outside the mountain entrance (cfg::kSpawnX/kSpawnZ).
struct PlayerStart
{
};

// Editor helper: "Generate preview" on a CaveWorld shows its mountain in the edit scene
// (a runtime voxel volume, never saved), so things can be placed against the real caves.
class CavePreviewSystem : public ISystem
{
public:
	bool RunsInEditMode() const override { return true; }
	void OnEditUpdate(Engine& engine, float deltaTime) override;
	void OnInspectorGui(Engine& engine, Entity entity) override;
	void OnUnload(Engine& engine) override;

private:
	void Generate(Engine& engine, uint32_t seed);
	void Clear(Engine& engine);

	VoxelVolumeHandle mVolume;
	Entity mEntity = INVALID_ENTITY;
	uint32_t mSeed = 0;
	float mGenerationMs = 0.0f;
};
