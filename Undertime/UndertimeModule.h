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
#include "Clock.h"
#include "DayNight.h"
#include "Skylight.h"
#include "NpcWorld.h"
#include "Dialog.h"
#include "Missions.h"
#include "Life.h"
#include "Letters.h"
#include "NpcTools.h"
#include "SaveGame.h"
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
	enum class State { Title, Playing, Paused, Talking, Board };
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
	// Development: `count` stone blocks in the free air ahead of the player (UNDERTIME_DEV_BLOCKS,
	// and the Game panel's loose block tests).
	void DropTestBlocks(Engine& engine, int count);
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
	// The outside clock: advances at the rate where the player is, changes the era while the
	// valley can't be seen, and says how much time passed when you come back out.
	void UpdateClock(Engine& engine, float deltaTime);
	void SetEra(Engine& engine, int newEra);
	std::vector<float> ZoneStarts() const;
	void DrawWatch(const hud::Frame& frame);
	// Dust: a puff where the pick broke rock, a cloud where rock fell, motes in the shafts.
	void RockDust(Engine& engine, const DirectX::XMFLOAT3& at, int count, const DirectX::XMFLOAT4& colour, const DirectX::XMFLOAT3& out, float size);
	void CreateShaftDust(Engine& engine);
	std::vector<Entity> shaftDust;
	// The cave comes alive: lamps in the first zone, light around Time crystal, lantern motes.
	void CreateCaveLife(Engine& engine);
	void CountCrystals(const VoxelWorld::Box& box);
	struct MineLamp { Entity entity; DirectX::XMFLOAT3 light; float phase; };
	std::vector<MineLamp> mineLamps;
	std::unordered_map<uint32_t, int> crystalCells;   // 2 m cell -> crystal voxels in it
	Entity lanternMotes = INVALID_ENTITY;
	Entity rainEmitter = INVALID_ENTITY;   // drops falling around you outside
	float devWeather[3] = { -1.0f, 0.0f, 0.0f };   // UNDERTIME_DEV_WEATHER (overcast < 0: none)

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
	bool showcase = false;              // CaveWorld::showcase or UNDERTIME_DEV_SHOWCASE=1
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
	Skylight skylight;               // daylight reaching into the cave
	LightVolumeHandle skylightVolume;   // ... as the renderer's interior light volume
	Entity voxelEntity = INVALID_ENTITY;
	std::vector<Entity> chunkEntities;
	std::vector<MeshHandle> chunkMeshes;
	std::unique_ptr<Valley> valley;
	ValleyScenery scenery;
	ValleyProps props;
	NpcWorld people;                 // the cast in the valley (Docs/NPCs.md)
	NpcWorld::View PeopleView() const;

	// Talking (E): outside time stops; the conversation runs from Content/Dialog.
	void StartTalk(Engine& engine, int npc);
	void EndTalk(Engine& engine);
	void UpdateTalk(Engine& engine, float dt);
	void DrawConversation(Engine& engine);
	// Barks: a line from someone you pass.
	void UpdateBarks(Engine& engine, float dt);
	npc::Conversation::Situation TalkSituation() const;
	npc::DialogContext ContextFor(int npc) const;
	std::string Speaker(int npc) const;   // their name once you've met, else what they are
	npc::DialogLibrary dialogs;
	npc::PlayerLog peopleLog;        // everything you did with the people (saved)
	npc::Conversation conversation;
	int talkTarget = -1;             // who E would talk to
	float talkEase = 1.0f;           // 0..1: the view turning to them
	float talkFromYaw = 0.0f, talkFromPitch = 0.0f;
	bool choosingGift = false;
	std::vector<float> barkCooldown;
	float barkGlobal = 0.0f, barkLeft = 0.0f;
	std::string barkSpeaker, barkText;
	int devTalk = -1, devTalkSteps = 0;   // UNDERTIME_DEV_TALK="id [continues]": open a conversation at start (screenshots)

	// Missions (Missions.h): from Content/Missions; the notice board by the trading post, lost
	// things in the cave, meetings, deadlines; the journal (Tab).
	void UpdateMissions(Engine& engine);
	void MissionEffect(const npc::Effect& effect);
	int BackpackItem(const std::string& item) const;   // how many of that block name you carry
	void AddToBackpack(const std::map<std::string, int>& items);
	// Whether you're looking at `point` within `reach` (the board, a lost thing).
	bool AimedAt(const DirectX::XMFLOAT3& point, float reach) const;
	void DrawBoard(Engine& engine);
	void DrawJournal(Engine& engine);
	npc::MissionBook missions;
	struct LostThing { std::string mission; Entity entity = INVALID_ENTITY; DirectX::XMFLOAT3 at{}; };
	std::vector<LostThing> lostThings;   // retrieve missions' things lying in caverns
	int lostTarget = -1;                 // the one E would pick up
	Entity noticeBoard = INVALID_ENTITY;
	DirectX::XMFLOAT3 boardAt{};         // where people read it (its front, at eye height)
	bool boardTarget = false;            // E would read it
	bool journalOpen = false;
	std::string boardPlaceName;          // where the board stands now ("post.board", "saloon.board")
	// Lives (Life.h): gatherings you join, graves for the dead (E reads them).
	void UpdateLives(Engine& engine);
	struct Grave { int npc = -1; Entity entity = INVALID_ENTITY; DirectX::XMFLOAT3 at{}; };
	std::vector<Grave> graves;
	int graveTarget = -1;
	NpcTools npcTools;                   // the people inspector and dialog view in the Game panel while playing

	// The save game (SaveGame.h): <project>/Saved/SaveGame.json.
	std::string SavePath(Engine& engine) const;
	void SaveGameNow(Engine& engine);
	bool LoadSavedGame(Engine& engine);
	bool savingAllowed = true;       // not in development views (screenshots must never replace a save)
	bool saveExists = false;
	std::string saveDate;
	float autosaveTimer = 0.0f;
	bool saveRequested = false;
	int era = 1;                     // which era's town stands (the clock's, once the valley is out of sight)
	int debugTimeScale = 0;            // debug ([ and ]): outside time runs 10^this times faster
	bool shadowFade = true;            // debug (J): shadows fade between steps
	ColorGradeSettings grade;          // the colour grade now, easing towards the era's (or the cave's)
	float gradeTime = -1.0f;           // `time` when the grade was last updated (-1: snap to the target)
	OutsideClock clock;
	float timeRate = cfg::kSurfaceRate;   // outside seconds per real second where the player is
	float strobe = 0.0f;                  // 0..1: how far the light blends to the day's average (smoothed)
	double cloudDrift[2] = { 0.0, 0.0 };  // metres the clouds have drifted (with outside time)
	float cloudBlur = 0.0f;               // 0..1, smoothed
	bool wasInside = false;
	double daysAtEntry = 0.0;             // when you last went in (for the summary when you come out)
	int eraAtEntry = 1;
	DirectX::XMFLOAT3 devSun{ 0, 0, 0 };   // UNDERTIME_DEV_SUN, for checking shadows
	bool devHold = false;            // UNDERTIME_DEV_VIEW "... hold": the player stays put (screenshots in mid-air)
	// UNDERTIME_DEV_BLOCKS: timing log
	bool devPerf = false;
	int perfFrames = 0;
	float perfTimer = 0.0f, perfFrameMs = 0.0f, perfUpdateMs = 0.0f, perfPhysicsMs = 0.0f;
public:
	void LogPerf(Engine& engine, float dt, float updateMs);
private:
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
	float rumble = 0.0f;            // 0..1: the ground shaking after a cave-in
	void Sparks(Engine& engine, const Target& at, float hardness);
	int voxelsMined = 0;

	// Shovel
	std::vector<LooseBlocks::Ref> scoopPreview;   // the loose blocks the next scoop takes
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
