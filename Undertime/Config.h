#pragma once
// UNDERTIME tuning: every gameplay number lives here.
#include <cstdint>

namespace cfg
{
	// ---- World grid ---------------------------------------------------------
	// Cell (x, y, z) occupies [x, x+1] x [y, y+1] x [z, z+1] in world space.
	// +Z goes into the mountain; the entrance face is the plane z = 0.
	constexpr int kGridX = 48;
	constexpr int kGridY = 24;
	constexpr int kGridZ = 160;
	constexpr int kCenterX = 24;                  // entrance column
	constexpr int kChunkDepth = 16;               // metres along Z per mesh chunk (mesh renderer)
	constexpr uint32_t kWorldSeed = 0x0DE7B00Bu;  // fixed seed for tests and scenery; games roll their own

	// The layout above is in metres; the world itself is stored as finer voxels.
	constexpr int kVoxelsPerMeter = 4;
	constexpr float kVoxelSize = 1.0f / kVoxelsPerMeter;
	constexpr int kVoxX = kGridX * kVoxelsPerMeter;
	constexpr int kVoxY = kGridY * kVoxelsPerMeter;
	constexpr int kVoxZ = kGridZ * kVoxelsPerMeter;

	// Rendering: ray-marched voxels (engine VoxelVolumeRenderer) or exposed-face meshes.
	// Toggle in game with V.
	constexpr bool kStartRayMarched = true;
	constexpr float kVoxelAoStrength = 0.6f;      // corner ambient occlusion (ray-marched)
	constexpr int kStoneBands = 48;               // palette entries for the stone depth gradient

	// ---- Cave generation (metres unless noted) -----------------------------------
	constexpr int kZoneCount = 5;                 // cave zones from the entrance to the far wall
	constexpr float kSealMin = 3.0f;              // solid rock band between zones (must be dug through)
	constexpr float kSealMax = 6.0f;
	constexpr float kEntranceDepth = 6.0f;        // the crack passage runs from z = 0 to here
	constexpr int kExtraRoomsMin = 2;             // rooms per zone besides entry and exit caverns
	constexpr int kExtraRoomsMax = 5;
	constexpr float kRoomRadiusMin = 3.0f;        // horizontal radius of a cavern
	constexpr float kRoomRadiusMax = 6.5f;
	constexpr float kRoomHeightMin = 2.2f;        // vertical radius of a cavern
	constexpr float kRoomHeightMax = 4.5f;
	constexpr float kHighRoomChance = 0.45f;      // how often a room sits on an upper level
	constexpr float kTunnelRadiusMin = 1.1f;
	constexpr float kTunnelRadiusMax = 1.8f;
	constexpr float kTunnelMinHeight = 1.45f;     // vertical radius floor: tunnels stay >= ~2.1 m tall
	constexpr float kTunnelMaxSlope = 0.65f;      // metres up per metre along (~33 degrees)
	constexpr float kTunnelWander = 0.9f;         // radians of heading noise
	constexpr int kExtraLoops = 2;                // tunnels beyond the spanning tree, per zone
	constexpr float kCheeseScale = 0.075f;        // cycles per metre of the cavern noise
	constexpr float kCheeseThreshold = 0.34f;     // higher = fewer noise caverns
	constexpr float kWallNoiseScale = 0.45f;      // cycles per metre of wall roughness
	constexpr float kWallNoiseAmount = 0.55f;     // metres of wall roughness
	constexpr int kPocketsMin = 1;                // sealed side pockets per zone
	constexpr int kPocketsMax = 3;
	constexpr float kPocketRadiusMin = 1.6f;
	constexpr float kPocketRadiusMax = 3.0f;
	constexpr float kPocketWallMin = 2.0f;        // rock between a pocket and any cave

	// Ore veins: blobs placed in rock close to cave air so they show in the walls. Depth
	// ranges are fractions of the mountain's depth (the original 110 m table, rescaled).
	constexpr int kVeinCount = 700;
	constexpr float kVeinRadiusMin = 0.8f;
	constexpr float kVeinRadiusMax = 1.9f;
	constexpr float kVeinFill = 0.75f;            // fraction of the blob that is ore
	constexpr float kVeinNearAir = 2.5f;          // a vein's centre is at most this far from a cave
	constexpr int kPocketVeins = 4;               // extra veins in the walls of each sealed pocket
	constexpr int kWatchCount = 16;               // Strange watch relics
	constexpr float kWatchMinDepth = 0.27f;
	constexpr float kWatchInPocketChance = 0.6f;  // relics prefer sealed pockets

