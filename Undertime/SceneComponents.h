#pragma once
#include <Game/GameModule.h>
#include <Renderer/RenderTypes.h>
#include <cstdint>
#include <memory>
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

// The valley outside the cave. One per scene; the game builds it on Play, and the editor
// shows it while this is in the scene (ValleyPreviewSystem).
struct ValleyWorld
{
	int previewEra = 1;   // which era's ValleyProps the editor shows (1 Camp .. 6 Gone)
};

// Something placed in the valley (buildings, props, lights): the eras it exists in, and
// whether it stands on the ground and blocks the player.
struct ValleyProp
{
	int eraFrom = 1;            // first and last era it exists in (inclusive):
	int eraTo = 6;              // 1 Camp, 2 Settlement, 3 Boomtown, 4 Industry, 5 Abandoned, 6 Gone
	bool snapToGround = true;   // its Transform follows the valley's ground (y)...
	float groundOffset = 0.0f;  // ... this many metres above it (a light over a fire, a wheel's axle)
	bool collide = true;        // its .vox model blocks the player (boxes baked from the model)
	bool flattenGround = true;  // with collide: the ground under its footprint is levelled (rebuild the editor preview to see a move)
};

// Marks the editor's valley preview entities, so the play world can drop its copies of them.
struct ValleyPreview
{
};

// Editor helper: shows the valley (terrain, water, forest) around the edit scene while it has a
// ValleyWorld, puts ValleyProps on the ground and shows only those of the ValleyWorld's previewEra.
class ValleyPreviewSystem : public ISystem
{
public:
	ValleyPreviewSystem();
	~ValleyPreviewSystem() override;
	bool RunsInEditMode() const override { return true; }
	void OnEditUpdate(Engine& engine, float deltaTime) override;
	void OnPlayBegin(Engine& engine) override;
	void OnInspectorGui(Engine& engine, Entity entity) override;
	void OnUnload(Engine& engine) override;

private:
	struct State;
	std::unique_ptr<State> mState;
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
