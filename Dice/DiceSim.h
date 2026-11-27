#pragma once
// The dice simulation: struct-of-arrays state, slots and the board, all in the reference's
// coordinates (PROTOTYPE_EXPORT.md B2: 1 unit = 1 cell, Y up, the board on y = 0 spanning
// [-N/2, N/2]). Pure C++ with no engine types, so the tests can run it headless.
//
// M1: the board, the slots of the square layout and the initial deal. M2: nearestSlot and
// hover (B7). M3: launch (B8), nudge, the animation and the exact landing (B9). M4: the session,
// scoring and the effects the module shows (popups, sparks, sounds: B15). M5: mid-air clash
// prediction at launch (B10.6) and per-frame separation and glancing (B10.7, B10.8). M6: bump
// mode, push at contact (B10.2) and bounce-off (B10.3). M7: hex and scatter layouts and the
// board shapes (B4). M8: stack mode, landing on top (B10.4) and toppling full stacks (B10.5).
#include "DiceConfig.h"
#include "DiceMath.h"
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace dice
{
	enum DieState : uint8_t { S_READY = 0, S_ROLLING = 1, S_COOLDOWN = 2, S_PENDING = 3 };   // PENDING = launched with a delay

	// Face layout (B3). Box face order +x, -x, +y, -y, +z, -z; opposite faces sum to 7.
	constexpr int kFaceOrder[6] = { 2, 5, 1, 6, 3, 4 };
	Vec3 FaceNormal(int face);                  // the local normal of face 1..6
	const Quat& FaceUp(int face);               // rotation taking that face's normal to +Y
	Quat RestQuat(int face, double yaw);        // yaw(about +Y) x FaceUp[face]
	int FaceOnTop(const Quat& q);               // the face whose normal q turns nearest to +Y

	// Math.random with a seed (tests and repeatable views).
	class Random
	{
	public:
		explicit Random(uint64_t seed = std::random_device{}()) : mEngine(seed) {}
		double operator()() { return std::uniform_real_distribution<double>(0.0, 1.0)(mEngine); }
		double Range(double a, double b) { return a + (b - a) * (*this)(); }
		void Seed(uint64_t seed) { mEngine.seed(seed); }
	private:
		std::mt19937_64 mEngine;
	};

	enum class Phase : uint8_t { Free, Running, Finishing, Ended };

	// Effects the simulation asks for; the module shows them and empties these lists.
	// Popup style flags, in the order the reference's stylesheet applies them (B15).
	enum PopStyle : uint8_t { Pop_Free = 1, Pop_Bump = 2, Pop_Six = 4, Pop_Clash = 8, Pop_Stack = 16, Pop_Topple = 32, Pop_Gold = 64 };

	// Die types (the upgrades, DiceUpgrades.h; not in the reference): golden dice score
	// goldMultiplier times as much, lucky dice roll with luckyLuck added to the luck.
	enum DieKind : uint8_t { Kind_Normal = 0, Kind_Gold = 1, Kind_Lucky = 2 };
	struct DieKinds
	{
		double goldShare = 0;          // fraction of the dealt dice that are golden
		double luckyShare = 0;         // ...and lucky
		double goldMultiplier = 2;
		double luckyLuck = 1.5;
	};
	struct PopupRequest { float x, y, z; std::string text; uint8_t style; };
	struct SparkRequest { float x, y, z; int count; float r, g, b; };
	enum class Sound : uint8_t { Clack, Knock, Tick };

	// A scheduled effect (B5 "Event queue"), fired when now >= t.
	struct Event
	{
		enum Type : uint8_t { Bump, Clash, Topple } type = Clash;
		double t = 0;
		float x = 0, y = 0, z = 0;
		int session = 0;
		int n = 0;
	};

	// One entry per die (B5). Times are double (B13.9).
	struct DiceState
	{
		std::vector<uint8_t> state;
		std::vector<int32_t> cell;               // slot it rests in / has claimed
		std::vector<float> px, py, pz;           // current position (py = height above rest, the render adds HALF x scaleY)
		std::vector<float> q;                    // current rotation (4 per die)
		std::vector<float> sx, sz;               // path start
		std::vector<float> mx, mz, um;           // via point and its path fraction
		std::vector<float> ex, ez;               // path end
		std::vector<double> t0;
		std::vector<float> dur, hop;
		std::vector<float> qs, qe;               // pre-wound start rotation, final rotation (4 per die)
		std::vector<float> ax, az, ang;          // tumble axis (horizontal) and total angle
		std::vector<uint8_t> face;               // face that is / will be up (1..6)
		std::vector<uint8_t> kind;               // DieKind
		std::vector<double> timer, landT, readyT, hitT;
		std::vector<uint8_t> chain, bumped, clashed, ease;
		std::vector<int32_t> session;
		std::vector<float> h0;                   // path fraction at the current start (re-plans)
		std::vector<uint8_t> hits;
		std::vector<int32_t> lastHit;
		std::vector<float> offX, offZ;           // mid-air separation offset
		std::vector<float> bu, arc;              // ricochet: hit fraction (0 = none), main-arc time scale
		std::vector<int32_t> below;              // die underneath (-1 = on the board)
		std::vector<uint8_t> level;              // 0 = on the board
		std::vector<float> by;                   // resting base height (level x dieSize)
		std::vector<float> sb, eb;               // roll start / end base height
		std::vector<uint8_t> inActive;           // bookkeeping for the B13.1 check: in the active list

		void Allocate(int capacity);
	};

	class DiceSim
	{
	public:
		DiceSim();

		DiceConfig config;
		Random rand;
		// Shares and strengths of the die types, applied when the board is dealt. All zero (the
		// reference): every die is normal and the dealing draws no extra random numbers.
		DieKinds kinds;

		DiceState D;
		int diceCount = 0;
		double now = 0.0;
		int N = 40;                              // board size of the current board

		// Slots: points on the board, each holding at most one stack.
		std::vector<float> slotX, slotZ;
		std::vector<int32_t> slotOwner;          // the top die of the slot's stack, or -1
		std::vector<int32_t> hashHead, hashNext; // 1 x 1 hash cells: first slot, next slot in the cell
		int slotCount = 0;

		float Half() const { return mHalf; }     // dieSize / 2, fixed at load like the reference's HALF
		int Capacity() const { return mCapacity; }

		// Indices of ROLLING and PENDING dice: only these are simulated.
		std::vector<int32_t> active;
		int activeCount = 0;

		struct Stats { long long rolls = 0, landings = 0, bumps = 0, bounces = 0, clashes = 0, stacks = 0, topples = 0; } stats;
		// Set when a launch finds the die already in the active list (B13.1); tests fail on it.
		bool reentryDetected = false;

		// Bump token bucket (B5): refills at bumpsPerSecond, capped there.
		double bumpTokens = 40;
		std::vector<Event> events;

		// The session (B15 "Session phases"): free -> running -> finishing -> ended.
		Phase phase = Phase::Free;
		int sessionId = 0;
		double timeLeft = 30;
		long long score = 0;
		int sessionAirborne = 0;
		struct SessionStats { int rolls = 0, bumps = 0, clashes = 0, sixes = 0; } sStats;
		void StartSession();             // Start / Restart
		bool SessionCounts(int session) const
		{
			return session != 0 && session == sessionId && (phase == Phase::Running || phase == Phase::Finishing);
		}

		// Effects for the module (drained by it every frame).
		std::vector<PopupRequest> popupsOut;
		std::vector<SparkRequest> sparksOut;
		std::vector<Sound> soundsOut;

		// One frame in the reference's order (B6), up to the rendering:
		//   now += dt; bump tokens; updateSession; processEvents; hover; updateRolls;
		//   resolveMidAir; COOLDOWN -> READY.
		// `dt` is the real frame time; it's clamped to 0.05 here.
		void Frame(double dt, bool inside, double hitX, double hitZ);

		// Profiling load (M10, not in the reference): each frame, about stormPerSecond x dt of the
		// dice are picked at random and launched if they're a READY top, as if hovered.
		double stormPerSecond = 0;

		// Regenerates the slots and deals the dice (B4 "Rebuilding the board"). `forcedFace`
		// 1..6 deals every die with that face up (a check of the face-up mapping); 0 = random.
		void BuildBoard(int forcedFace = 0);

		// B4 shapes: the island circles (centre and radius as fractions of N).
		static constexpr double kIslands[5][3] = { { 0, 0, 0.17 }, { -0.3, -0.3, 0.15 }, { 0.3, -0.3, 0.15 }, { -0.3, 0.3, 0.15 }, { 0.3, 0.3, 0.15 } };

		int HashCoord(double v) const;
		int HashOf(double x, double z) const { return HashCoord(z) * N + HashCoord(x); }

		// Nearest slot to (x, z), searching hash cells ring by ring (B4). freeOnly skips owned
		// slots, with a little randomness so nudges don't always go one way. -1 if none.
		int NearestSlot(double x, double z, bool freeOnly);

		// B7, given where the mouse ray meets the board plane (`inside` = the pointer is over
		// the view): sets the hovered slot, and every READY die whose slot is within
		// hoverRadius is launched (random direction, distance in [rollDistMin, rollDistMax]).
		void UpdateHover(bool inside, double hitX, double hitZ);

		// B8. angle in radians (cell space), dist in cells; delay > 0 waits (PENDING) first.
		void Launch(int i, double angle, double dist, int chain, int bumped, double delay, int session);
		// B9: advances the active dice, landing those whose roll is over.
		void UpdateRolls(double dt);
		// B10.7: airborne dice that touch are pushed apart, and glance off on first touch.
		void ResolveMidAir();
		// B8 pickFace: weight exp(luck x 3 x (f - 3.5) / 2.5), with `extraLuck` added (lucky dice).
		int PickFace(double extraLuck = 0);
		void FaceWeights(double w[7], double& sum, double extraLuck = 0) const;
		double ExpectedFace(double extraLuck = 0) const;
		// Ground position of die i at roll fraction u (start -> via -> end).
		void PathXZ(int i, double u, double& x, double& z) const;
		// B10.2 step 4: the first roll fraction at which die i's path comes within
		// dieSize x bumpContact of (ox, oz) (1 if it never does).
		double ContactU(int i, double ox, double oz) const;
		double PathFrac(int i, double u) const { const double v = 1 - u; return D.ease[i] ? 1 - v * v : u; }
		// Reflects a coordinate back inside [0, max] (the board edge), up to 4 times.
		static double Reflect(double v, double max);
		bool hoverVisible = false;
		double hoverX = 0, hoverZ = 0;
		int hoverCell = -1;
		int hoverResponses = 0;                  // dice launched by the hover so far (stats and tests)

		// B9 "Rendering": COOLDOWN becomes READY once its timer runs out (done for every die
		// each frame, as the reference's writeInstances does).
		void ExpireCooldowns();
		// The colour a die is tinted with now (multiplies the texture; above 1 brightens).
		void DieColor(int i, float& r, float& g, float& b) const;

	private:
		void GenerateSlots();
		void AddSlot(double x, double z);
		void GenSquare();
		void GenHex();
		void GenScatter();
		bool InShape(double x, double z) const;
		void LeaveSlot(int i);
		void SetEnd(int i, int c);
		int ResolveTarget(int i, int target, double travel, int chain, int session);
		int BounceOff(int i, int occ, double travel);
		int StackOn(int i, int T, int target);
		int Topple(int i, int target, double travel, int chain, int session);
		int NearestFree(int target);
		void Land(int i);
		int CheckClash(int i, int target, double t0, double dur, int session);
		void Glance(int i, double u, double nx, double nz, int other);
		std::vector<int32_t> mBucketHead, mBucketNext, mBucketCell;
		void UpdateSession(double dt);
		void ProcessEvents();
		void AwardClash(float x, float y, float z, int session);
		void Popup(float x, float y, float z, std::string text, uint8_t style) { popupsOut.push_back({ x, y, z, std::move(text), style }); }
		void Spark(float x, float y, float z, int count = 22, float r = 1, float g = 0.75f, float b = 0.3f) { sparksOut.push_back({ x, y, z, count, r, g, b }); }
		double Jit() { return (rand() * 2 - 1) * config.slotJitter; }

		float mHalf = 0.31f;
		int mCapacity = 0;
		double mStormCarry = 0;
		void Reserve(int capacity);
	};
}
