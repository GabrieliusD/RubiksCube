#pragma once
// UNDERTIME tuning: every gameplay number lives here.
#include <cstdint>

namespace cfg
{
	// ---- Version ----------------------------------------------------------------
	// Bump when a change is worth telling apart in play. The HUD also shows when the game
	// module was built, so a stale build is obvious.
	constexpr const char* kGameVersion = "0.6 (loose blocks, shovel, backpack)";

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
	// Loose blocks: every voxel the pick knocks out becomes one block with its own body. Blocks
	// tumble out of the face, pile up against the rock, boulders and each other, pass through
	// the player, and never go back into the world: the shovel scoops them into the backpack.
	constexpr float kBlockGap = 0.04f;            // blocks are this fraction smaller than a voxel, so piles read as blocks
	constexpr float kBlockSpeed = 0.8f;           // m/s out of the rock face (then they drop and tumble)
	constexpr float kBlockScatter = 0.5f;         // m/s of random sideways spread
	constexpr float kBlockSpin = 4.0f;            // rad/s of random tumble
	constexpr int kMaxLooseBlocks = 3000;         // more than this lying around: the oldest crumble away
	constexpr float kBlockCrumbleTime = 0.5f;     // s to shrink away (only over the cap)
	constexpr float kBlockLostY = -20.0f;         // blocks and boulders falling below this are gone
	// Loose boulders (rock that broke off in a cave-in): stay loose until the pick breaks them up.
	constexpr int kMaxBoulders = 60;              // more than this: the oldest comes apart into blocks
	constexpr int kMinBoulderVoxels = 6;          // pieces smaller than this after a hit come apart into blocks
	constexpr float kLooseRockBonus = 1.6f;       // loose rock breaks this much more easily (bite strength)
	constexpr int kMaxFragmentBoxes = 48;         // collision boxes per boulder before coarsening
	constexpr int kSupportSearchBudget = 20000;   // voxels; regions larger than this count as anchored

	// Pre-carved openings for the basic (test) layout; generated worlds keep the entrance.
	constexpr int kEntranceZ0 = 0, kEntranceZ1 = 5;    // 1 wide x 2 tall
	constexpr int kEntranceHeight = 2;
	constexpr int kChamberZ0 = 6, kChamberZ1 = 10;     // 5 wide x 3 tall
	constexpr int kChamberHalfWidth = 2;
	constexpr int kChamberHeight = 3;

	// Outside: ground level at the cave mouth is y = 1. Tests (no valley) use flat ground,
	// walkable within +-kOutsideExtent of the entrance.
	constexpr float kGroundY = 1.0f;
	constexpr float kOutsideExtent = 60.0f;

