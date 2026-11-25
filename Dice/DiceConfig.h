#pragma once
// Every tunable of the dice prototype, with the reference's names and defaults
// (PROTOTYPE_EXPORT.md, section B12). The debug panel edits this live.

enum class Layout { Square, Hex, Scatter };
enum class BoardShape { Full, Circle, Ring, Islands };
enum class CollisionMode { Nudge, Bump, Stack };

constexpr int kMaxSide = 80;            // the panel's range (B12)
constexpr int kMaxProfileSide = 320;    // presets and dev switches may go this far (M10: ~100k dice)
// Slots (and dice) the arrays hold. The reference allocates 80² x 1.2 for hex (~1.155 slots per
// cell²), but scatter at its minimum spacing 0.7 packs ~1.29 per cell² (~8,100 slots at 80 x 80),
// so there the reference silently drops the last slots generated and leaves a hole in the board.
// 1.3 holds every layout (Docs/Port.md, "Known differences").
constexpr int kMaxDice = kMaxSide * kMaxSide * 13 / 10;
// Slots (and dice) a board of size N needs room for; the arrays grow to it when one is dealt.
constexpr int CapacityFor(int N) { return N * N * 13 / 10 + 16; }

struct DiceConfig
{
	// Board
	Layout layout = Layout::Square;
	BoardShape boardShape = BoardShape::Full;
	int boardSize = 40;            // board is N x N cells
	float scatterSpacing = 0.95f;  // scatter layout: minimum distance between slots (cells)
	float density = 0.8f;          // chance each slot starts with a die (0..1)
	float dieSize = 0.62f;         // die edge length in cells (load-time only)
	float slotJitter = 0.12f;      // max random offset from slot centre, in cells

	// Hover
	float hoverRadius = 1.5f;      // in cells

	// Roll
	float rollDistMin = 1.0f;      // cells
	float rollDistMax = 3.0f;      // cells
	float rollDuration = 0.5f;     // seconds
	float durationJitter = 0.15f;  // +/- fraction of rollDuration
	float hopHeight = 0.9f;        // peak hop height in cells (scaled down for short hops)
	float tumblePerCell = 1.0f;    // quarter-turns per cell travelled
	float squash = 0.3f;           // landing squash amount (0 = none)
	float cooldown = 0.6f;         // seconds after landing before it can roll again
	float readyGlowTime = 0.45f;   // seconds the "ready again" glow lasts
	float luck = 0.0f;             // 0 = fair die, 1 = heavily biased toward 6

	// Collisions
	CollisionMode collisionMode = CollisionMode::Nudge;
	int stackMax = 3;              // stack mode: max dice in one stack (incl. the bottom one)
	float stackChance = 0.75f;     // stack mode: chance a die settles on top (else nudges to a free slot)
	float stackScorePerLevel = 0.5f;   // landing score x (1 + this x level)
	float toppleBonus = 15.0f;     // per die knocked off a full stack
	float toppleSpreadDeg = 70.0f; // spread of the toppled dice around the incoming direction
	float toppleDistMin = 1.0f;
	float toppleDistMax = 2.2f;    // higher dice fly a bit further
	int bumpChainLimit = 2;        // max generations of bump -> bump -> bump
	float bumpsPerSecond = 40.0f;  // global rate limit (token bucket)
	float bumpDistMin = 1.0f;
	float bumpDistMax = 1.6f;
	float bumpSpreadDeg = 20.0f;   // random spread around the impact direction
	float bumpScoreMult = 0.5f;    // pushed dice score this fraction
	float bumpContact = 1.15f;     // contact distance, as a multiple of dieSize (when the push starts)
	float bumpNormalWeight = 0.6f; // push direction: 0 = along incoming travel, 1 = along contact normal
	float bumpHopMult = 1.0f;      // pushed dice hop height multiplier
	float bumpDurationMult = 0.8f; // ...and faster (they also start fast and slow down)
	float bumpRecoil = 0.12f;      // incoming die stops this far short of slot centre (cells)
	float bounceChance = 0.5f;     // bump mode: chance the incoming die ricochets off instead of pushing
	float bounceDistMin = 0.8f;    // ricochet distance from the contact point (cells)
	float bounceDistMax = 1.5f;
	float bounceSpreadDeg = 25.0f; // random spread around the mirror-reflected direction
	float bounceTime = 0.3f;       // seconds for the ricochet leg
	float bounceHopMult = 0.35f;   // ricochet hop height, as a fraction of the main hop
	float hitWobble = 0.14f;       // jolt on the die that got hit but stayed put
	bool clashEnabled = true;
	float clashDistance = 0.55f;   // cells: how close two airborne paths must pass
	float clashDeflect = 1.0f;     // cells each die's landing is shoved sideways
	float clashBonus = 25.0f;
	bool midAirCollide = true;     // per-frame check: airborne dice that touch glance off each other
	float midAirContact = 0.95f;   // touch distance, as a multiple of dieSize
	float midAirDeflect = 0.4f;    // cells each die veers sideways before curving back to its slot
	int midAirMaxHits = 2;         // max glancing hits per die per roll (stops ping-pong)

	// Scoring / session
	float pointsPerPip = 10.0f;
	float sessionSeconds = 30.0f;

	// Audio
	float volume = 0.5f;
	bool landTicks = true;
};
