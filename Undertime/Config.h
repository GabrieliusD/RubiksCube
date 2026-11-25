#pragma once
// UNDERTIME tuning: every gameplay number lives here.
#include <cstdint>

namespace cfg
{
	// ---- Version ----------------------------------------------------------------
	// Bump when a change is worth telling apart in play. The HUD also shows when the game
	// module was built, so a stale build is obvious.
	constexpr const char* kGameVersion = "0.7 (money, the barrow, your claim, food, wood, farming, machines)";

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
	// tumble out of the face, pile up against the rock, boulders and each other (passing through
	// the player unless the solid flags below are on), settle into piles (LooseBlocks.h) and never
	// go back into the world: the shovel scoops them into the backpack.
	constexpr float kBlockGap = 0.04f;            // blocks are this fraction smaller than a voxel, so piles read as blocks
	constexpr float kBlockSpeed = 0.8f;           // m/s out of the rock face (then they drop and tumble)
	constexpr float kBlockScatter = 0.5f;         // m/s of random sideways spread
	constexpr float kBlockSpin = 4.0f;            // rad/s of random tumble
	constexpr int kMaxLooseBlocks = 3000;         // with kLooseCapDefault: more than this lying around and the oldest crumble away
	// Test flags (Design.md §3, "Mined stone"): the defaults here, switched while playing in
	// the editor's Game panel ("Loose block tests").
	constexpr bool kLooseCapDefault = false;      // the old cap: past kMaxLooseBlocks the oldest crumble away (resources lost)
	constexpr bool kLoosePilesDefault = true;     // blocks at rest settle into piles: one mesh and one static body per cell
	constexpr bool kSolidBlocksDefault = false;   // falling and tumbling blocks are solid to the player (else pass through)
	constexpr bool kSolidPilesDefault = false;    // settled piles are solid to the player (you collect them to get past)
	// Piles: a block that has stayed put this long joins the pile of its cell. Each cell is one
	// mesh and one static collision mesh, rebuilt when blocks join or leave it.
	constexpr float kPileCell = 1.0f;             // m per side of a pile cell (at most ~64 blocks: a rebuild stays around a millisecond)
	constexpr float kPileSettleTime = 0.5f;       // s a block must stay put before it joins its pile
	constexpr float kBlockStillDrift = 0.03f;     // m: a block that drifts less than this (jitter in a heap) counts as staying put...
	constexpr float kBlockStillTurn = 4.0f;       // ... and turns less than this (degrees)
	constexpr float kBlockSleepAfter = 0.2f;      // s staying put before its body is put to sleep (heaps jitter otherwise)
	constexpr float kPileBatchTime = 0.3f;        // s a pile collects settling blocks before it is rebuilt with all of them
	constexpr float kPileBuildBudgetMs = 2.0f;    // ms per frame for rebuilding piles that blocks joined (at least one per frame)
	constexpr int kMaxLiveBlocks = 2500;          // more blocks with bodies than this: the stillest join their piles early (none are lost)
	constexpr float kPileWakeMargin = 0.3f;       // m around a rock edit where settled blocks turn live again (plus what rests on them)
	constexpr float kBlockCrumbleTime = 0.5f;     // s to shrink away (only over the cap)
	constexpr float kBlockLostY = -20.0f;         // boulders falling below this are gone; loose blocks are put back at the mine mouth
	// Loose boulders (rock that broke off in a cave-in): stay loose until the pick breaks them up.
	constexpr int kMaxBoulders = 60;              // more than this: the oldest comes apart into blocks
	constexpr int kMinBoulderVoxels = 6;          // pieces smaller than this after a hit come apart into blocks
	constexpr float kLooseRockBonus = 1.6f;       // loose rock breaks this much more easily (bite strength)
	constexpr int kMaxFragmentBoxes = 48;         // collision boxes per boulder before coarsening
	constexpr int kSupportSearchBudget = 20000;   // voxels; regions larger than this count as anchored

	// ---- The mountain face (CaveGenerator: the front of the grid, z = 0) ----------------
	// The grid's front is shaped into a craggy rock face that matches the terrain around it:
	// recessed in noisy ledges (grass where a ledge faces up), with an unbreakable skin of
	// mountain rock, so the outside can't be dug into. The way in is a rough portal.
	constexpr float kFaceRelief = 2.25f;          // m the face recedes at most
	constexpr float kFaceNoiseScale = 0.16f;      // cycles per metre of the ledges (squashed vertically)
	constexpr float kFaceEdgeTaper = 3.0f;        // m: no recess at the grid's sides and top, where the terrain meets it
	constexpr float kFaceSkin = 1.0f;             // m of unbreakable rock behind the face
	constexpr float kPortalHalfWidth = 1.6f;      // m, the entrance passage
	constexpr float kPortalHeight = 3.1f;         // m above the floor in the middle
	constexpr float kPortalArch = 0.7f;           // m the ceiling drops toward the walls
	constexpr float kPortalRoughness = 0.3f;      // m of wobble on walls and ceiling
	constexpr float kPortalMouthRoughness = 1.6f; // ... this much more at the mouth (broken rock)
	constexpr float kPortalMouthDepth = 1.5f;     // m in from the face where it's that rough
	constexpr float kPortalClear = 1.0f;          // m around the portal where the face recess fades out
	// Terrain around the face (Valley.cpp): buttresses over the grid's side edges, rubble at its foot.
	constexpr float kButtressInset = 2.0f;        // m the buttresses reach over the face's side edges
	constexpr float kButtressFront = 1.5f;        // m they stand out in front of the face
	constexpr float kButtressFall = 2.2f;         // m they drop per metre away from the face
	constexpr float kTalusReach = 3.0f;           // m of rubble slope in front of the face
	constexpr float kTalusHeight = 2.0f;          // m high against the face
	constexpr float kMouthPathHalfWidth = 3.0f;   // m kept clear in front of the portal