	// ---- The valley (metres; Valley.cpp) --------------------------------------------
	// Laid out after Undertime/Docs/Images/valley_map.png: the cave mouth at the valley's
	// north edge (z = 0), the valley floor to the south (-z), mountains all around.
	constexpr float kValleyCenter[2] = { 27.0f, -150.0f };   // x, z of the valley floor's middle
	constexpr float kValleyRadius[2] = { 215.0f, 185.0f };   // half-size of the valley floor
	constexpr float kValleyFlatAtMouth = 28.0f;   // flat ground (y = kGroundY) this far around the cave mouth
	constexpr float kValleyFloorBumps = 2.5f;     // +- metres of rolling ground on the valley floor
	constexpr float kValleyFoothills = 9.0f;      // the floor rises this much towards the edge
	constexpr float kMountainHeight = 85.0f;      // typical height of the ring of mountains
	constexpr float kMountainOverCave = 34.0f;    // the mountain above the cave rises to at least this
	constexpr float kSnowLine = 72.0f;            // snow above this (with noise)
	constexpr float kTreeLine = 62.0f;            // no trees above this
	constexpr float kRockSlope = 0.6f;            // metres of drop between neighbouring 0.25 m columns that make rock
	// The river, from the waterfall in the north-east to the pass in the south-west.
	constexpr float kRiverPoints[][2] = {
		{ 184.5f, 15.0f }, { 169.5f, -27.5f }, { 134.5f, -62.5f }, { 119.5f, -107.5f }, { 94.5f, -147.5f },
		{ 54.5f, -177.5f }, { 9.5f, -217.5f }, { -35.5f, -257.5f }, { -75.5f, -297.5f }, { -100.5f, -347.5f },
		{ -110.5f, -420.0f } };
	constexpr float kRiverHalfWidth = 2.75f;
	constexpr float kRiverDepth = 0.75f;          // water depth below the water level
	constexpr float kRiverBank = 5.0f;            // banks slope down to the river over this distance
	constexpr float kLakeCenter[2] = { 74.5f, -307.5f };
	constexpr float kLakeRadius[2] = { 30.0f, 19.0f };
	constexpr float kLakeDepth = 2.0f;
	// Clearings (no trees): x, z, radius. The cave mouth, the town square, the homestead...
	constexpr float kClearings[][3] = {
		{ 24.5f, -10.0f, 32.0f },     // cave mouth
		{ -15.5f, -132.5f, 55.0f },   // town square
		{ 34.5f, -47.5f, 14.0f },     // the big tree
		{ -70.5f, -20.0f, 24.0f },    // founders' house
		{ -138.0f, -77.5f, 16.0f },   // clockmaker
		{ 159.5f, -107.5f, 28.0f },   // chapel
		{ 67.0f, -170.0f, 24.0f },    // sawmill
		{ -110.5f, -197.5f, 40.0f },  // homestead
		{ 234.5f, -27.5f, 10.0f },    // lookout
	};
	constexpr float kForestSpacing = 5.5f;        // metres between tree spots on the valley floor
	constexpr float kForestSpacingFar = 11.0f;    // ... on the mountains
	// Terrain chunks: 0.25 m blocks on the valley floor and around the cave mouth, 1 m blocks
	// on the mountains beyond (chosen per chunk; trees follow the same rule).
	constexpr float kValleyWorld[4] = { -400.0f, -544.0f, 448.0f, 320.0f };  // min x, min z, max x, max z (multiples of kTerrainChunk)
	constexpr float kTerrainChunk = 16.0f;        // metres per chunk side
	constexpr int kTerrainRegion = 4;             // chunks per side merged into one mesh and one collision body
	constexpr float kDetailValleyDistance = 1.0f; // detailed where the chunk centre's ValleyDistance is below this...
	constexpr float kDetailMouthReach = 56.0f;    // ... or it's this close to the cave mouth

	// Ground colours (sRGB).
	constexpr uint32_t kTerrainGrass = 0x5f7d3a;
	constexpr uint32_t kTerrainGrass2 = 0x6a8840;
	constexpr uint32_t kTerrainDirt = 0x7a6248;
	constexpr uint32_t kTerrainSand = 0x9c8a66;
	constexpr uint32_t kTerrainGravel = 0x6e675d;
	constexpr uint32_t kTerrainRock = 0x7a7064;
	constexpr uint32_t kTerrainRock2 = 0x6a6258;
	constexpr uint32_t kTerrainSnow = 0xe8eef4;
	constexpr uint32_t kWaterColor = 0x2f6f8a;
	constexpr float kWaterOpacity = 0.72f;