	struct OreRule
	{
		int block;          // Block value
		float minDepth;     // fraction of depth
		float maxDepth;
		float weight;
	};
	constexpr OreRule kOreRules[] =
	{
		{ 3, 0.00f, 0.59f, 3.0f },   // Copper
		{ 4, 0.16f, 0.82f, 2.0f },   // Silver
		{ 5, 0.38f, 1.00f, 1.6f },   // Gold
		{ 6, 0.64f, 1.00f, 1.2f },   // Time crystal
	};

	// ---- Physics ----------------------------------------------------------------
	constexpr int kCollisionChunk = 8;            // metres per side of a static collision chunk
	constexpr float kCharacterPushStrength = 600.0f;   // newtons the player pushes loose rock with
	constexpr float kCharacterMaxSlope = 50.0f;   // degrees
	constexpr float kRockDensity = 2600.0f;       // kg/m^3
	constexpr int kDebrisPerSwing = 4;            // chunks thrown per pick swing
	constexpr int kDebrisMaxSize = 3;             // voxels per side of a thrown chunk
	constexpr float kDebrisSpeed = 2.5f;          // m/s out of the rock face
	constexpr int kMaxActiveFragments = 150;      // more than this: the oldest bake immediately
	constexpr int kMaxFragmentBoxes = 48;         // collision boxes per fragment before coarsening
	constexpr int kMinBakeVoxels = 8;             // smaller fragments vanish instead of baking
	constexpr float kFragmentMaxLifetime = 8.0f;  // seconds before a fragment bakes regardless
	constexpr float kFragmentMinLifetime = 0.6f;  // don't bake before this, even if asleep
	constexpr int kSupportSearchBudget = 20000;   // voxels; regions larger than this count as anchored
	constexpr float kPickImpulse = 60.0f;         // N*s applied to a loose chunk hit by the pick

	// Pre-carved openings for the basic (test) layout; generated worlds keep the entrance.
	constexpr int kEntranceZ0 = 0, kEntranceZ1 = 5;    // 1 wide x 2 tall
	constexpr int kEntranceHeight = 2;
	constexpr int kChamberZ0 = 6, kChamberZ1 = 10;     // 5 wide x 3 tall
	constexpr int kChamberHalfWidth = 2;
	constexpr int kChamberHeight = 3;

	// Outside: flat ground at y = 1, walkable within +-kOutsideExtent of the entrance.
	constexpr float kGroundY = 1.0f;
	constexpr float kOutsideExtent = 60.0f;

	// ---- Colours (sRGB 0xRRGGBB) --------------------------------------------
	constexpr uint32_t kStoneNear = 0x7d6b58;     // warm brown at the entrance
	constexpr uint32_t kStoneDeep = 0x3a3756;     // dark violet at the far wall
	constexpr float kStoneJitter = 0.15f;         // +-15% brightness per cell
	constexpr float kOreJitter = 0.06f;
	constexpr float kBedrockDarken = 0.55f;
	constexpr uint32_t kGrass = 0x5f7d3c;
	constexpr uint32_t kMountainRock = 0x6f6559;
	constexpr uint32_t kMountainHigh = 0x9a948c;
	constexpr uint32_t kTreeTrunk = 0x5a3d24;
	constexpr uint32_t kTreeLeaves = 0x2f5a2c;

	// ---- Player ---------------------------------------------------------------
	constexpr float kWalkSpeed = 4.2f;            // m/s
	constexpr float kFullBagSlowdown = 0.35f;     // speed loss with a full bag (milestone 2)
	constexpr float kJumpSpeed = 6.2f;            // m/s
	constexpr float kGravity = 20.0f;             // m/s^2
	constexpr float kMaxFallSpeed = 30.0f;
	constexpr float kStepHeight = 0.3f;           // walk up ledges this high (carved floors are uneven)
	constexpr float kBodyWidth = 0.6f;
	constexpr float kBodyHeight = 1.7f;
	constexpr float kEyeHeight = 1.55f;
	constexpr float kMouseSensitivity = 0.0022f;  // radians per raw mouse count
	constexpr float kFovDegrees = 75.0f;          // horizontal
	constexpr float kNearPlane = 0.05f;
	constexpr float kFarPlane = 600.0f;
	constexpr float kSpawnX = kCenterX + 0.5f, kSpawnZ = -8.0f;