	// ---- Natural shafts (CaveGenerator, Valley, ValleyScenery) ------------------------
	// Chimneys from the mountain top down into a cavern, letting daylight in (Design.md,
	// "The cave"). They're part of the mountain, so fixed: where they open at the grid's top
	// (x, z) and the zone (0-based) whose cavern they drop into. Every run's cave makes room
	// for them. They lean north along the noon sunlight, so around midday a patch of sun
	// reaches the cavern floor. A log grate over each opening keeps you from falling in.
	struct Shaft { float x, z; int zone; };
	constexpr Shaft kShafts[] = { { 14.0f, 9.0f, 0 }, { 34.0f, 41.0f, 1 }, { 13.0f, 73.0f, 2 } };
	constexpr float kShaftRadius = 1.7f;          // m, the chimney (it wobbles a little)
	constexpr float kShaftLean = 0.78f;           // m north per m down (the noon sun at kSunNoonElevation)
	constexpr float kShaftCavernFloor = 6.0f;     // m: the cavern under it sits on an upper level
	constexpr float kShaftCavernRadius = 4.5f;
	constexpr float kShaftCavernHeight = 4.0f;
	constexpr float kShaftZoneMargin = 6.0f;      // m of the zone kept beyond the shaft and its cavern (room for the exit cavern)
	constexpr float kShaftHoleMargin = 0.75f;     // m the hole in the ground reaches past the shaft (the grate rests there)

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
	// Strata: steep ground above the valley floor breaks into layers, each a rock riser and a
	// near-flat ledge (grass where it's flat enough), like the cave's face.
	constexpr float kStrataHeight = 2.5f;         // m per layer (+-30% with noise)
	constexpr float kStrataSharpness = 4.0f;      // higher: flatter ledges, steeper risers
	constexpr float kStrataWarp = 1.5f;           // m the layers wander, so ledges don't follow the contours
	constexpr float kStrataFrom = 0.15f;          // by how high the ground is above the floor (0..1 over 12 m)
	constexpr float kStrataFull = 0.6f;
	// Rock outcrops on steep ground (Valley::Outcrops, placeholder .vox crags).
	constexpr float kOutcropSpacing = 11.0f;      // small ones on the valley floor
	constexpr float kOutcropSpacingFar = 38.0f;   // crags on the mountains (four times the size, Crag*.vox)
	constexpr float kOutcropSlope = 2.0f;         // m of rise over 4 m that counts as steep
	constexpr float kOutcropChance = 0.65f;
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
		{ 96.8f, -121.1f, 18.0f },    // clockmaker, by the river
		{ 8.0f, -60.0f, 8.0f },       // along the trail from the mine
		{ 159.5f, -107.5f, 28.0f },   // chapel
		{ 67.0f, -170.0f, 24.0f },    // sawmill
		{ -110.5f, -197.5f, 40.0f },  // homestead
		{ 234.5f, -27.5f, 10.0f },    // lookout
		{ 100.0f, -40.0f, 19.0f },    // your claim (kClaimCenter)
	};
	// Trails (start x, z, end x, z): the mine to the camp, the camp to the clockmaker.
	constexpr float kPathSegments[][4] = {
		{ 24.5f, -4.0f, 20.0f, -40.0f }, { 20.0f, -40.0f, 6.0f, -80.0f }, { 6.0f, -80.0f, -8.0f, -112.0f }, { -8.0f, -112.0f, -15.5f, -124.0f },
		{ -10.0f, -136.0f, 30.0f, -130.0f }, { 30.0f, -130.0f, 62.0f, -124.0f }, { 62.0f, -124.0f, 88.0f, -121.0f },
		// Settlement lanes: the homes, the founders' house, the sawmill, over the footbridge to the chapel.
		{ -20.0f, -131.0f, -33.0f, -131.0f }, { -33.0f, -131.0f, -44.0f, -116.0f }, { -33.0f, -131.0f, -44.0f, -150.0f },
		{ -15.5f, -146.0f, -30.0f, -164.0f },
		{ -15.5f, -112.0f, -40.0f, -60.0f }, { -40.0f, -60.0f, -70.5f, -34.0f },
		{ -5.0f, -145.0f, 30.0f, -160.0f }, { 30.0f, -160.0f, 62.0f, -167.0f },
		{ 96.8f, -121.0f, 112.3f, -108.6f }, { 121.7f, -114.4f, 150.0f, -104.0f },
		// A track from the mine mouth to your claim.
		{ 20.0f, -40.0f, 60.0f, -36.0f }, { 60.0f, -36.0f, 86.0f, -38.0f },
	};
	// Footbridges NPCs walk over (start x, z, end x, z: the trail ends either side of the water).
	constexpr float kBridges[][4] = { { 112.3f, -108.6f, 121.7f, -114.4f } };
	constexpr float kPathHalfWidth = 0.9f;
	constexpr float kPadBlend = 4.0f;             // metres over which a building's flat pad blends into the land
	constexpr float kPadMargin = 0.5f;            // pads reach this far past a building's footprint
	constexpr float kForestSpacing = 5.5f;        // metres between tree spots on the valley floor
	constexpr float kForestSpacingFar = 11.0f;    // ... on the mountains
	// Terrain chunks. Everything is 0.25 m blocks; far away it's drawn coarser (LOD, by region:
	// kTerrainRegion x kTerrainRegion chunks), rebuilt around the camera on worker threads.
	// Chunks the river runs through stay 0.25 m at any distance (the water steps must meet).
	constexpr float kValleyWorld[4] = { -400.0f, -544.0f, 448.0f, 320.0f };  // min x, min z, max x, max z (multiples of kTerrainChunk)
	constexpr float kTerrainChunk = 16.0f;        // metres per chunk side
	constexpr int kTerrainRegion = 4;             // chunks per side merged into one mesh and one collision body
	constexpr float kDetailValleyDistance = 1.0f; // the valley floor (denser trees, small outcrops) where the chunk centre's ValleyDistance is below this...
	constexpr float kDetailMouthReach = 56.0f;    // ... or it's this close to the cave mouth
	// m from the camera: 0.25 m blocks with ground cover up to [0], 0.25 m without up to [1],
	// 0.5 m blocks up to [2], 1 m beyond (UNDERTIME_DEV_LOD="a b c" overrides them).
	constexpr float kLodDistance[3] = { 110.0f, 110.0f, 260.0f };
	constexpr bool kLodFade = true;               // crossfade (screen-door dither) between levels instead of swapping
	constexpr float kLodFadeSeconds = 0.6f;
	constexpr bool kLodCompact = true;            // terrain meshes in 28-byte vertices instead of 48 (MeshFormat::Compact)
	constexpr float kLodHysteresis = 12.0f;       // m past a distance before a region turns coarser again (no flapping)
	constexpr int kLodBuildsInFlight = 6;         // region rebuilds queued at once (nearest first)
	constexpr float kModelLodDistance = 150.0f;   // trees and crags further than this use their 0.5 m versions
	constexpr float kCollisionRadius = 40.0f;     // m: terrain collision (0.25 m) exists around the player out to here...
	constexpr float kCollisionKeep = 56.0f;       // ... and is dropped past this
	constexpr float kCollisionNow = 12.0f;        // collision this close that's missing is built at once (teleports, start)
	constexpr float kCollisionBudgetMs = 2.0f;    // ms a frame for making collision bodies

	// Ground colours (sRGB).
	constexpr uint32_t kTerrainGrass = 0x5f7d3a;
	constexpr uint32_t kTerrainGrass2 = 0x6a8840;
	constexpr uint32_t kTerrainDirt = 0x7a6248;
	constexpr uint32_t kTerrainSand = 0x9c8a66;
	constexpr uint32_t kTerrainGravel = 0x6e675d;
	constexpr uint32_t kTerrainRock = 0x7a7064;
	constexpr uint32_t kTerrainRock2 = 0x6a6258;
	constexpr uint32_t kTerrainSnow = 0xe8eef4;
	constexpr uint32_t kTerrainPath = 0x8a7656;
	constexpr uint32_t kTerrainMoss = 0x46602e;   // the forest floor
	constexpr float kMossPatch = 0.15f;           // how wooded (Valley::ForestPatch) before the floor turns mossy

	// Ground cover on the detailed valley floor (ValleyScenery): tall grass, flowers, bushes
	// and pebbles (Models/Placeholders), one try per spot of a kCoverSpacing grid.
	constexpr float kCoverSpacing = 1.25f;        // m between spots (each is jittered)
	constexpr float kCoverGrass = 0.30f;          // chance of a tuft of tall grass on grass
	constexpr float kCoverFlowers = 0.20f;        // chance of flowers inside a flower patch
	constexpr float kCoverFlowerPatch = 0.62f;    // noise (0..1) above which flowers grow: patches across meadows
	constexpr float kCoverBush = 0.10f;           // chance of a bush on the forest floor and its edge
	constexpr float kCoverPebbles = 0.025f;       // chance of pebbles anywhere grassy
	constexpr float kCoverClear = 1.2f;           // m kept clear around building pads

	// Wind (foliage sway; Content/Shaders/Foliage.hlsl). It blows the way the clouds drift.
	constexpr float kWindStrength = 1.0f;         // average (0 calm .. 1 breezy)
	constexpr float kWindVariation = 0.45f;       // how much it rises and falls over a minute or so
	constexpr float kWindGustSpeed = 0.9f;        // how fast gusts roll across (radians of gust phase per second)
	constexpr uint32_t kWaterColor = 0x2f6f8a;
	constexpr float kWaterOpacity = 0.72f;
	// Flowing water (Content/Shaders/Water.hlsl, the flow map in ValleyScenery).
	constexpr uint32_t kWaterDeep = 0x1f4f66;     // deep water
	constexpr uint32_t kWaterShallow = 0x4f8f88;  // over the shallows and the banks
	constexpr float kRiverSpeed = 0.7f;           // m/s on the flat
	constexpr float kRiverSlopeSpeed = 10.0f;     // m/s more per metre of drop per metre (rapids)
	constexpr float kRiverMaxSpeed = 2.5f;
	constexpr float kLakeSpeed = 0.05f;
	constexpr float kFallDrop = 0.3f;             // m of drop within a metre that counts as a fall
	constexpr float kFallFoamLength = 5.0f;       // m downstream over which a fall's foam fades
	constexpr int kFallSprayMax = 12;             // spray emitters at the falls nearest the town

	// ---- Colours (sRGB 0xRRGGBB) --------------------------------------------
	constexpr uint32_t kStoneNear = 0x7d6b58;     // warm brown at the entrance
	constexpr uint32_t kStoneDeep = 0x3a3756;     // dark violet at the far wall
	// Ore in the rock (Content/Shaders/CaveRock.hlsl): an ore block is rock with metal in it,
	// the metal in pixel clusters that run on as seams across blocks.
	constexpr float kOreRichness = 0.5f;          // share of an ore block's face that's metal (about)
	constexpr float kGoldRichness = 0.4f;         // gold: sparse flecks...
	constexpr uint32_t kGoldQuartz = 0xd6cdbb;    // ... in pale quartz
	constexpr uint32_t kCopperVerdigris = 0x4f9a7c;   // green patches on copper
	constexpr float kSilverTarnish = 0.5f;        // how dark tarnished silver gets
	// Relief in the rock (CaveRock.hlsl): shading only (no shape), on the same pixel grid,
	// so the lantern picks out chiselled facets and cracks look cut in. 0 turns each off.
	constexpr float kRockRelief = 0.35f;          // how far the chiselled facets tilt
	constexpr float kCrackGroove = 0.7f;          // how far the pixels beside a crack tilt into it
	constexpr float kReliefFadeDistance = 14.0f;  // m: the relief fades out by here (it would shimmer)
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
	constexpr float kCrackShade[2] = { 0.9f, 0.78f };     // brightness of lightly / badly cracked voxels (the cave rock shader draws the crack lines)
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

	// ---- Money, selling and the yard (Docs/Hauling.md, H1) -------------------------
	// Money is counted in cents (shown as dollars). Ore sells for BlockInfo::basePrice dollars
	// a block times the era's factor; stone for kStoneCents by era. Index = era - 1 (Camp ..
	// Gone).
	constexpr int kStartingCents = 500;
	constexpr float kOreEraFactor[6] = { 1.0f, 1.1f, 1.3f, 0.9f, 0.6f, 0.0f };
	constexpr int kStoneCents[6] = { 5, 6, 3, 2, 1, 0 };   // the town builds its foundations in stone, later in brick
	constexpr float kShopBuyBack = 0.5f;
	constexpr int kPackCapacity[3] = { 400, 600, 900 };   // blocks, by pack level (replaces kBackpackCapacity)          // what shops pay for goods they sell, of the price
	// The trading post's stone yard: stone that comes to rest inside it is paid for once and
	// belongs to the town from then on (the shovel leaves it). A box on the ground (x, z
	// centre, half sizes), north of the trading post beside the mine trail.
	constexpr float kYardCenter[2] = { -15.0f, -101.0f };
	constexpr float kYardHalf[2] = { 3.5f, 3.0f };
	constexpr float kYardPayDelay = 0.6f;
	constexpr float kTownStonePerDay = 120.0f;    // the town builds its projects (econ::Projects) with this much yard stone an outside day         // s after blocks start settling before the "+$" toast sums them up

	// ---- The wheelbarrow (H2) -----------------------------------------------------
	constexpr int kBarrowCapacity = 160;          // blocks
	constexpr float kBarrowReach = 2.6f;          // m: grab it, and fill it with the shovel, within this
	constexpr float kBarrowHold = 1.45f;          // m ahead of you while you push it
	constexpr float kBarrowSlowFull = 0.45f;      // a full barrow slows your walk to this share
	constexpr float kBarrowTipTime = 0.6f;        // s to tip it out
	constexpr float kBarrowRescueY = -30.0f;      // below this it's put back at the mine mouth

	// ---- Your claim and building on it (H4) ---------------------------------------
	// A meadow on the foothills east of the mine, away from town; flattened (a ground pad) and
	// cleared of trees. Pieces snap to its grid of 1 m cells and levels (Building.h).
	constexpr float kClaimCenter[2] = { 100.0f, -40.0f };
	constexpr int kClaimCells = 24;               // cells across (it's square)
	constexpr int kClaimLevels = 6;
	constexpr float kHammerReach = 6.0f;          // m
	constexpr float kHammerPeriod = 0.42f;        // s per swing while the button is held
	constexpr float kTakeDownTime = 0.8f;         // s holding X on a piece to take it down
	constexpr float kSourceReach = 4.0f;          // m: building takes from a barrow parked this close
	constexpr int kSleepUntilHour = 6;            // a night in bed (from 20:00) ends at this hour

	// ---- Things you place outside the claim (H4b): props, ladders, chutes, torches ----
	constexpr float kPlaceReach = 4.5f;           // m
	constexpr float kClimbSpeed = 1.6f;           // m/s up a ladder
	constexpr float kLadderHeight = 3.5f;         // m (shorter if rock is in the way)
	constexpr float kPropMaxHeight = 4.0f;        // a prop reaches a roof at most this high
	constexpr double kTorchHours = 2.0;           // a torch burns this long, in its own time
	constexpr uint32_t kTorchColor = 0xffa860;
	constexpr float kTorchIntensity = 2.4f;
	constexpr float kTorchRange = 8.0f;

	// ---- Machines (H8-H11) -------------------------------------------------------------
	// Every machine works in its own time: the rate of where it stands (OwnRate), so one
	// shallower than you runs faster than you, and the surface runs fastest. Fuel burns and
	// wear builds in that time too; a full hopper stops a drill. Rates are per own hour.
	constexpr float kDrillMetres[3] = { 0.8f, 1.6f, 5.0f };   // hand borer, steam, electric
	constexpr int kDrillWide = 4, kDrillTall = 6;             // the tunnel, in voxels (1 m x 1.5 m)
	constexpr int kHopperCapacity = 600;                       // blocks
	constexpr float kHandBorerReach = 3.0f;                    // the hand borer works while you're this close
	constexpr float kCoalPerHour = 1.0f;                       // steam drills and engines
	constexpr float kFuelTank = 24.0f;                         // coal a machine holds
	constexpr double kWearHours = 120.0;                       // own hours of work until it breaks
	constexpr int kElectricFromEra = 4;                        // the power line (Industry)
	constexpr float kBeltRate = 2.0f;                          // blocks per own second along a belt
	constexpr int kBeltHolds = 30;                             // blocks on one 1 m belt
	constexpr float kEngineReach = 30.0f;                      // m: belts an engine drives
	constexpr int kBeltSpawnPerFrame = 6;                      // blocks a belt's end drops each frame at most
	constexpr float kMillWheel[2] = { 103.95f, -125.6f };      // the water wheel; the stamp mill goes within kMillReach
	constexpr float kMillReach = 16.0f;
	constexpr float kMillStonePerHour = 600.0f;
	constexpr float kMillCopper = 0.04f, kMillSilver = 0.015f;  // ore washed out per stone (silver from the Settlement on)
	constexpr int kMillBin = 2000;
	constexpr int kCartCapacity = 400;
	constexpr float kCartPushSpeed = 2.4f;                     // m/s along the rails
	constexpr float kCartRollAccel = 2.5f;                     // m/s^2 per unit of slope

	// ---- Hunger and food (H5) ----------------------------------------------------
	// The meter drains in your own time (real time), the same at any depth: it limits how long
	// you stay down, not how much outside time passes. Empty, you're weaker until you eat.
	constexpr float kHungerMinutes = 20.0f;       // a full meter lasts this long
	constexpr float kHungerWarn = 0.25f;          // "You're getting hungry."
	constexpr float kHungryStrength = 0.55f;      // pick, hammer and axe at this share when it's empty
	constexpr float kHungryBarrow = 0.8f;         // ... and pushing the barrow
	constexpr float kSleepHunger = 0.25f;         // a night's sleep empties this much

	// ---- Wood (H6) -----------------------------------------------------------------
	// The axe (from the smith, key 4) fells valley trees; a stump grows back from the day it
	// was felled (outside time), so long dives regrow the forest.
	constexpr float kAxeReach = 3.2f;             // m
	constexpr float kAxePeriod = 0.55f;           // s per chop while the button is held
	constexpr int kAxeChops = 5;                  // chops to fell a tree (more when you're hungry)
	constexpr int kLogsPerTree[4] = { 1, 2, 4, 3 };   // small, medium, large conifer, broadleaf
	constexpr double kStumpDays = 40.0;           // a stump before a sapling shows
	constexpr double kRegrowDays = 3.0 * 365.0;   // felled to full grown
	// Trees that can't be felled: the big tree (a story place), x, z, radius.
	constexpr float kUncuttable[][3] = { { 34.5f, -47.5f, 7.0f } };

	// ---- Farming (H7) ---------------------------------------------------------------
	// Crops grow in outside days from the day they were planted: ripe after `ripe` days, good
	// for `lasts` more, then gone to seed (the harvest is spoiled). Apple trees fruit every
	// autumn from their third year.
	struct Crop { const char* seed; const char* produce; double ripe; double lasts; int yield; };
	constexpr Crop kCrops[] = {
		{ "potato_seed", "potato", 3.0, 4.0, 4 },
		{ "wheat_seed", "wheat", 10.0, 6.0, 6 },
		{ "pumpkin_seed", "pumpkin", 30.0, 15.0, 2 },
		{ "apple_sapling", "apple", 3.0 * 365.0, 60.0, 12 },   // then every year (autumn, the yield again)
	};

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
	// The cave grid is the renderer's interior (per pixel): daylight fades out over
	// kSunFadeDistance, starting kInteriorFadeStart in from the face, so the outside stays
	// lit when you look out and the tunnel stays dark when you look in.
	constexpr float kInteriorFadeStart = 1.5f;    // m in from the face
	constexpr float kSunFadeDistance = 6.0f;      // sun/sky light gone over this many metres
	constexpr float kHideValleyDepth = 40.0f;     // deeper than this (m into the mountain), the valley isn't drawn
	// Daylight in the cave (Skylight.h): it spreads in from the face and the shafts, losing
	// kSkylightStep (of 255) per 0.5 m sideways or up and kSkylightDownStep per 0.5 m down.
	constexpr int kSkylightStep = 9;              // ~14 m from the mouth before it's gone
	constexpr int kSkylightDownStep = 2;          // light falls down a shaft easily
	constexpr float kSkylightGamma = 2.0f;        // how fast it dims (levels are squared for the renderer)
	constexpr float kSkylightStrength = 1.0f;     // x the outside ambient, where the level is 1
	constexpr float kSkylightSunReach = 3.0f;     // the sun reaches in where the level x this >= 1 (its shadows decide where)
	// Clouds (their shadows drift over the valley; the sky shows them).
	constexpr float kCloudCoverage = 0.45f;
	constexpr float kCloudScale = 130.0f;         // m across a typical cloud (several shadows in view at once)
	constexpr float kCloudSoftness = 0.05f;       // how soft their edges are (low: a shadow line you see sweep by)
	constexpr float kCloudDarkness = 0.8f;        // how much sun a cloud takes
	constexpr float kCloudHeight = 320.0f;
	constexpr float kCloudWind[2] = { 4.0f, 1.5f };   // m/s (x, z) on the surface; faster as outside time speeds up
	// Clouds blur into an even veil once one passes overhead faster than kCloudBlurSlow
	// seconds (fully by kCloudBlurFast), so fast time underground doesn't make them flicker.
	constexpr float kCloudBlurSlow = 3.0f;
	constexpr float kCloudBlurFast = 0.5f;
	// Screen-space ambient occlusion (creases, the feet of buildings, under eaves).
	constexpr float kSsaoRadius = 0.8f;
	constexpr float kSsaoStrength = 0.75f;
	constexpr float kSsaoBias = 0.04f;
	// Edge highlights: block edges catch the light (a thin lighter line), fading with distance.
	constexpr float kEdgeStrength = 0.55f;
	constexpr float kEdgeDistance = 45.0f;        // m where they've faded out
	constexpr float kEdgeSharpness = 0.004f;
	// Light shafts: sunlight scattered by the air (beams through trees, past rims, a column of
	// light down each natural shaft). Density is scattering per metre.
	constexpr float kShaftDensity = 0.0012f;      // valley air
	constexpr float kShaftCaveDensity = 0.035f;   // dusty cave air
	constexpr float kShaftAnisotropy = 0.7f;      // brighter looking towards the sun
	constexpr float kShaftIntensity = 1.0f;
	constexpr float kShaftDistance = 160.0f;      // m marched along each view ray
	constexpr int kShaftSteps = 32;
	constexpr float kOutsideFogStart = 110.0f;
	constexpr float kOutsideFogEnd = 520.0f;
	// Valley haze (RenderSettings::heightFog): thickest on the valley floor, thinning upwards,
	// so mist lies low and the peaks stand clear; bluer like the sky, warmer towards the sun.
	constexpr float kHazeStart = 25.0f;           // m of clear air around you
	constexpr float kHazeDensity = 0.0032f;       // per metre on the valley floor
	constexpr float kHazeFalloff = 0.022f;        // per metre up (halves every ~30 m)
	constexpr float kHazeBlue = 0.3f;             // how much of the sky's zenith colour it takes (aerial perspective)
	constexpr float kHazeSunWarmth = 0.45f;       // how much of the sunlight it takes looking towards the sun
	// Dawn mist: thicker and lower around sunrise, a little at dusk.
	constexpr float kMistDawnHour = 6.5f;
	constexpr float kMistDawnHours = 2.5f;        // its half width
	constexpr float kMistDawn = 3.0f;             // extra density at its peak (x kHazeDensity)
	constexpr float kMistDusk = 0.8f;
	constexpr float kMistDuskHour = 20.0f;
	constexpr float kMistFalloff = 0.07f;         // ... and how low it lies at its peak
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
	constexpr float kDayAmbient = 0.20f;

	// ---- The outside clock (Clock.h; Design.md "Day and night") -----------------------
	// Outside time runs at kSurfaceRate on the surface, faster in each cave zone (outside
	// seconds per real second). Through the entrance passage it rises from the surface rate
	// to zone 1's.
	constexpr float kDayMinutes = 15.0f;                       // real minutes per outside day, on the surface
	constexpr float kSurfaceRate = 86400.0f / (kDayMinutes * 60.0f);
	constexpr float kZoneRate[5] = {
		8.0f * 60.0f,                // zone 1: 1 minute = 8 hours
		24.0f * 60.0f,               // zone 2: 1 minute = 1 day
		7.0f * 24.0f * 60.0f,        // zone 3: 1 minute = 1 week
		30.0f * 24.0f * 60.0f,       // zone 4: 1 minute = 1 month
		365.0f * 24.0f * 60.0f,      // zone 5: 1 minute = 1 year
	};
	constexpr float kArrivalHour = 9.0f;          // you arrive in the morning...
	constexpr int kArrivalDayOfYear = 140;        // ... in late spring (day 0 = 1 January)
	constexpr int kStartYear = 1871;              // the year on the watch when you arrive
	// Eras by years since you arrived (Design.md section 4): Settlement from 2, Boomtown 10,
	// Industry 30, Abandoned 60, Gone 100.
	constexpr float kEraStartYears[5] = { 2.0f, 10.0f, 30.0f, 60.0f, 100.0f };

	// The colour grade per era (RenderSettings::colorGrade): the valley looks like a photo of
	// its time. Inside the cave it eases to kCaveGrade, the same in every era, so coming out
	// shows how the era has changed.
	struct Grade
	{
		float tint[3];       // white balance (multiplies the scene's light)
		float saturation;
		float contrast;
		float lift[3];       // raises the shadows (a faded print)
		float gain[3];       // scales the highlights
		float vignette;
	};
	constexpr Grade kEraGrades[6] = {
		{ { 1.08f, 1.00f, 0.84f }, 0.78f, 1.06f, { 0.035f, 0.022f, 0.000f }, { 1.00f, 0.97f, 0.90f }, 0.30f },   // Camp: warm sepia
		{ { 1.05f, 1.00f, 0.92f }, 0.92f, 1.05f, { 0.015f, 0.008f, 0.000f }, { 1.00f, 0.99f, 0.96f }, 0.24f },   // Settlement: warm
		{ { 1.02f, 1.00f, 0.97f }, 1.15f, 1.10f, { 0.000f, 0.000f, 0.000f }, { 1.00f, 1.00f, 0.98f }, 0.20f },   // Boomtown: rich
		{ { 0.92f, 0.98f, 1.08f }, 0.78f, 1.12f, { 0.000f, 0.006f, 0.022f }, { 0.97f, 0.99f, 1.02f }, 0.26f },   // Industry: cold
		{ { 1.00f, 0.98f, 0.93f }, 0.55f, 0.88f, { 0.060f, 0.055f, 0.050f }, { 0.95f, 0.95f, 0.93f }, 0.32f },   // Abandoned: faded
		{ { 0.98f, 1.02f, 0.97f }, 1.10f, 1.02f, { 0.000f, 0.004f, 0.000f }, { 1.00f, 1.00f, 1.00f }, 0.16f },   // Gone: green, natural
	};
	constexpr Grade kCaveGrade = { { 1.0f, 1.0f, 1.0f }, 1.0f, 1.04f, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 0.30f };
	constexpr float kGradeCaveDepth = 8.0f;        // m in from the face where the cave's grade has taken over
	constexpr float kGradeSeconds = 1.5f;          // how quickly the grade follows (a change of era, going in)

	// Eye adaptation (RenderSettings::eyeAdaptation): the exposure follows how bright the view
	// is, never below 1 (the valley by day looks as tuned), up to kEyeMaxExposure in the dark.
	constexpr float kEyeKey = 0.2f;                // the average brightness the view is shown at
	constexpr float kEyeMaxExposure = 2.2f;        // how far the eyes open in the cave
	constexpr float kEyeMaxExposureNight = 1.5f;   // ... outside at night (so night stays night)
	constexpr float kEyeSpeedBrighter = 1.6f;      // per second: dazzled coming out, for about a second
	constexpr float kEyeSpeedDarker = 0.35f;       // per second: the cave slowly opens up
	constexpr float kEraWarningSeconds = 20.0f;   // the watch warns when the next era is this close (real seconds at the current rate)
	// Shadows follow the sun in steps (each move turns the shadow maps' texel grid, which makes
	// edges shimmer if it happens every frame). Smaller steps: more often, smaller jumps.
	constexpr float kShadowStepDegrees = 0.25f;
	// Fade between steps: the old shadows blend into the new over this many seconds (an extra
	// set of shadow maps). 0 = off: each step jumps. J toggles it while playing (debug).
	constexpr float kShadowFadeSeconds = 0.5f;
	constexpr float kSkipHours = 2.0f;            // debug: T skips ahead this far
	// The showcase cave (CaveWorld::showcase, Scenes/TestCave.scene, UNDERTIME_DEV_SHOWCASE=1):
	// a fixed mountain with a hall just inside the entrance holding everything to check by eye.
	constexpr uint32_t kShowcaseSeed = 12345;

	// ---- Weather (WeatherAt in DayNight.h) --------------------------------------
	constexpr double kWeatherDays = 1.3;          // days between independent weather values (it changes over about this long)
	constexpr float kOvercastStart = 0.45f;       // weather noise (0..1) where clouds start to gather...
	constexpr float kOvercastFull = 0.8f;         // ... and where it's fully grey
	constexpr float kRainStart = 0.45f;           // second noise where rain starts (when overcast)...
	constexpr float kRainFull = 0.75f;            // ... and pours
	constexpr double kDryDays = 0.6;              // days the ground takes to dry after rain
	constexpr float kOvercastCoverage = 0.97f;    // cloud cover when fully overcast
	constexpr float kOvercastSun = 0.25f;         // the sun's strength left when fully overcast
	constexpr float kOvercastGrey = 0.65f;        // how far the sky turns grey
	constexpr float kOvercastHaze = 1.8f;         // haze density multiplier when overcast
	constexpr float kOvercastWind = 0.8f;         // extra wind when overcast
	constexpr int kRainRate = 1800;               // drops per second around you in a downpour
	constexpr int kRainMax = 2600;
	constexpr float kRainSpeed = 11.0f;           // m/s
	constexpr float kDebugRunMultiplier = 4.0f;   // debug: holding Shift moves this much faster (walking and flying)
	constexpr float kFlySpeed = 12.0f;            // debug: m/s flying (G), no collision
	constexpr int kDebugTimeScaleMax = 3;         // debug: ] and [ multiply outside time by 10^0 .. 10^this

	// ---- Day and night (DayNight.h) ---------------------------------------------------
	// The sun rises in the east (+x) at 6:00, stands in the south (-z, over the valley) at
	// noon and sets in the west at 18:00; the moon follows half a day behind, on a lower path.
	constexpr float kSunNoonElevation = 52.0f;    // degrees above the horizon at noon
	constexpr uint32_t kSunNoonColor = 0xfff4e2;
	constexpr uint32_t kSunLowColor = 0xffa25a;   // near the horizon
	constexpr float kSunStrength = 1.3f;          // strong sun against a weaker sky: shadows (trees, clouds) read clearly
	constexpr uint32_t kMoonColor = 0x9fb4ff;
	constexpr float kMoonStrength = 0.16f;
	constexpr float kMoonDiskBrightness = 3.5f;
	// Sky (zenith, horizon) and ambient for day, twilight and night.
	constexpr uint32_t kTwilightZenith = 0x4c5d94;
	constexpr uint32_t kTwilightHorizon = 0xf0a070;
	constexpr uint32_t kNightZenith = 0x070b1c;
	constexpr uint32_t kNightHorizon = 0x1c2740;
	constexpr uint32_t kNightGround = 0x0e1014;
	constexpr float kTwilightAmbient = 0.14f;
	constexpr float kNightAmbient = 0.045f;
	constexpr float kNightFill = 0.12f;           // the two fill lights at night (1 by day)
	constexpr float kStarIntensity = 1.2f;
	// Lamps and lit windows: how bright they are by day (1 at night).
	constexpr float kLampDayLevel = 0.25f;
	constexpr uint32_t kWindowLightColor = 0xffb466;   // lit windows (Shaders/TownVoxel.hlsl)
	// Indoors (the renderer's rooms, one per building with windows): the sky's ambient drops
	// to this share, over kRoomFade metres in from the walls; sunlight comes in through the
	// windows and doors.
	constexpr float kRoomAmbient = 0.3f;
	constexpr float kRoomFade = 0.6f;
	// No strobing: once outside days pass faster than one per kStrobeSlowDay real seconds,
	// the light blends towards the day's average, fully at one per kStrobeFastDay, with a
	// gentle pulse instead.
	constexpr float kStrobeSlowDay = 4.0f;
	constexpr float kStrobeFastDay = 1.0f;
	constexpr float kStrobePulse = 0.12f;         // relative brightness of the pulse
	constexpr float kStrobePulseHz = 0.7f;

	// ---- Particles (the engine's ParticleSystem) -----------------------------------------
	constexpr int kDustPerBite = 10;              // rock dust when the pick breaks rock (+2 per voxel)
	constexpr int kDustPerBiteMax = 60;
	constexpr float kDustSpeed = 1.2f;            // m/s out of the face
	constexpr int kCaveInDustPerMetre = 6;        // dust per cubic metre that falls in a cave-in
	// Sparks when the pick strikes ore or rock too hard to break (the softest stone gives none).
	constexpr float kSparkHardness = 1.3f;        // block hardness from which strikes spark
	constexpr int kSparksPerStrike = 14;          // ... more for harder blocks
	constexpr int kSparksMax = 40;
	// Cave-ins shake the ground: a rumble that grows with the rock that fell and fades with distance.
	constexpr float kRumblePerMetre = 0.12f;      // rumble per cubic metre fallen (1 = strong)
	constexpr float kRumbleReach = 18.0f;         // m beyond which a cave-in isn't felt
	constexpr float kRumbleSeconds = 1.2f;        // how long it takes to die down
	constexpr float kRumbleShake = 0.035f;        // m of camera shake at full rumble
	constexpr float kRumbleKick = 0.035f;         // radians the view jolts at full rumble
	constexpr int kCaveInDustMax = 160;
	constexpr int kShaftDustRate = 30;            // motes per second hanging in each shaft
	// The cave comes alive (milestone 14).
	constexpr uint32_t kMineLampColor = 0xffb060; // lamps hung in the first zone's caverns
	constexpr float kMineLampIntensity = 2.6f;
	constexpr float kMineLampRange = 7.0f;
	constexpr float kMineLampFlicker = 0.18f;
	constexpr float kMineLampMinHeadroom = 4.0f;  // m: only caverns at least this high get one (it hangs 1.5 m)
	constexpr uint32_t kCrystalLightColor = 0x5ff4ff;   // around clusters of Time crystal
	constexpr float kCrystalLightPerVoxel = 0.05f;      // intensity per crystal voxel in the cluster
	constexpr float kCrystalLightMax = 1.6f;
	constexpr float kCrystalLightRange = 4.0f;
	constexpr float kCrystalLightReach = 24.0f;   // m: clusters further than this from you aren't lit
	constexpr int kCrystalClusterMin = 3;         // crystal voxels in a 2 m cell to make a light
	constexpr int kLanternMoteRate = 18;          // dust motes per second drifting around you in the cave
	constexpr int kLanternMoteMax = 140;
	constexpr int kShaftDustMax = 350;

	// ---- People (Npcs.h, NpcTown.h, NpcWorld.h; Docs/NPCs.md) ---------------------------------
	constexpr float kNpcVoxelSize = 1.0f / 16.0f; // the placeholder people's blocks (1.75 m = 28 blocks); hand-made models will choose theirs
	constexpr float kNpcWalkSpeed = 1.3f;         // m per real second at the surface rate (faster as outside time speeds up)
	constexpr float kNpcStride = 1.3f;            // m per full walk cycle (two steps)
	constexpr float kNpcJitterHours = 0.35f;      // each block starts up to this much earlier or later each day (not like clockwork)
	constexpr float kNpcRainThreshold = 0.25f;    // rain from which people go to their rain places
	constexpr float kNpcPathLink = 15.0f;         // m: trail ends this close to another trail are joined (on dry land)
	constexpr float kNpcDirectDistance = 40.0f;   // m: places closer than this are walked to straight, off the trails
	constexpr float kNpcDrawDistance = 220.0f;    // m from the camera beyond which people aren't drawn
	constexpr float kNpcNearDistance = 40.0f;     // ... within which they look at you and block you
	constexpr float kNpcLookDistance = 7.0f;      // m: people turn their heads to you this close
	constexpr float kNpcBlockerDistance = 12.0f;  // m: people standing still this close block you (a box)
	// Seen while outside time races (the entrance passage, the ] key): faint, fast figures like a
	// long-exposure photograph. Rates are multiples of the surface rate.
	constexpr float kNpcLapseStart = 1.5f;        // figures start to fade here...
	constexpr float kNpcLapseFull = 4.5f;         // ... and are fully faint here (zone 1 is 5x)
	constexpr float kNpcLapseFaint = 0.4f;        // the share of a faint figure's pixels drawn
	constexpr float kNpcLapseVanish = 80.0f;      // gone from here (they'd cross the town in a blink)...
	constexpr float kNpcLapseVanishFrom = 30.0f;  // ... fading out from here
	constexpr int kNpcEchoes = 2;                 // fainter copies trailing each fast figure
	constexpr float kNpcEchoSeconds = 0.18f;      // real seconds of outside time between copies
	// Talking (E): within reach and roughly in front of you. Outside time stops while you talk.
	constexpr float kTalkDistance = 3.0f;         // m from your eye to their head
	constexpr float kTalkAimCos = 0.9f;           // cosine of the widest angle off your view (about 25 degrees)
	constexpr float kTalkTurnSeconds = 0.35f;     // the view eases round to them
	// Friendship (regard, -100..100; NpcLog.h). Gains are scaled by the person's warmth (0.5 + warmth).
	constexpr float kRegardTalk = 3.0f;           // the first conversation of each outside day
	constexpr float kRegardGiftLiked = 8.0f;      // the first gift of each outside day: something they like...
	constexpr float kRegardGiftNeutral = 3.0f;    // ... anything else...
	constexpr float kRegardGiftDisliked = -5.0f;  // ... something they dislike
	// Friendships fade while you're away: after a grace period, half of it is gone every half-life,
	// longer for patient people ((0.5 + patience) x) and for close friends (1 + regard / 50 x).
	constexpr float kRegardGraceDays = 7.0f;      // outside days you can be away without it fading
	constexpr float kRegardHalfLifeDays = 60.0f;
	// Tiers (shown, never the number): below these it's Hostile, Wary; then Stranger if you've
	// never talked; then Acquaintance, Friendly, Friend; Close from the last.
	constexpr float kTierHostile = -50.0f, kTierWary = -15.0f, kTierFriendly = 20.0f, kTierFriend = 45.0f, kTierClose = 75.0f;
	// Gossip: a share of a big change in regard reaches the people they're tied to a little later.
	constexpr float kGossipMin = 5.0f;            // changes at least this big are talked about...
	constexpr float kGossipShare = 0.3f;          // ... this share of them...
	constexpr float kGossipDays = 2.0f;           // ... this many outside days later
	// Barks: a line when you pass someone (not in a conversation).
	constexpr float kBarkDistance = 5.0f;         // m
	constexpr float kBarkCooldown = 45.0f;        // real seconds before the same person speaks again
	constexpr float kBarkGlobalCooldown = 9.0f;   // ... before anyone does
	constexpr float kBarkSeconds = 4.0f;          // how long the line shows
	constexpr float kAutosaveSeconds = 120.0f;    // real seconds between saves while playing (also after each conversation and on coming out of the cave)
	// Missions (Missions.h): the notice board, and friendship for the one a delivery is for.
	constexpr int kBoardJobsPerWeek = 3;          // jobs posted each outside week
	constexpr float kCourierRegard = 3.0f;        // the person you deliver to likes it too
	constexpr float kRetrieveReach = 2.5f;        // m: close enough to pick up a lost thing in the cave (E)
	// Lives across the eras (Life.h): gatherings, ageing, letters.
	constexpr float kWeddingHours[2] = { 16.0f, 20.0f };   // outside hours of the day
	constexpr float kFuneralHours[2] = { 10.0f, 13.0f };
	constexpr double kFuneralDelayDays = 3.0;     // after the death
	constexpr float kFestivalHours[2] = { 17.0f, 22.0f };
	constexpr int kFestivalDayOfYear = 200;       // Founders' Day (day 0 = 1 January)
	constexpr int kFestivalEras[2] = { 2, 4 };    // Settlement to Industry
	constexpr int kGatherSlots = 12;              // spots in the ring round a gathering
	constexpr float kGatherRadius[2] = { 3.0f, 4.8f };   // m: round the fire or the graves; round the clock tower
	constexpr float kGatherReach = 12.0f;         // m from its middle: you're at the gathering
	constexpr float kGatheringRegard = 6.0f;      // being there, with each of its hosts (the couple, the family)
	constexpr float kGatheringMissed = 6.0f;      // missing it, with the close friends (Friend and up) among them
	constexpr float kFestivalRegard = 2.0f;       // being at Founders' Day, with everyone there
	constexpr float kInheritShare = 0.5f;         // a child's start: this share of their parents' best friendship at the birth
	constexpr float kChildScale = 0.55f;          // a newborn's size against a grown-up's...
	constexpr float kGrownAge = 16.0f;            // ... grown by this age
	constexpr float kOldAge = 62.0f;              // from here they stoop
	constexpr double kLetterDelayDays = 2.0;      // letters about something are dated this long after it
	constexpr float kGraveRow[3] = { 168.0f, -106.0f, -3.0f };   // the cast's graves: x, z of the first, z step (beside the chapel's two)

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
		Cliff,         // the mountain face's skin (unbreakable)
		Turf,          // ... where a ledge of it faces up
		Cliff2,        // the face's other strata shades (as the terrain's rock walls)
		Cliff3,
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
		{ "Mountain rock", 0.0f,   0, 0x7a7064 },   // unbreakable; kTerrainRock
		{ "Turf",          0.0f,   0, 0x5f7d3a },   // unbreakable; kTerrainGrass
		{ "Mountain rock", 0.0f,   0, 0x655d53 },   // unbreakable; kTerrainRock2, a little darker
		{ "Mountain rock", 0.0f,   0, 0x6f665b },   // unbreakable; kTerrainRock, darker
	};

	inline const BlockInfo& Info(Block block) { return kBlocks[static_cast<int>(block)]; }
	inline bool Unbreakable(Block block)
	{
		return block == Block::Bedrock || block == Block::Cliff || block == Block::Cliff2 || block == Block::Cliff3 || block == Block::Turf;
	}
}