	// ---- Colours (sRGB 0xRRGGBB) --------------------------------------------
	constexpr uint32_t kStoneNear = 0x7d6b58;     // warm brown at the entrance
	constexpr uint32_t kStoneDeep = 0x3a3756;     // dark violet at the far wall
	constexpr float kStoneJitter = 0.15f;         // +-15% brightness per cell
	constexpr float kOreJitter = 0.06f;
	constexpr float kBedrockDarken = 0.55f;

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
	constexpr float kHardnessDepthScale = 60.0f;  // effective = hardness * (1 + z / this), z in metres
	// Each swing damages a rough sphere of radius kBiteRadius * sqrt(pickSpeed / kPickSpeed[0])
	// around the impact point. A voxel takes
	//   kPickPower * (pickSpeed / kPickSpeed[0]) / effectiveHardness * falloff
	// damage per hit (falloff is 1 near the centre, 0 at the edge) and breaks at 1. Soft rock
	// near the entrance pops in one hit; deeper rock and ore crack over a few hits first, then
	// a whole bite comes out at once.
	constexpr float kSwingPeriod = 0.42f;         // seconds per swing (wind-up + strike)
	constexpr float kFirstSwingStart = 0.45f;     // a fresh click starts this far into the swing, so it lands fast
	constexpr float kHitKick = 0.035f;            // rad the view jolts up on impact
	constexpr float kHitShake = 0.012f;           // m of camera shake on impact
	constexpr float kHitFeedbackTime = 0.14f;     // s for the jolt and shake to settle
	constexpr float kPickRecoil = 0.45f;          // rad the pick bounces back off the rock
	constexpr float kHollowHitScale = 0.35f;      // jolt on a swing that broke nothing (bedrock, hard ore)
	constexpr float kHitStop = 0.05f;             // s the pick freezes in the rock on impact
	constexpr float kBiteRadius = 0.55f;          // m, for the starting pick in hardness-1 rock
	constexpr float kBiteMaxScale = 2.2f;         // cap on the bite growth from upgrades
	constexpr float kBiteRoughness = 0.35f;       // jagged edges: +- this fraction per voxel
	constexpr float kPickPower = 1.25f;           // damage per hit at the centre for the starting pick in hardness-1 rock
	constexpr float kBiteFalloff = 2.2f;          // full damage inside (1 - 1/this) of the radius
	constexpr float kCrackShade[2] = { 0.74f, 0.52f };   // brightness of lightly / badly cracked voxels
	constexpr float kCrackHitScale = 0.7f;        // jolt on a hit that only cracked the rock

	// ---- Shovel and backpack ------------------------------------------------------
	// 1 = pick, 2 = shovel (or the mouse wheel). Holding left mouse with the shovel scoops the
	// loose blocks around where you aim into the backpack.
	constexpr float kShovelReach = 3.5f;          // m
	constexpr float kShovelRadius = 0.45f;        // m around the aim point that one scoop takes from
	constexpr int kShovelScoop = 12;              // blocks per scoop at most
	constexpr float kShovelPeriod = 0.38f;        // s per scoop while the button is held
	constexpr float kScoopFlyTime = 0.18f;        // s for a scooped block to fly into the backpack
	constexpr int kBackpackCapacity = 400;        // blocks
	constexpr float kPickupToastTime = 1.6f;      // s the "+3 Copper" line stays up

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
	constexpr float kHideValleyDepth = 40.0f;     // deeper than this (m into the mountain), the valley isn't drawn
	constexpr float kOutsideFogStart = 110.0f;
	constexpr float kOutsideFogEnd = 520.0f;
	constexpr float kCaveFogStartFraction = 0.35f;// cave fog starts at this fraction of lantern range
	constexpr float kCaveAmbient = 0.012f;
	constexpr uint32_t kDaySky = 0x8fb8e8;        // flat background when the sky is off
	constexpr uint32_t kDayFog = 0xb9cde0;        // also the sky's horizon, so distant land meets it
	constexpr uint32_t kSkyZenith = 0x3f74cc;     // straight up
	constexpr uint32_t kSkyGround = 0x6f7458;     // below the horizon
	constexpr float kSkyGradientPower = 0.45f;
	constexpr float kSunDiskSize = 0.7f;          // degrees
	constexpr float kSunDiskIntensity = 25.0f;
	constexpr float kShadowNear = 40.0f;          // metres of sharp sun shadows around the camera
	constexpr float kShadowFar = 240.0f;          // softer ones out to here, fading away
	constexpr float kBloomIntensity = 0.06f;      // glow around bright things (windows, fire, crystals)
	constexpr float kBloomThreshold = 1.2f;
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
