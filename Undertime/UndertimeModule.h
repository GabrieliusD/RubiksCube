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
#include "Economy.h"
#include "Building.h"
#include <algorithm>
#include <array>
#include <deque>
#include <unordered_set>
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
	enum class State { Title, Playing, Paused, Talking, Board, Shop, Menu };
	enum class WorldRenderer { RayMarched, Meshes };
	enum class Tool { Pick, Shovel, Hammer, Axe };
	// The windows of State::Menu: the hammer's piece list, a crate, your bench, a fire.
	enum class Menu { None, Pieces, Store, Bench, Cook };

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
	// Which sealed band between zones a bedrock voxel is part of (1 = between zones 1 and 2),
	// 0 if it isn't one (a pocket's shell, the grid's shell).
	int SealBand(int x, int y, int z) const;
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
	Entity hammerEntity = INVALID_ENTITY, axeEntity = INVALID_ENTITY;
	MeshHandle pickMesh, shovelMesh, lanternMesh, hammerMesh, axeMesh;
	bool HasTool(Tool t) const { return t != Tool::Axe || purse.Count("axe") > 0; }

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

	// Gear (upgrade levels, 0-based; bought from the smith and the stores)
	int pickLevel = 0;
	int lanternLevel = 0;
	int packLevel = 0;
	int PackCapacity() const { return cfg::kPackCapacity[std::clamp(packLevel, 0, 2)]; }

	// ---- Money, shops, the yard, the barrow, the claim (Docs/Hauling.md; Hauling.cpp) ----
	void StartHauling(Engine& engine);              // OnStartup, after the world is made
	void StopHauling(Engine& engine);               // OnShutdown
	void UpdateHauling(Engine& engine, float dt);   // every frame while playing
	void AfterHauling(Engine& engine, float dt);    // every frame, in any state (the yard, the barrow's body)
	// E with nothing else to do (the barrow...). Returns true if it did something.
	bool HaulingInteract(Engine& engine);
	std::string HaulingPrompt() const;              // the "E ..." line, or empty
	void HaulingHud(std::vector<std::string>& bag) const;
	nlohmann::json SaveHauling() const;
	void LoadHauling(Engine& engine, const nlohmann::json& j);
	void ResetHauling(Engine& engine);              // a new game
	void DevHauling(Engine& engine);                // UNDERTIME_DEV_HAUL (Hauling.cpp)
	// Shops: a "Trade" topic with whoever keeps one, then the shop window.
	void ShopTopics(const npc::DialogContext& c, std::deque<npc::Topic>& out);
	void OpenShop(Engine& engine, econ::Shop shop);
	void CloseShop(Engine& engine);
	void DrawShop(Engine& engine);
	// Goods: blocks in the backpack (by block name) plus everything in the purse.
	int Have(const std::string& id) const;
	void Take(const std::string& id, int count);
	void Give(Engine& engine, const std::string& id, int count);   // upgrades set gear, tools arrive
	econ::Purse purse;
	econ::Shop shopOpen = econ::Shop::Count;
	econ::Shop pendingShop = econ::Shop::Count;   // chosen in a conversation; opens when it ends
	std::string shopKeeper;
	std::string shopNote;           // the last thing bought or sold
	float yardTimer = 0.0f;         // collecting the yard's payments into one toast
	int yardCents = 0, yardBlocks = 0;
	Entity yardEntity = INVALID_ENTITY;
	MeshHandle yardMesh;
	float yardGround = 0.0f;
	int yardRestore = 0;            // the town's stone to put back in the yard (loading a save)
	std::vector<BodyId> yardBodies;
	// Town projects (econ::Projects): stone used from the yard, by project id; finished ones.
	void UpdateProjects(Engine& engine);
	void DrawProjects();
	std::map<std::string, int> projectStone;
	std::map<std::string, double> projectDone;   // the outside day it was finished
	double projectClock = -1.0;
	float projectStoneDebt = 0.0f;

	// The wheelbarrow (Barrow.cpp): bought at the trading post, pushed with E, filled with the
	// shovel when it's parked within reach, tipped with Q.
	void StartBarrow(Engine& engine);
	void StopBarrow(Engine& engine);
	void ResetBarrow(Engine& engine);
	void BuyBarrow(Engine& engine);
	bool HasBarrow() const { return barrow.owned; }
	void UpdateBarrow(Engine& engine, float dt);
	void AfterBarrow(Engine& engine, float dt);
	bool BarrowInteract(Engine& engine);
	std::string BarrowPrompt() const;
	void BarrowHud(std::vector<std::string>& bag) const;
	nlohmann::json SaveBarrow() const;
	void LoadBarrow(Engine& engine, const nlohmann::json& j);
	float BarrowSpeedScale() const;
	int BarrowCount() const;
	// Whether the next scoop goes into the barrow (parked within reach), and where it lands.
	bool BarrowTakesScoops(DirectX::XMFLOAT3& into) const;
	void AddToBarrow(const LooseBlocks::Counts& counts);
	void PlaceBarrow(Engine& engine, const DirectX::XMFLOAT3& at, float yaw);
	void RebuildBarrowLoad(Engine& engine);
	void ParkBarrow(Engine& engine);
	// ---- Your claim and building on it (Claim.cpp) ----
	void StartClaim(Engine& engine);
	void StopClaim(Engine& engine);
	void ResetClaim(Engine& engine);
	void UpdateClaim(Engine& engine, float dt);     // the hammer, while it's the tool
	bool ClaimInteract(Engine& engine);             // E on furniture, doors, the tent
	std::string ClaimPrompt() const;
	void ClaimHud(std::vector<std::string>& lines) const;
	nlohmann::json SaveClaim() const;
	void LoadClaim(Engine& engine, const nlohmann::json& j);
	void RebuildClaim(Engine& engine);
	void OpenMenu(Engine& engine, Menu which);
	void CloseMenu(Engine& engine);
	void DrawMenu(Engine& engine);
	// The claim's grid corner (voxel 0, 0, 0 of its model) in the world.
	DirectX::XMFLOAT3 ClaimOrigin() const { return claim.origin; }
	bool OnClaim(const DirectX::XMFLOAT3& p, float margin = 0.0f) const;
	// Building materials come from the backpack and purse, a barrow parked close by, and (on
	// the claim) your crates.
	int BuildHave(const std::string& id) const;
	void BuildTake(const std::string& id, int count);
	void Sleep(Engine& engine);
	build::Plan claimPlan{ cfg::kClaimCells, cfg::kClaimCells, cfg::kClaimLevels };
	struct Claim
	{
		DirectX::XMFLOAT3 origin{ 0, 0, 0 };
		Entity entity = INVALID_ENTITY, ghost = INVALID_ENTITY, tent = INVALID_ENTITY;
		MeshHandle mesh, ghostMesh, tentMesh;
		BodyId body = kInvalidBody, tentBody = kInvalidBody;
		bool dirty = true;
		int selected = 0;           // the piece the hammer places (build::Pieces index)
		int turn = 0;               // R turns cell pieces
		int ghostKey = -1;          // the piece and turn the ghost mesh was made for
		build::Piece candidate;     // where the selected piece would go
		bool candidateValid = false;
		std::string why;            // why it can't go there
		int aimed = -1;             // the piece under the crosshair
		float swingTimer = 0.0f;
		float takeDown = 0.0f;
		econ::Goods store;          // what's in your crates
		std::vector<Entity> lamps;
	} claim;
	Menu menu = Menu::None;

	// ---- Hunger and food (Food.cpp) ----
	void UpdateHunger(Engine& engine, float dt);    // drains in your own time; F eats
	bool Eat(const std::string& id);
	void DrawCooking(Engine& engine);
	void OnSlept(Engine& engine, double days);      // a night passed (the bed, the tent)
	float Strength() const { return hunger > 0.0f ? 1.0f : cfg::kHungryStrength; }
	float hunger = 1.0f;            // 1 full .. 0 empty
	bool hungerWarned = false, starvingWarned = false;

	// ---- Wood (Wood.cpp): the axe fells trees; stumps grow back with outside time ----
	void StartWood(Engine& engine);
	void StopWood(Engine& engine);
	void ResetWood(Engine& engine);
	void UpdateWood(Engine& engine, float dt);
	std::string WoodPrompt() const;
	nlohmann::json SaveWood() const;
	void LoadWood(Engine& engine, const nlohmann::json& j);
	int AimedTree() const;          // the tree the axe points at, -1 if none
	std::map<int, double> felled;   // tree -> the outside day it was felled
	std::map<int, Entity> stumps;
	MeshHandle stumpMesh;
	float axeTimer = 0.0f;
	bool axeSwinging = false;
	int chopTree = -1, chops = 0;
	int axeTarget = -1;
	float woodTimer = 0.0f;

	// ---- Things placed in the world (Placeables.cpp: props, ladders, chutes, torches;
	// Machines.cpp: drills, belts, engines, the mill, rails and the cart) ----
	struct Placed
	{
		std::string item;
		DirectX::XMFLOAT3 at{ 0, 0, 0 };   // its base: on the floor, or against the wall
		float yaw = 0.0f;
		float height = 0.0f;               // props and ladders
		Entity entity = INVALID_ENTITY, light = INVALID_ENTITY, extra = INVALID_ENTITY;
		BodyId body = kInvalidBody;
		uint32_t anchor = UINT32_MAX;      // a prop: the voxel it holds up
		double ownTime = 0.0;              // seconds of its own time (torches burn, machines work)
		// Machines
		float fuel = 0.0f;                 // coal (steam) in units
		double wear = 0.0;                 // own seconds of work since the last repair
		LooseBlocks::Counts hopper{};
		double progress = 0.0;             // a drill: metres bored; a mill: stone crushed
		bool on = true;
	};
	void UpdatePlacing(Engine& engine, float dt);   // the hammer with one of your things
	void UpdatePlaced(Engine& engine, float dt);    // torches burn, ladders climb, machines work
	bool PlacedInteract(Engine& engine);
	std::string PlacedPrompt() const;
	void SpawnPlaced(Engine& engine, Placed& p);
	void DespawnPlaced(Engine& engine, Placed& p);
	void RemovePlacedAt(Engine& engine, int index, bool giveBack);
	nlohmann::json SavePlaced() const;
	void LoadPlaced(Engine& engine, const nlohmann::json& j);
	void StopPlaced(Engine& engine);
	MeshHandle PlacedMesh(Engine& engine, const std::string& item, float height);
	// Own seconds per real second at `at`: things work in the time of where they stand.
	double OwnRate(const DirectX::XMFLOAT3& at) const;
	bool InCave(const DirectX::XMFLOAT3& p) const { return p.z > 0.0f && p.x >= 0.0f && p.x <= cfg::kGridX && p.y < cfg::kGridY; }
	std::vector<Placed> placed;
	std::unordered_set<uint32_t> propHeld;   // voxels props hold up (Structure.h)
	std::map<std::string, MeshHandle> placedMeshes;
	std::string placeItem;                   // the hammer places this from your pack (instead of claim pieces)
	int placedAimed = -1;
	float placedTakeDown = 0.0f;
	int climbLadder = -1;
	// Machines.cpp: where a machine may go, and what it does each frame.
	bool PlaceMachine(Engine& engine, const std::string& item, const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal, Placed& out, std::string& why);
	void UpdateMachines(Engine& engine, float dt);
	bool MachineInteract(Engine& engine, Placed& p);
	std::string MachinePrompt(const Placed& p) const;
	bool IsMachine(const std::string& item) const;
	MeshHandle MachineMesh(Engine& engine, const std::string& item);
	void SpawnMachine(Engine& engine, Placed& p);
	void UpdateDrill(Engine& engine, Placed& p, double hours);
	void UpdateBelts(Engine& engine, float dt);
	void UpdateMill(Engine& engine, Placed& p, double hours);
	void UpdateCart(Engine& engine, Placed& p, float dt);
	bool EngineNear(const DirectX::XMFLOAT3& at) const;
	bool CartTakesScoops(DirectX::XMFLOAT3& into, int& cart) const;
	MeshHandle PlacedMeshFor(Engine& engine, const std::string& key, const VoxelModel& model);
	int heldCart = -1;              // the cart you're pushing (index in placed)
	std::vector<int> beltLoad;      // blocks on each placed belt (parallel to placed; rebuilt as needed)
	float beltWake = 0.0f;

	// ---- Farming (Farm.cpp): crops on farm plots, by outside day ----
	void StopFarm(Engine& engine);
	void UpdateFarm(Engine& engine, float dt);
	bool FarmInteract(Engine& engine);
	std::string FarmPrompt() const;
	nlohmann::json SaveFarm() const;
	void LoadFarm(Engine& engine, const nlohmann::json& j);
	struct Planting { int crop = 0; double day = 0.0; int harvestedYear = -1000; Entity entity = INVALID_ENTITY; int shown = -1; };
	std::map<int, Planting> plantings;   // by plot cell key (x, y, z on the claim)
	std::map<int, MeshHandle> cropMeshes;  // by crop and look
	float farmTimer = 0.0f;

	struct Barrow
	{
		bool owned = false;
		bool held = false;
		DirectX::XMFLOAT3 position{ 0, 0, 0 };   // on the ground under the middle of the tray
		float yaw = 0.0f;
		LooseBlocks::Counts load{};
		float tipping = -1.0f;      // >= 0: seconds into tipping out
		int tipped = 0;             // blocks out so far while tipping
		bool fillPack = false;      // B: scoops go into the pack even next to the barrow
		Entity entity = INVALID_ENTITY, loadEntity = INVALID_ENTITY;
		MeshHandle mesh, loadMesh;
		int loadShown = -1;         // the load the heap mesh was built for
		BodyId body = kInvalidBody; // while parked
	} barrow;

	DirectX::XMFLOAT3 lanternWorld{ 0, 0, 0 };
	float time = 0.0f;
};