	// ---- Mining ---------------------------------------------------------------
	constexpr float kReach = 4.5f;                // m
	constexpr float kHardnessDepthScale = 45.0f;  // effective = hardness * (1 + z / this), z in metres
	// Each swing carves a rough sphere. A voxel breaks if it is within
	//   kBiteRadius * sqrt((pickSpeed / kPickSpeed[0]) / effectiveHardness)
	// of the impact point, so hard ore stands proud and better picks take bigger bites.
	constexpr float kSwingPeriod = 0.42f;         // seconds per swing (wind-up + strike)
	constexpr float kBiteRadius = 0.55f;          // m, for the starting pick in hardness-1 rock
	constexpr float kBiteMaxScale = 2.2f;         // cap on the bite growth from upgrades
	constexpr float kBiteRoughness = 0.35f;       // jagged edges: +- this fraction per voxel

	// Upgrade levels (index 0 = starting gear).
	constexpr float kPickSpeed[4] = { 1.5f, 2.6f, 4.2f, 7.0f };
	constexpr float kLanternRange[4] = { 8.0f, 12.0f, 17.0f, 24.0f };
	constexpr int kBagCapacity[4] = { 8, 14, 22, 32 };

	// ---- View models (camera space: +X right, +Y up, +Z forward) -------------
	constexpr float kPickOffset[3] = { 0.20f, -0.17f, 0.40f };
	constexpr float kPickScale = 0.42f;
	constexpr float kLanternOffset[3] = { -0.18f, -0.12f, 0.38f };
	constexpr float kLanternScale = 0.38f;

	// ---- Lantern --------------------------------------------------------------
	constexpr uint32_t kLanternColor = 0xffc477;
	constexpr float kLanternIntensity = 1.8f;
	constexpr float kLanternFlicker = 0.07f;      // relative intensity wobble

	// ---- Lighting by position -------------------------------------------------
	constexpr float kSunFadeDistance = 6.0f;      // sun/sky light gone this far inside
	constexpr float kOutsideFogStart = 70.0f;
	constexpr float kOutsideFogEnd = 280.0f;
	constexpr float kCaveFogStartFraction = 0.35f;// cave fog starts at this fraction of lantern range
	constexpr float kCaveAmbient = 0.012f;
	constexpr uint32_t kDaySky = 0x8fb8e8;
	constexpr uint32_t kDayFog = 0xb9cde0;
	constexpr float kDayAmbient = 0.30f;

	// ---- UI -------------------------------------------------------------------
	constexpr float kMessageSeconds = 4.5f;
	constexpr float kMessageFadeSeconds = 1.0f;

	// ---- Blocks ---------------------------------------------------------------
	enum class Block : uint8_t
	{
		Air,
		Stone,
		Bedrock,
		Copper,
		Silver,
		Gold,
		TimeCrystal,
		StrangeWatch,
		Count
	};

	struct BlockInfo
	{
		const char* name;
		float hardness;
		int basePrice;     // 0 = not sellable
		uint32_t color;    // sRGB; stone uses the depth gradient instead
	};

	constexpr BlockInfo kBlocks[] =
	{
		{ "Air",           0.0f,   0, 0x000000 },
		{ "Stone",         1.0f,   0, 0x7d6b58 },
		{ "Bedrock",       0.0f,   0, 0x2a2630 },   // unbreakable
		{ "Copper",        1.4f,   6, 0xc8743a },
		{ "Silver",        2.0f,  15, 0xd9e1e8 },
		{ "Gold",          2.8f,  40, 0xffc928 },
		{ "Time crystal",  4.0f, 110, 0x6ff0ff },
		{ "Strange watch", 1.5f, 250, 0xff5fd2 },
	};

	inline const BlockInfo& Info(Block block) { return kBlocks[static_cast<int>(block)]; }
}
