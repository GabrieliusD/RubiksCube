// DiceTests: checks of the dice port that run without the engine (PROTOTYPE_EXPORT.md B11,
// B13.6). Prints a [pass] / [FAIL] line per check and exits non-zero on any failure.
#include "../DiceSim.h"
#include "../DiceVisuals.h"
#include "../DiceAudio.h"
#include "../DiceControls.h"
#include "../DiceUpgrades.h"
#include <RmlUi/Core.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dice;
using namespace DirectX;

namespace
{
	int gFailures = 0;

	void Check(bool ok, const std::string& what)
	{
		std::printf("%s %s\n", ok ? "[pass]" : "[FAIL]", what.c_str());
		if (!ok) ++gFailures;
	}

	bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

	// Dark or red blobs in one atlas cell (4-connected flood fill over pip-coloured pixels).
	int CountPips(const Image& atlas, int column, int row)
	{
		const int S = kAtlasFace;
		std::vector<uint8_t> seen(static_cast<size_t>(S) * S, 0);
		auto isPip = [&](int x, int y)
		{
			const uint8_t* p = &atlas.rgba[(static_cast<size_t>(row * S + y) * atlas.width + column * S + x) * 4];
			return p[1] < 120;   // pips (#1c1e23, #c8372d) have little green; face and border have lots
		};
		int blobs = 0;
		std::vector<int> stack;
		for (int y = 0; y < S; ++y)
			for (int x = 0; x < S; ++x)
			{
				if (seen[y * S + x] || !isPip(x, y)) continue;
				++blobs;
				stack.push_back(y * S + x);
				seen[y * S + x] = 1;
				while (!stack.empty())
				{
					const int at = stack.back();
					stack.pop_back();
					const int px = at % S, py = at / S;
					const int nx[4] = { px + 1, px - 1, px, px }, ny[4] = { py, py, py + 1, py - 1 };
					for (int k = 0; k < 4; ++k)
					{
						if (nx[k] < 0 || ny[k] < 0 || nx[k] >= S || ny[k] >= S || seen[ny[k] * S + nx[k]] || !isPip(nx[k], ny[k])) continue;
						seen[ny[k] * S + nx[k]] = 1;
						stack.push_back(ny[k] * S + nx[k]);
					}
				}
			}
		return blobs;
	}

	// The atlas cell a mesh face's UVs fall in (centre of its four vertices, D3D v down).
	void CellOfMeshFace(const MeshData& mesh, int boxFace, int& column, int& row)
	{
		double u = 0, v = 0;
		for (int k = 0; k < 4; ++k) { u += mesh.vertices[boxFace * 4 + k].uv.x / 4; v += mesh.vertices[boxFace * 4 + k].uv.y / 4; }
		column = static_cast<int>(u * 3);
		row = static_cast<int>(v * 2);
	}

	void TestFaceUp()
	{
		for (int f = 1; f <= 6; ++f)
		{
			const Vec3 up = FaceUp(f).Rotate(FaceNormal(f));
			Check(Near(up.x, 0) && Near(up.y, 1) && Near(up.z, 0), "faceUp[" + std::to_string(f) + "] takes face " + std::to_string(f) + "'s normal to +Y");
			bool restOk = true;
			for (int k = 0; k < 16; ++k) restOk &= FaceOnTop(RestQuat(f, k * 0.41)) == f;
			Check(restOk, "restQuat(" + std::to_string(f) + ", any yaw) keeps face " + std::to_string(f) + " on top");
		}
		bool opposite = true;
		for (int f = 1; f <= 6; ++f)
		{
			const Vec3 a = FaceNormal(f), b = FaceNormal(7 - f);
			opposite &= Near(a.x, -b.x) && Near(a.y, -b.y) && Near(a.z, -b.z);
		}
		Check(opposite, "opposite faces sum to 7");
	}

	void TestAtlasAndMesh()
	{
		const Image atlas = MakeAtlas();
		const MeshData mesh = MakeDieMesh(0.62f);
		Check(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "die mesh: 6 faces of 4 vertices");
		for (int k = 0; k < 6; ++k)
		{
			const int value = kFaceOrder[k];
			// The face's normal is the face of that value (box face order +x, -x, +y, -y, +z, -z).
			const XMFLOAT3& n = mesh.vertices[k * 4].normal;
			const Vec3 expected = FaceNormal(value);
			Check(Near(n.x, expected.x) && Near(n.y, expected.y) && Near(n.z, expected.z),
				"box face " + std::to_string(k) + " is the face of value " + std::to_string(value));
			int column = 0, row = 0;
			CellOfMeshFace(mesh, k, column, row);
			const int pips = CountPips(atlas, column, row);
			Check(pips == value, "box face " + std::to_string(k) + " shows " + std::to_string(pips) + " pips (wants " + std::to_string(value) + ")");
		}
		// The acceptance check of B3: faceUp[6] shows six pips on top. The mesh face whose
		// normal ends up at +Y must be textured with six pips.
		for (int f = 1; f <= 6; ++f)
		{
			int top = -1;
			for (int k = 0; k < 6; ++k)
			{
				const XMFLOAT3& n = mesh.vertices[k * 4].normal;
				if (FaceUp(f).Rotate({ n.x, n.y, n.z }).y > 0.99) top = k;
			}
			int column = 0, row = 0;
			CellOfMeshFace(mesh, top, column, row);
			Check(top >= 0 && CountPips(atlas, column, row) == f, "a die at faceUp[" + std::to_string(f) + "] shows " + std::to_string(f) + " pips on top");
		}
	}

	// B13.6: the reference's tumble (axis (d.z, 0, -d.x), angle phi = ang (ur - 1) rising to
	// 0) rolls the die forward over its leading edge: its top moves along the travel.
	void TestTumbleDirection()
	{
		bool forward = true;
		for (int k = 0; k < 8; ++k)
		{
			const double a = k * kPi / 4, dx = std::cos(a), dz = std::sin(a);
			const Vec3 axis{ dz, 0, -dx };
			const double ang = kPi / 2;
			const auto phiAt = [&](double u) { return ang * ((1 - std::pow(1 - u, 1.7)) - 1); };
			const Vec3 top0 = Quat::AxisAngle(axis, phiAt(0.4)).Rotate({ 0, 1, 0 });
			const Vec3 top1 = Quat::AxisAngle(axis, phiAt(0.41)).Rotate({ 0, 1, 0 });
			forward &= (top1.x - top0.x) * dx + (top1.z - top0.z) * dz > 0;
		}
		Check(forward, "the tumble rolls the die forward over its leading edge, in every direction");
	}

	// The render mirror (DiceVisuals.h): a die matrix maps the mirrored mesh like the reference's
	// matrix maps the reference mesh, mirrored.
	void TestMirror()
	{
		const Quat q = Quat::AxisAngle(Vec3{ 0.6, 0, 0.8 }, 0.7).Multiply(RestQuat(3, 1.1));
		float qf[4];
		PutQ(qf, 0, q);
		XMFLOAT4X4 m;
		DieMatrix(qf, 1.2f, 0.7f, 1.5f, 0.4f, -2.0f, m);
		const Vec3 local{ 0.31, -0.31, 0.31 };
		// Reference: world = diag(1.2, 0.7, 1.2) R local + t.
		Vec3 r = q.Rotate(local);
		r = { r.x * 1.2 + 1.5, r.y * 0.7 + 0.4, r.z * 1.2 - 2.0 };
		const Vec3 e = ToEngine(local);
		const double ox = e.x * m.m[0][0] + e.y * m.m[1][0] + e.z * m.m[2][0] + m.m[3][0];
		const double oy = e.x * m.m[0][1] + e.y * m.m[1][1] + e.z * m.m[2][1] + m.m[3][1];
		const double oz = e.x * m.m[0][2] + e.y * m.m[1][2] + e.z * m.m[2][2] + m.m[3][2];
		const Vec3 want = ToEngine(r);
		Check(Near(ox, want.x, 1e-5) && Near(oy, want.y, 1e-5) && Near(oz, want.z, 1e-5), "the die matrix is the reference's, mirrored (world-space squash)");
	}

	void TestBoard()
	{
		DiceSim sim;
		sim.rand.Seed(7);
		sim.config.density = 1.0f;
		sim.BuildBoard();
		Check(sim.slotCount == 1600 && sim.diceCount == 1600, "a 40 x 40 square board at density 1 holds 1,600 dice");
		bool ok = true;
		for (int i = 0; i < sim.diceCount; ++i)
		{
			const int c = sim.D.cell[i];
			ok &= sim.slotOwner[c] == i && std::fabs(sim.D.px[i] - sim.slotX[c]) <= sim.config.slotJitter + 1e-6
				&& std::fabs(sim.D.pz[i] - sim.slotZ[c]) <= sim.config.slotJitter + 1e-6
				&& FaceOnTop(GetQ(sim.D.q.data(), i)) == sim.D.face[i];
		}
		Check(ok, "every dealt die owns its slot, sits within the jitter and shows its face");
		sim.BuildBoard(6);
		bool six = true;
		for (int i = 0; i < sim.diceCount; ++i) six &= FaceOnTop(GetQ(sim.D.q.data(), i)) == 6;
		Check(six, "the face check deals every die six up");
	}

	// B4: nearestSlot finds the closest slot (ring search), and with freeOnly a free one.
	void TestNearestSlot()
	{
		DiceSim sim;
		sim.rand.Seed(11);
		sim.config.density = 0.8f;
		sim.BuildBoard();
		Random pick(5);
		bool exact = true, free = true;
		const double h = sim.N / 2.0;
		for (int k = 0; k < 4000; ++k)
		{
			const double x = pick.Range(-h - 2, h + 2), z = pick.Range(-h - 2, h + 2);
			double best = 1e18;
			for (int s = 0; s < sim.slotCount; ++s)
				best = std::min(best, (sim.slotX[s] - x) * (sim.slotX[s] - x) + (sim.slotZ[s] - z) * (sim.slotZ[s] - z));
			const int s = sim.NearestSlot(x, z, false);
			exact &= s >= 0 && Near((sim.slotX[s] - x) * (sim.slotX[s] - x) + (sim.slotZ[s] - z) * (sim.slotZ[s] - z), best, 1e-9);
			const int f = sim.NearestSlot(x, z, true);
			free &= f >= 0 && sim.slotOwner[f] < 0;
		}
		Check(exact, "nearestSlot matches a brute-force search (4,000 points, on and off the board)");
		Check(free, "nearestSlot(freeOnly) always returns a free slot");
	}

	// B7 / M2 done-when: exactly the READY dice whose slots are under the ring respond (are
	// launched); after landing they dim for the cooldown, then glow when ready again.
	void TestHover()
	{
		DiceSim sim;
		sim.rand.Seed(3);
		sim.BuildBoard();
		Random pick(9);
		bool sameSet = true, cellOk = true;
		for (int k = 0; k < 300; ++k)
		{
			sim.config.hoverRadius = static_cast<float>(pick.Range(0.3, 6.0));
			const double h = sim.N / 2.0;
			const double x = pick.Range(-h - 3, h + 3), z = pick.Range(-h - 3, h + 3);
			// Some dice are cooling down from earlier hovers: they must not respond.
			std::vector<int> expected;
			const double R2 = sim.config.hoverRadius * sim.config.hoverRadius;
			for (int s = 0; s < sim.slotCount; ++s)
			{
				const int o = sim.slotOwner[s];
				const double dx = sim.slotX[s] - x, dz = sim.slotZ[s] - z;
				if (o >= 0 && sim.D.state[o] == S_READY && dx * dx + dz * dz <= R2) expected.push_back(o);
			}
			std::vector<uint8_t> before(sim.D.state.begin(), sim.D.state.begin() + sim.diceCount);
			const int responsesBefore = sim.hoverResponses;
			sim.UpdateHover(true, x, z);
			int changed = 0;
			for (int i = 0; i < sim.diceCount; ++i) changed += sim.D.state[i] != before[i];
			bool all = changed == static_cast<int>(expected.size()) && sim.hoverResponses - responsesBefore == changed;
			for (int o : expected) all &= sim.D.state[o] == S_ROLLING;
			sameSet &= all;

			int wantCell = -1;
			if (std::fabs(x) < h && std::fabs(z) < h)
			{
				const int s = sim.NearestSlot(x, z, false);
				if ((sim.slotX[s] - x) * (sim.slotX[s] - x) + (sim.slotZ[s] - z) * (sim.slotZ[s] - z) < 1) wantCell = s;
			}
			cellOk &= sim.hoverCell == wantCell;
			sim.now += 0.05;
			sim.ExpireCooldowns();
		}
		Check(sameSet, "hover: exactly the READY dice under the ring respond (300 rings, radius 0.3..6)");
		Check(cellOk, "hover: the hovered slot is the nearest one within 1 cell, on the board only");

		sim.UpdateHover(false, 0, 0);
		Check(!sim.hoverVisible && sim.hoverCell < 0, "no pointer: no ring, no hovered slot");

		// Dim, then glow (B9 "Colour").
		DiceSim one;
		one.config.boardSize = 6;
		one.config.density = 1.0f;
		one.BuildBoard();
		const int i = one.slotOwner[0];
		one.config.hoverRadius = 0.3f;
		one.Frame(1.0 / 60, true, one.slotX[0], one.slotZ[0]);
		const bool launched = one.D.state[i] == S_ROLLING;
		for (int k = 0; k < 120 && one.D.state[i] == S_ROLLING; ++k) one.Frame(1.0 / 60, false, 0, 0);
		float r, g, b;
		one.DieColor(i, r, g, b);
		Check(launched && one.D.state[i] == S_COOLDOWN && Near(r, 0.6) && Near(g, 0.6) && Near(b, 0.6), "a hovered die rolls, lands and is dimmed to 0.6 grey");
		one.now += one.config.cooldown - 0.01;
		one.ExpireCooldowns();
		Check(one.D.state[i] == S_COOLDOWN, "it stays dim for the cooldown");
		one.now += 0.02;
		one.ExpireCooldowns();
		one.DieColor(i, r, g, b);
		Check(one.D.state[i] == S_READY && r > 2.1f && g > 1.7f && b < 0.7f, "then it is ready, glowing warm (about 2.2, 1.8, 0.6)");
		one.now += one.config.readyGlowTime;
		one.DieColor(i, r, g, b);
		Check(Near(r, 1) && Near(g, 1) && Near(b, 1), "the glow is gone after readyGlowTime");
	}

	// ---- B11 invariants ---------------------------------------------------------------
	// 1 slot chains, 2 every die in exactly one chain, 3 resting heights, 4 no duplicates in
	// the active list. Returns "" when they hold, else what broke.
	std::string Invariants(const DiceSim& sim, int stackLimit = 0)
	{
		if (stackLimit < sim.config.stackMax) stackLimit = sim.config.stackMax;
		const DiceState& D = sim.D;
		std::vector<int> seen(sim.diceCount, 0);
		for (int s = 0; s < sim.slotCount; ++s)
		{
			int length = 0, expectLevel = -1;
			for (int d = sim.slotOwner[s]; d >= 0; d = D.below[d])
			{
				if (d >= sim.diceCount) return "slot " + std::to_string(s) + " names die " + std::to_string(d) + " past the dice";
				if (D.cell[d] != s) return "die " + std::to_string(d) + " is in slot " + std::to_string(s) + "'s chain but its cell is " + std::to_string(D.cell[d]);
				if (expectLevel >= 0 && D.level[d] != expectLevel) return "slot " + std::to_string(s) + ": levels not consecutive";
				expectLevel = D.level[d] - 1;
				if (++seen[d] > 1) return "die " + std::to_string(d) + " is in a chain twice";
				if (++length > stackLimit) return "slot " + std::to_string(s) + ": chain longer than stackMax";
			}
			if (length > 0 && expectLevel != -1) return "slot " + std::to_string(s) + ": the bottom die isn't at level 0";
		}
		for (int i = 0; i < sim.diceCount; ++i)
		{
			if (seen[i] != 1) return "die " + std::to_string(i) + " is in " + std::to_string(seen[i]) + " chains";
			const bool resting = D.state[i] == S_READY || D.state[i] == S_COOLDOWN;
			if (resting && std::fabs(D.py[i] - D.level[i] * sim.config.dieSize) > 1e-4)
				return "resting die " + std::to_string(i) + " at py " + std::to_string(D.py[i]) + ", level " + std::to_string(D.level[i]);
		}
		int moving = 0;
		std::vector<int> inList(sim.diceCount, 0);
		for (int i = 0; i < sim.diceCount; ++i) moving += D.state[i] == S_ROLLING || D.state[i] == S_PENDING;
		for (int k = 0; k < sim.activeCount; ++k)
		{
			const int i = sim.active[k];
			if (++inList[i] > 1) return "die " + std::to_string(i) + " is in the active list twice";
			if (D.state[i] != S_ROLLING && D.state[i] != S_PENDING) return "active die " + std::to_string(i) + " isn't moving";
		}
		if (moving != sim.activeCount) return "active list " + std::to_string(sim.activeCount) + " vs " + std::to_string(moving) + " moving dice";
		if (sim.reentryDetected) return "a die was launched while already in the active list (B13.1)";
		if (sim.bumpTokens < 0) return "the bump token bucket went negative";
		for (int k = 0; k < sim.activeCount; ++k)
			if (D.chain[sim.active[k]] > sim.config.bumpChainLimit) return "a push chain went past bumpChainLimit";
		return "";
	}

	long long lastTopples = 0;   // topples in the last stack scenario run (packed boards)

	struct Scenario
	{
		std::string name;
		int board = 40;
		float density = 0.8f;
		double fps = 60;
		bool spikes = false;          // some frames take 0.1 s (clamped to 0.05)
		void (*tune)(DiceConfig&) = nullptr;
		CollisionMode mode = CollisionMode::Nudge;
		Layout layout = Layout::Square;
		BoardShape shape = BoardShape::Full;
		bool hopping = false;         // the pointer jumps to a random spot every 10 frames (for 20 s)
	};

	// Hovers a sweeping pointer for `seconds`, then lets go. Checks the invariants every frame,
	// every landing (6: the face, exactly qe, at the end point) and that everything lands (5).
	void RunScenario(const Scenario& sc, uint64_t seed)
	{
		DiceSim sim;
		sim.rand.Seed(seed);
		sim.config.boardSize = sc.board;
		sim.config.density = sc.density;
		sim.config.hoverRadius = 2.0f;
		sim.config.collisionMode = sc.mode;
		sim.config.layout = sc.layout;
		sim.config.boardShape = sc.shape;
		if (sc.tune) sc.tune(sim.config);
		sim.BuildBoard();
		Random frames(seed * 31 + 7);

		std::string broken;
		long long landings = 0, wrongFaces = 0, inexact = 0;
		int maxLevel = 0;
		const double h = sim.N / 2.0;
		// B11.5 says "about 2 s" for the defaults; a push chain can take its rolls one after another
		// (the longest roll, then each pushed one at bumpDurationMult), plus a bounce leg.
		const DiceConfig& c = sim.config;
		const double chainTime = c.rollDuration * (1 + c.durationJitter) * (1 + (sc.mode != CollisionMode::Nudge ? c.bumpChainLimit * c.bumpDurationMult : 0)) + c.bounceTime
			+ (sc.mode == CollisionMode::Stack ? c.stackMax * 0.04 : 0);   // topples launch 0.04 s apart
		const double hover = sc.hopping ? 20.0 : 8.0, settle = std::max(2.5, chainTime + 0.5);
		Random hop(seed * 7 + 3);
		double hopX = 0, hopZ = 0;
		int frame = 0;
		double t = 0;
		std::vector<double> prevLand(sim.diceCount, -1e9);
		while (t < hover + settle && broken.empty())
		{
			double dt = 1.0 / sc.fps;
			if (sc.spikes && frames() < 0.05) dt = 0.1;
			t += dt;
			const bool inside = t < hover;
			// A Lissajous sweep over the whole board, or a pointer hopping about.
			double x = h * 0.9 * std::sin(t * 0.9 + seed), z = h * 0.9 * std::sin(t * 1.3 + seed * 0.5);
			if (sc.hopping)
			{
				if (frame++ % 10 == 0) { hopX = hop.Range(-h, h); hopZ = hop.Range(-h, h); }
				x = hopX;
				z = hopZ;
			}
			sim.Frame(dt, inside, x, z);
			broken = Invariants(sim);
			for (int i = 0; i < sim.diceCount; ++i) maxLevel = std::max(maxLevel, static_cast<int>(sim.D.level[i]));
			for (int i = 0; i < sim.diceCount; ++i)
			{
				if (sim.D.landT[i] != sim.now || prevLand[i] == sim.now) continue;
				prevLand[i] = sim.now;
				++landings;
				const Quat q = GetQ(sim.D.q.data(), i);
				if (FaceOnTop(q) != sim.D.face[i]) ++wrongFaces;
				bool exact = sim.D.px[i] == sim.D.ex[i] && sim.D.pz[i] == sim.D.ez[i] && sim.D.py[i] == sim.D.eb[i];
				for (int n = 0; n < 4; ++n) exact &= sim.D.q[i * 4 + n] == sim.D.qe[i * 4 + n];
				if (!exact) ++inexact;
			}
		}
		const char* modeName = sc.mode == CollisionMode::Bump ? "bump" : sc.mode == CollisionMode::Stack ? "stack" : "nudge";
		std::string counts = std::to_string(sim.stats.clashes) + " clashes";
		if (sc.mode == CollisionMode::Bump) counts = std::to_string(sim.stats.bumps) + " pushes, " + std::to_string(sim.stats.bounces) + " bounces, " + counts;
		if (sc.mode == CollisionMode::Stack) counts = std::to_string(sim.stats.stacks) + " stack landings, " + std::to_string(sim.stats.topples) + " topples, tallest " + std::to_string(maxLevel + 1) + ", " + counts;
		const std::string tag = std::string(modeName) + ", " + sc.name + " (seed " + std::to_string(seed) + ", " + counts + ")";
		if (sc.mode == CollisionMode::Stack && sc.density >= 0.9f) lastTopples = sim.stats.topples;
		Check(broken.empty(), tag + ": invariants 1-4 hold every frame" + (broken.empty() ? "" : ": " + broken));
		Check(landings > 50 && wrongFaces == 0 && inexact == 0, tag + ": " + std::to_string(landings) + " landings, every one on its face, exactly at its end point and rotation");
		char settleText[16];
		std::snprintf(settleText, sizeof(settleText), "%.1f", settle);
		Check(sim.activeCount == 0, tag + ": everything has landed " + settleText + " s after the pointer left");
	}

	void TestRollInvariants()
	{
		const Scenario scenarios[] = {
			{ "default board" },
			{ "full board (density 1)", 40, 1.0f },
			{ "small crowded board", 6, 1.0f },
			{ "30 fps", 40, 0.8f, 30 },
			{ "144 fps with 0.1 s spikes", 40, 0.8f, 144, true },
			{ "long rolls (dist 0..8, 1.5 s, tumble 3)", 40, 0.8f, 60, false, [](DiceConfig& c) { c.rollDistMin = 0; c.rollDistMax = 8; c.rollDuration = 1.5f; c.tumblePerCell = 3; } },
			{ "short rolls (dist 0, 0.15 s, tumble 0)", 40, 0.8f, 60, false, [](DiceConfig& c) { c.rollDistMin = 0; c.rollDistMax = 0; c.rollDuration = 0.15f; c.tumblePerCell = 0; } },
			{ "no cooldown, big ring", 40, 0.8f, 60, false, [](DiceConfig& c) { c.cooldown = 0; c.hoverRadius = 6; } },
			{ "largest board", 80, 0.8f },
		};
		for (CollisionMode mode : { CollisionMode::Nudge, CollisionMode::Bump })
			for (Scenario sc : scenarios)
			{
				sc.mode = mode;
				for (uint64_t seed : { 1ull, 2ull }) RunScenario(sc, seed);
			}
		// Bump mode pushed hard.
		const Scenario bumpOnly[] = {
			{ "always push, chains up to 10, 200 tokens/s", 40, 1.0f, 60, false, [](DiceConfig& c) { c.bounceChance = 0; c.bumpChainLimit = 10; c.bumpsPerSecond = 200; c.hoverRadius = 4; } },
			{ "always bounce off", 40, 1.0f, 60, false, [](DiceConfig& c) { c.bounceChance = 1; c.hoverRadius = 4; } },
			{ "push chains, 1 token/s", 40, 1.0f, 60, false, [](DiceConfig& c) { c.bounceChance = 0; c.bumpChainLimit = 10; c.bumpsPerSecond = 1; } },
			{ "small full board, chains 10, no cooldown", 6, 1.0f, 60, false, [](DiceConfig& c) { c.bounceChance = 0.3f; c.bumpChainLimit = 10; c.bumpsPerSecond = 200; c.cooldown = 0; } },
			{ "pushes with clashes and glancing off", 40, 1.0f, 60, false, [](DiceConfig& c) { c.clashEnabled = false; c.midAirCollide = false; c.bumpChainLimit = 5; } },
		};
		for (Scenario sc : bumpOnly)
		{
			sc.mode = CollisionMode::Bump;
			for (uint64_t seed : { 1ull, 2ull, 3ull }) RunScenario(sc, seed);
		}
	}

	// B10.2 / B10.3 on single rolls: a die launched one cell at a neighbour's slot.
	void TestBumps()
	{
		int pushes = 0, bounces = 0;
		bool pushTiming = true, pushAway = true, pushTokens = true, bounceStays = true, bounceTiming = true, bounceFree = true;
		for (uint64_t seed = 1; seed <= 300; ++seed)
		{
			DiceSim sim;
			sim.rand.Seed(seed);
			sim.config.boardSize = 12;
			sim.config.density = 1.0f;
			sim.config.collisionMode = CollisionMode::Bump;
			sim.config.clashEnabled = false;
			sim.config.bounceChance = seed % 2 ? 0.0f : 1.0f;
			sim.config.durationJitter = 0;
			sim.BuildBoard();
			const int from = 5 * 12 + 5, to = from + 1;   // the next slot in +x
			const int i = sim.slotOwner[from], occ = sim.slotOwner[to];
			const float occX = sim.D.px[occ], occZ = sim.D.pz[occ];
			const double tokens = sim.bumpTokens;
			sim.Launch(i, 0.0, 1.0, 0, 0, 0, 0);
			const double R = sim.config.dieSize * sim.config.bumpContact;
			if (sim.config.bounceChance == 0)
			{
				// Push: the occupant waits (PENDING) until the moment of contact, then leaves.
				++pushes;
				pushTokens &= sim.bumpTokens <= tokens - 1 && sim.slotOwner[to] == i && sim.D.bumped[occ] == 1 && sim.D.chain[occ] == 1;
				const double tHit = sim.D.t0[occ];
				pushTiming &= sim.D.state[occ] == S_PENDING && tHit > sim.now;
				double x, z;
				sim.PathXZ(i, (tHit - sim.D.t0[i]) / sim.D.dur[i], x, z);
				pushTiming &= std::fabs(std::hypot(x - occX, z - occZ) - R) < 0.02;
				pushAway &= (sim.D.ex[occ] - occX) > 0.3;   // shoved on along +x, the way the hit came
			}
			else
			{
				// Bounce: the occupant stays (it wobbles at the hit), the incoming die ricochets to a free slot.
				++bounces;
				bounceStays &= sim.D.state[occ] == S_READY && sim.slotOwner[to] == occ && sim.D.hitT[occ] > sim.now;
				const int landing = sim.D.cell[i];
				bounceFree &= landing != to && sim.slotOwner[landing] == i && sim.D.bu[i] > 0;
				double x, z;
				sim.PathXZ(i, sim.D.bu[i], x, z);
				bounceTiming &= std::fabs(std::hypot(x - occX, z - occZ) - R) < 0.02
					&& std::fabs(sim.D.t0[i] + sim.D.bu[i] * sim.D.dur[i] - sim.D.hitT[occ]) < 1e-4;
			}
			// Either way everything lands and the invariants hold.
			for (int k = 0; k < 120; ++k) sim.Frame(1.0 / 60, false, 0, 0);
			pushTiming &= Invariants(sim).empty() && sim.activeCount == 0;
		}
		Check(pushTokens, "push (" + std::to_string(pushes) + " rolls): spends a token (more if the pushed die pushes on), takes the slot, the occupant is pushed (chain 1)");
		Check(pushTiming, "push: the occupant starts moving at the moment of contact (dieSize x bumpContact apart)");
		Check(pushAway, "push: the occupant goes on in the direction of the hit");
		Check(bounceStays, "bounce-off (" + std::to_string(bounces) + " rolls): the occupant stays in its slot and wobbles at the hit");
		Check(bounceTiming, "bounce-off: the incoming die turns at the contact point, at the moment of the hit");
		Check(bounceFree, "bounce-off: the incoming die lands in a free slot of its own");
	}

	// A launch starts where the die is, as it is (no pop), and lands smoothly on the plan.
	void TestRollContinuity()
	{
		bool startOk = true, endOk = true, heightOk = true;
		for (uint64_t seed = 1; seed <= 200; ++seed)
		{
			DiceSim sim;
			sim.rand.Seed(seed);
			sim.config.boardSize = 12;
			sim.BuildBoard();
			const int i = sim.slotOwner[sim.slotCount / 2] >= 0 ? sim.slotOwner[sim.slotCount / 2] : 0;
			const Quat q0 = GetQ(sim.D.q.data(), i);
			const float x0 = sim.D.px[i], z0 = sim.D.pz[i];
			sim.Launch(i, sim.rand() * kPi * 2, sim.rand.Range(1, 3), 0, 0, 0, 0);
			const double dur = sim.D.dur[i];
			sim.now += dur * 1e-4;
			sim.UpdateRolls(dur * 1e-4);
			const Quat qa = GetQ(sim.D.q.data(), i);
			startOk &= std::fabs(std::fabs(qa.x * q0.x + qa.y * q0.y + qa.z * q0.z + qa.w * q0.w) - 1) < 1e-4
				&& std::hypot(sim.D.px[i] - x0, sim.D.pz[i] - z0) < 1e-3;
			double minY = 1e9;
			for (int k = 1; k < 100; ++k)
			{
				sim.now = sim.D.t0[i] + dur * k / 100.0;
				sim.UpdateRolls(0.0);
				minY = std::min(minY, static_cast<double>(sim.D.py[i]));
			}
			heightOk &= minY >= -1e-4;
			sim.now = sim.D.t0[i] + dur * 0.9999;
			sim.UpdateRolls(0.0);
			const Quat qb = GetQ(sim.D.q.data(), i), qe = GetQ(sim.D.qe.data(), i);
			endOk &= std::fabs(std::fabs(qb.x * qe.x + qb.y * qe.y + qb.z * qe.z + qb.w * qe.w) - 1) < 1e-4
				&& std::hypot(sim.D.px[i] - sim.D.ex[i], sim.D.pz[i] - sim.D.ez[i]) < 1e-3;
		}
		Check(startOk, "a launch starts from the die's own position and rotation (200 rolls)");
		Check(endOk, "a roll arrives at its planned rotation and end point without a jump");
		Check(heightOk, "a rolling die never dips below the board");
	}

	// B11.7: luck 0 is a fair die; luck 1 matches sum(f w_f) / sum(w_f), sixes ~400x ones.
	void TestLuck()
	{
		DiceSim sim;
		sim.rand.Seed(42);
		const int n = 120000;
		int counts[7] = {};
		for (int k = 0; k < n; ++k) ++counts[sim.PickFace()];
		bool fair = true;
		for (int f = 1; f <= 6; ++f) fair &= std::fabs(counts[f] / static_cast<double>(n) - 1.0 / 6) < 0.006;
		Check(fair && Near(sim.ExpectedFace(), 3.5, 1e-9), "luck 0: every face within 0.6% of 1/6, expected face 3.5");

		sim.config.luck = 1.0f;
		int lucky[7] = {};
		double sum = 0;
		for (int k = 0; k < n; ++k) { const int f = sim.PickFace(); ++lucky[f]; sum += f; }
		double w[7], total;
		sim.FaceWeights(w, total);
		Check(std::fabs(sum / n - sim.ExpectedFace()) < 0.01, "luck 1: the mean face " + std::to_string(sum / n) + " matches the expected " + std::to_string(sim.ExpectedFace()));
		Check(Near(w[6] / w[1], std::exp(6.0), 1e-6) && lucky[6] > 300 * std::max(lucky[1], 1), "luck 1: a six is about 400 times as likely as a one");
	}

	// Sum of the points in this frame's counting popups: landings ("+pts", "+pts x1.5"),
	// clashes ("CLASH +25") and topples ("TOPPLE xn +bonus").
	long long CountedPoints(const DiceSim& sim)
	{
		long long sum = 0;
		for (const PopupRequest& p : sim.popupsOut)
		{
			if (p.style & Pop_Free) continue;
			const size_t plus = p.text.find('+');
			if (plus != std::string::npos) sum += std::stoll(p.text.substr(plus + 1));
		}
		return sum;
	}

	int SessionDiceInAir(const DiceSim& sim)
	{
		int n = 0;
		for (int k = 0; k < sim.activeCount; ++k) n += sim.D.session[sim.active[k]] == sim.sessionId;
		return n;
	}

	// M4 done-when: a 30 s session works and late landings count (B15 "Session phases").
	void TestSession()
	{
		DiceSim sim;
		sim.rand.Seed(21);
		sim.config.boardSize = 20;
		sim.config.hoverRadius = 2.5f;
		sim.BuildBoard();
		const double dt = 1.0 / 60;
		double t = 0;
		auto pointer = [&](double time, double& x, double& z) { x = 8 * std::sin(time * 1.1); z = 8 * std::sin(time * 0.7); };

		// Free play first: rolls happen, nothing scores, popups are grey.
		bool freeOk = true;
		long long freeLandings = 0;
		for (int k = 0; k < 120; ++k)
		{
			double x, z;
			pointer(t += dt, x, z);
			sim.Frame(dt, true, x, z);
			for (const PopupRequest& pop : sim.popupsOut) { freeOk &= (pop.style & Pop_Free) != 0; ++freeLandings; }
			sim.popupsOut.clear();
		}
		Check(freeOk && freeLandings > 0 && sim.score == 0, "free play: " + std::to_string(freeLandings) + " landings, none scores, every popup grey");

		sim.StartSession();
		Check(sim.phase == Phase::Running && sim.timeLeft == 30 && sim.score == 0, "Start: running, 30.0 s on the clock, score 0");
		long long expected = 0, lateLandings = 0, lateRollsWhileFinishing = 0;
		bool airborneOk = true, scoreOk = true;
		long long sessionRolls = 0;
		double finishingAt = -1, endedAt = -1;
		while (sim.phase != Phase::Ended && t < 1000)
		{
			double x, z;
			pointer(t += dt, x, z);
			const long long rollsBefore = sim.stats.rolls;
			const Phase before = sim.phase;
			sim.Frame(dt, true, x, z);   // the pointer keeps moving the whole time
			// (A frame that ends the session rolls free-play dice again after updateSession.)
			if (before == Phase::Finishing && sim.phase == Phase::Finishing) lateRollsWhileFinishing += sim.stats.rolls - rollsBefore;
			if (sim.phase == Phase::Running) sessionRolls += sim.stats.rolls - rollsBefore;
			if (sim.phase == Phase::Finishing && finishingAt < 0) finishingAt = t;
			if (sim.phase == Phase::Ended && endedAt < 0) endedAt = t;
			const long long points = CountedPoints(sim);
			if (before == Phase::Finishing && points > 0) ++lateLandings;
			expected += points;
			scoreOk &= sim.score == expected;
			airborneOk &= sim.phase == Phase::Ended || sim.sessionAirborne == SessionDiceInAir(sim);
			sim.popupsOut.clear();
		}
		Check(finishingAt > 0 && std::fabs((finishingAt - (t - (endedAt - finishingAt))) ) >= 0 && sim.timeLeft == 0,
			"the clock runs out after 30 s (finishing at " + std::to_string(finishingAt - 2.0) + " s of play)");
		Check(lateRollsWhileFinishing == 0, "while finishing, hovering launches nothing");
		Check(lateLandings > 0, "late landings count: " + std::to_string(lateLandings) + " frames scored after the time ran out");
		Check(scoreOk && sim.score > 0, "the score is the sum of the counted landings (" + std::to_string(sim.score) + ")");
		Check(airborneOk, "sessionAirborne always equals the session's dice in the air");
		Check(sim.phase == Phase::Ended && endedAt - finishingAt < 1.0, "the session ends once its last die lands (" + std::to_string(endedAt - finishingAt) + " s after the time)");
		Check(sim.sStats.rolls == sessionRolls && sessionRolls > 0, "the end card counts the session's rolls (" + std::to_string(sim.sStats.rolls) + ")");

		// Restart while dice are in the air: the old ones never count for the new session.
		sim.StartSession();
		for (int k = 0; k < 30; ++k) { double x, z; pointer(t += dt, x, z); sim.Frame(dt, true, x, z); sim.popupsOut.clear(); }
		const int oldSession = sim.sessionId;
		sim.StartSession();
		bool restartOk = sim.score == 0 && sim.sessionAirborne == 0;
		long long counted = 0;
		for (int k = 0; k < 60; ++k)
		{
			sim.Frame(dt, false, 0, 0);
			for (const PopupRequest& pop : sim.popupsOut) counted += !(pop.style & Pop_Free);
			sim.popupsOut.clear();
		}
		restartOk &= counted == 0 && sim.score == 0 && sim.sessionAirborne == 0 && sim.sessionId == oldSession + 1;
		Check(restartOk, "Restart mid-session: dice still in the air from before land as free play, score stays 0");
	}

	void TestSounds()
	{
		const std::vector<float> noise = synth::NoiseBuffer(1);
		const std::vector<float> clack = synth::Clack(noise);
		const std::vector<float> knock = synth::NoiseHit(noise, 1700, 4, 0.45, 0.05);
		const std::vector<float> tick = synth::NoiseHit(noise, 1500, 5, 0.12, 0.035);
		auto peak = [](const std::vector<float>& s, size_t from, size_t to)
		{
			float p = 0;
			for (size_t n = from; n < std::min(to, s.size()); ++n) p = std::max(p, std::fabs(s[n]));
			return p;
		};
		const int sr = synth::kSampleRate;
		Check(clack.size() == static_cast<size_t>(sr / 10) && knock.size() == static_cast<size_t>(sr * 0.07) && tick.size() == static_cast<size_t>(sr * 0.055),
			"sound lengths: clack 100 ms, knock 70 ms, tick 55 ms (decay + 20 ms)");
		Check(peak(clack, 0, sr / 100) > 0.1f && peak(clack, 0, clack.size()) < 1.5f && peak(clack, sr * 9 / 100, clack.size()) < 0.01f,
			"the clack is loud at once and has died away by 90 ms");
		Check(peak(knock, 0, sr / 100) > peak(tick, 0, sr / 100) && peak(tick, sr * 4 / 100, tick.size()) < 0.001f,
			"a knock is louder than a tick, and the tick is silent after 40 ms");
	}

	// Airborne pairs overlapping "deeply": centres closer than `fraction` x dieSize across the
	// board while also overlapping in height (|dy| < 0.9 x dieSize, B10.7's test).
	struct Overlap { int pairs = 0; double closest = 1e9; };
	Overlap AirborneOverlap(const DiceSim& sim, double fraction)
	{
		Overlap o;
		const double size = sim.config.dieSize;
		for (int a = 0; a < sim.activeCount; ++a)
		{
			const int i = sim.active[a];
			if (sim.D.state[i] != S_ROLLING) continue;
			for (int b = a + 1; b < sim.activeCount; ++b)
			{
				const int j = sim.active[b];
				if (sim.D.state[j] != S_ROLLING || std::fabs(sim.D.py[i] - sim.D.py[j]) >= 0.9 * size) continue;
				const double d = std::hypot(sim.D.px[i] - sim.D.px[j], sim.D.pz[i] - sim.D.pz[j]);
				o.closest = std::min(o.closest, d);
				if (d < fraction * size) ++o.pairs;
			}
		}
		return o;
	}

	struct MidAirRun { long long deepPairFrames = 0, touchingPairFrames = 0, frames = 0; double closest = 1e9; long long clashes = 0; std::string broken; };
	MidAirRun RunMidAir(bool collide, bool clash, uint64_t seed)
	{
		DiceSim sim;
		sim.rand.Seed(seed);
		sim.config.boardSize = 30;
		sim.config.density = 0.9f;
		sim.config.hoverRadius = 4.0f;
		sim.config.cooldown = 0.2f;
		sim.config.midAirCollide = collide;
		sim.config.clashEnabled = clash;
		sim.BuildBoard();
		MidAirRun run;
		const double dt = 1.0 / 60;
		for (double t = 0; t < 10.0 && run.broken.empty(); t += dt)
		{
			const double x = 11 * std::sin(t * 1.7 + seed), z = 11 * std::sin(t * 1.1 + seed * 0.3);
			sim.Frame(dt, t < 8.0, x, z);
			run.broken = Invariants(sim);
			const Overlap deep = AirborneOverlap(sim, 0.5), touching = AirborneOverlap(sim, 0.8);
			run.deepPairFrames += deep.pairs;
			run.touchingPairFrames += touching.pairs;
			run.closest = std::min(run.closest, deep.closest);
			++run.frames;
		}
		run.clashes = sim.stats.clashes;
		if (sim.activeCount != 0 && run.broken.empty()) run.broken = "dice still in the air 2 s after the pointer left";
		return run;
	}

	// M5 done-when: no deep overlaps of airborne dice (B10.6..B10.8).
	void TestMidAir()
	{
		MidAirRun on, off;
		for (uint64_t seed : { 1ull, 2ull, 3ull })
		{
			const MidAirRun a = RunMidAir(true, true, seed), b = RunMidAir(false, false, seed);
			on.deepPairFrames += a.deepPairFrames; on.touchingPairFrames += a.touchingPairFrames; on.closest = std::min(on.closest, a.closest); on.clashes += a.clashes;
			off.deepPairFrames += b.deepPairFrames; off.touchingPairFrames += b.touchingPairFrames; off.closest = std::min(off.closest, b.closest);
			if (on.broken.empty()) on.broken = a.broken;
		}
		std::printf("       airborne pairs closer than 0.5 die (pair-frames): %lld with clash + glancing, %lld with both off; closest %.3f vs %.3f\n",
			on.deepPairFrames, off.deepPairFrames, on.closest, off.closest);
		std::printf("       ... closer than 0.8 die: %lld vs %lld; clashes %lld\n", on.touchingPairFrames, off.touchingPairFrames, on.clashes);
		Check(on.broken.empty(), "a crowded board with clashes and glancing keeps the invariants and lands everything" + (on.broken.empty() ? "" : ": " + on.broken));
		Check(on.clashes > 0, "clashes happen (" + std::to_string(on.clashes) + ")");
		Check(on.deepPairFrames == 0, "no airborne dice overlap deeply (centres under half a die apart) with separation on");
		Check(off.deepPairFrames > 0, "... while without it they do (" + std::to_string(off.deepPairFrames) + " pair-frames): the check can see them");
	}


	const char* LayoutName(Layout l) { return l == Layout::Hex ? "hex" : l == Layout::Scatter ? "scatter" : "square"; }
	const char* ShapeName(BoardShape b) { return b == BoardShape::Circle ? "circle" : b == BoardShape::Ring ? "ring" : b == BoardShape::Islands ? "islands" : "full"; }

	// B4: the layouts and shapes make the slots they should.
	void TestLayouts()
	{
		DiceSim sim;
		sim.rand.Seed(17);
		sim.config.boardSize = 40;
		sim.config.layout = Layout::Square;
		sim.BuildBoard();
		Check(sim.slotCount == 1600, "square, full: N x N slots");

		// Hex: offset rows sqrt(3)/2 apart, every slot's nearest neighbour exactly 1 cell away.
		sim.config.layout = Layout::Hex;
		sim.BuildBoard();
		const int rows = static_cast<int>(std::floor(39 / (std::sqrt(3.0) / 2))) + 1;
		const int hexCount = (rows + 1) / 2 * 40 + rows / 2 * 39;
		bool unit = true;
		for (int s = 0; s < sim.slotCount; ++s)
		{
			double best = 1e9;
			for (int t = 0; t < sim.slotCount; ++t)
				if (t != s) best = std::min(best, std::hypot(static_cast<double>(sim.slotX[s] - sim.slotX[t]), static_cast<double>(sim.slotZ[s] - sim.slotZ[t])));
			unit &= std::fabs(best - 1.0) < 1e-4;
		}
		Check(sim.slotCount == hexCount && unit, "hex, full: " + std::to_string(sim.slotCount) + " slots in " + std::to_string(rows) + " offset rows, every nearest neighbour 1 cell away");

		// Scatter: never closer than max(0.7, spacing), inside the margin, and maximal (any point
		// of the area has a slot within 2 x spacing).
		for (float spacing : { 0.7f, 0.95f, 2.0f })
		{
			sim.config.layout = Layout::Scatter;
			sim.config.scatterSpacing = spacing;
			sim.BuildBoard();
			const double minD = std::max(0.7, static_cast<double>(spacing)), lo = -20 + 0.4, hi = 20 - 0.4;
			bool apart = true, inside = true, dense = true;
			for (int s = 0; s < sim.slotCount; ++s)
			{
				inside &= sim.slotX[s] >= lo - 1e-5 && sim.slotX[s] <= hi + 1e-5 && sim.slotZ[s] >= lo - 1e-5 && sim.slotZ[s] <= hi + 1e-5;
				for (int t = s + 1; t < sim.slotCount; ++t)
					apart &= std::hypot(static_cast<double>(sim.slotX[s] - sim.slotX[t]), static_cast<double>(sim.slotZ[s] - sim.slotZ[t])) >= minD - 1e-4;
			}
			Random pick(3);
			for (int k = 0; k < 2000; ++k)
			{
				const double x = pick.Range(lo, hi), z = pick.Range(lo, hi);
				const int s = sim.NearestSlot(x, z, false);
				dense &= std::hypot(sim.slotX[s] - x, sim.slotZ[s] - z) <= 2 * minD + 1e-4;
			}
			char text[160];
			std::snprintf(text, sizeof(text), "scatter %.2f: %d slots, none closer than %.2f, all inside the margin, no gap wider than 2 x spacing", spacing, sim.slotCount, minD);
			Check(apart && inside && dense, text);
		}

		// Shapes filter the points (R = N/2 - 0.3).
		for (Layout layout : { Layout::Square, Layout::Hex, Layout::Scatter })
			for (BoardShape shape : { BoardShape::Circle, BoardShape::Ring, BoardShape::Islands })
			{
				sim.config.layout = layout;
				sim.config.boardShape = shape;
				sim.config.scatterSpacing = 0.95f;
				sim.BuildBoard();
				const double R = 20 - 0.3;
				bool ok = sim.slotCount > 0;
				for (int s = 0; s < sim.slotCount; ++s)
				{
					const double x = sim.slotX[s], z = sim.slotZ[s], d = std::hypot(x, z);
					if (shape == BoardShape::Circle) ok &= d <= R + 1e-4;
					if (shape == BoardShape::Ring) ok &= d <= R + 1e-4 && d >= 0.45 * R - 1e-4;
					if (shape == BoardShape::Islands)
					{
						bool in = false;
						for (const auto& island : DiceSim::kIslands) in |= std::hypot(x - island[0] * 40, z - island[1] * 40) <= island[2] * 40 + 1e-4;
						ok &= in;
					}
				}
				Check(ok, std::string(LayoutName(layout)) + ", " + ShapeName(shape) + ": " + std::to_string(sim.slotCount) + " slots, all inside the shape");
			}

		// The biggest board fits the capacity (hex packs the most).
		sim.config.boardSize = 80;
		sim.config.boardShape = BoardShape::Full;
		sim.config.layout = Layout::Hex;
		sim.BuildBoard();
		const int hex80 = sim.slotCount;
		sim.config.layout = Layout::Scatter;
		sim.config.scatterSpacing = 0.7f;
		sim.BuildBoard();
		Check(hex80 < sim.Capacity() && sim.slotCount < sim.Capacity() && sim.Capacity() == CapacityFor(80),
			"80 x 80: hex " + std::to_string(hex80) + " and densest scatter " + std::to_string(sim.slotCount) + " slots fit the " + std::to_string(sim.Capacity()) + " capacity");
		sim.config.boardSize = 317;
		sim.config.layout = Layout::Square;
		sim.config.density = 1.0f;
		sim.BuildBoard();
		Check(sim.diceCount == 317 * 317 && Invariants(sim).empty(), "a 317 x 317 profiling board deals " + std::to_string(sim.diceCount) + " dice (the arrays grow to fit)");

		// B13.5: a rebuild while dice are flying leaves nothing behind.
		sim.config.boardSize = 20;
		sim.config.layout = Layout::Square;
		sim.config.collisionMode = CollisionMode::Bump;
		sim.BuildBoard();
		for (int k = 0; k < 30; ++k) sim.Frame(1.0 / 60, true, 2 * std::sin(k * 0.2), 0);
		const bool busy = sim.activeCount > 0;
		sim.config.layout = Layout::Hex;
		sim.BuildBoard();
		Check(busy && sim.activeCount == 0 && sim.events.empty() && sim.popupsOut.empty() && sim.sparksOut.empty() && Invariants(sim).empty(),
			"rebuilding mid-roll clears the active list, events and effects; the new board is consistent");
	}

	// M7 done-when: all 24 combinations (3 layouts x 4 shapes x nudge / bump) pass the invariants.
	void TestAllLayouts()
	{
		for (Layout layout : { Layout::Square, Layout::Hex, Layout::Scatter })
			for (BoardShape shape : { BoardShape::Full, BoardShape::Circle, BoardShape::Ring, BoardShape::Islands })
				for (CollisionMode mode : { CollisionMode::Nudge, CollisionMode::Bump, CollisionMode::Stack })
				{
					Scenario sc;
					sc.name = std::string(LayoutName(layout)) + " " + ShapeName(shape);
					sc.layout = layout;
					sc.shape = shape;
					sc.mode = mode;
					for (uint64_t seed : { 1ull, 2ull }) RunScenario(sc, seed);
				}
	}

	// M8 done-when: the invariants hold on packed boards with topples (stackMax 2..6).
	void TestStackInvariants()
	{
		long long totalTopples = 0;
		bool everyHeightTopples = true;
		for (int stackMax = 2; stackMax <= 6; ++stackMax)
		{
			long long heightTopples = 0;
			for (float density : { 1.0f, 0.9f })
			{
				Scenario sc;
				sc.mode = CollisionMode::Stack;
				sc.density = density;
				sc.name = "packed board, stackMax " + std::to_string(stackMax) + ", density " + (density == 1.0f ? std::string("1") : std::string("0.9"));
				static int sStackMax;
				sStackMax = stackMax;
				sc.tune = [](DiceConfig& c) { c.stackMax = sStackMax; c.stackChance = 1.0f; c.hoverRadius = 1.5f; c.bumpChainLimit = 4; c.bumpsPerSecond = 200; };
				sc.hopping = true;
				for (uint64_t seed : { 1ull, 2ull })
				{
					RunScenario(sc, seed);
					totalTopples += lastTopples;
					heightTopples += lastTopples;
				}
			}
			// The hover relaunches tops, so full stacks get rarer as they get taller (none above 4 here).
			if (stackMax <= 4) everyHeightTopples &= heightTopples > 0;
		}
		Scenario dflt;
		dflt.mode = CollisionMode::Stack;
		dflt.density = 1.0f;
		dflt.name = "packed board, default stack settings, hopping pointer";
		dflt.hopping = true;
		for (uint64_t seed : { 1ull, 2ull, 3ull }) { RunScenario(dflt, seed); totalTopples += lastTopples; }
		Check(everyHeightTopples && totalTopples > 0, "packed boards topple at every stackMax from 2 to 4 (" + std::to_string(totalTopples) + " topples in all)");
	}

	// B10.4, B10.5 and the stack rules on single rolls.
	void TestStackRules()
	{
		bool stacked = true, scored = true, topOnly = true, uncover = true, toppled = true, toppleScore = true;
		for (uint64_t seed = 1; seed <= 100; ++seed)
		{
			DiceSim sim;
			sim.rand.Seed(seed);
			sim.config.boardSize = 12;
			sim.config.density = 1.0f;
			sim.config.collisionMode = CollisionMode::Stack;
			sim.config.stackMax = 2;
			sim.config.stackChance = 1.0f;
			sim.config.clashEnabled = false;
			sim.config.midAirCollide = false;
			sim.config.durationJitter = 0;
			sim.config.hoverRadius = 0.3f;
			sim.BuildBoard();
			sim.StartSession();
			const int from = 5 * 12 + 5, to = from + 1, other = from - 1;
			const int i = sim.slotOwner[from], T = sim.slotOwner[to];

			// Land on top: level 1, resting at dieSize, scored x1.5 with a stack popup.
			sim.Launch(i, 0.0, 1.0, 0, 0, 0, sim.sessionId);
			sim.popupsOut.clear();
			for (int k = 0; k < 60 && sim.D.state[i] == S_ROLLING; ++k) sim.Frame(1.0 / 60, false, 0, 0);
			stacked &= sim.slotOwner[to] == i && sim.D.below[i] == T && sim.D.level[i] == 1 && std::fabs(sim.D.py[i] - sim.config.dieSize) < 1e-5;
			bool found = false;
			for (const PopupRequest& pop : sim.popupsOut)
			{
				const long long want = std::llround(sim.D.face[i] * 10 * 1.5);
				if ((pop.style & Pop_Stack) && pop.text == "+" + std::to_string(want) + " \xC3\x97" "1.5") found = true;
			}
			scored &= found;

			// A full stack (stackMax 2): the die from the left topples the top one off.
			for (int k = 0; k < 60; ++k) sim.Frame(1.0 / 60, false, 0, 0);   // all cooled down again
			const int j = sim.slotOwner[other];
			sim.popupsOut.clear();
			const long long scoreBefore = sim.score;
			sim.Launch(j, 0.0, 2.0, 0, 0, 0, sim.sessionId);
			const bool pending = sim.D.state[i] == S_PENDING && sim.D.bumped[i] == 1 && sim.slotOwner[to] == j && sim.D.below[j] == T;
			bool topplePopup = false;
			for (int k = 0; k < 90; ++k)
			{
				sim.Frame(1.0 / 60, false, 0, 0);
				for (const PopupRequest& pop : sim.popupsOut) topplePopup |= (pop.style & Pop_Topple) && pop.text == "TOPPLE \xC3\x97" "1 +15";
				sim.popupsOut.clear();
			}
			// (The knocked-off die may land on another stack: stack mode with stackChance 1.)
			toppled &= pending && sim.stats.topples == 1 && sim.slotOwner[to] == j && sim.D.level[j] == 1 && sim.D.below[j] == T
				&& sim.D.cell[i] != to && Invariants(sim).empty();
			toppleScore &= topplePopup && sim.score > scoreBefore + 15;

			// Top-only hover and the uncover cooldown: hovering the stack launches only its top,
			// and the die uncovered gets a fresh cooldown.
			for (int k = 0; k < 60; ++k) sim.Frame(1.0 / 60, false, 0, 0);
			const int top = sim.slotOwner[to], under = sim.D.below[top];
			const bool ready = sim.D.state[top] == S_READY && sim.D.state[under] == S_READY;
			sim.Frame(1.0 / 60, true, sim.slotX[to], sim.slotZ[to]);
			topOnly &= ready && sim.D.state[top] == S_ROLLING && sim.D.state[under] != S_ROLLING && sim.slotOwner[to] != top;
			uncover &= sim.D.state[under] == S_COOLDOWN && std::fabs(sim.D.timer[under] - (sim.now + sim.config.cooldown)) < 1e-9;
		}
		Check(stacked, "stack: a die rolled onto a resting one lands on top (level 1, height dieSize, it becomes the slot's top)");
		Check(scored, "stack: the landing scores x1.5 and says so (\"+N x1.5\", stack style)");
		Check(toppled, "topple: a die hitting a full stack knocks the upper die off at contact and settles on the bottom one");
		Check(toppleScore, "topple: \"TOPPLE x1 +15\" and the bonus counts in the session");
		Check(topOnly, "hovering a stack launches only its top die");
		Check(uncover, "the die it uncovers gets a fresh cooldown (no peeling a stack in one sweep)");
	}

	// ---- M9: the config as data, the panel, presets --------------------------------------
	// B12 in its order, with its defaults (strings for the enums).
	struct Expected { const char* key; double value; const char* name; };
	const Expected kB12[] = {
		{ "layout", 0, "square" }, { "boardShape", 0, "full" }, { "boardSize", 40 }, { "scatterSpacing", 0.95 }, { "density", 0.8 },
		{ "dieSize", 0.62 }, { "slotJitter", 0.12 }, { "hoverRadius", 1.5 }, { "rollDistMin", 1 }, { "rollDistMax", 3 },
		{ "rollDuration", 0.5 }, { "durationJitter", 0.15 }, { "hopHeight", 0.9 }, { "tumblePerCell", 1.0 }, { "squash", 0.3 },
		{ "cooldown", 0.6 }, { "readyGlowTime", 0.45 }, { "luck", 0 }, { "collisionMode", 0, "nudge" }, { "stackMax", 3 },
		{ "stackChance", 0.75 }, { "stackScorePerLevel", 0.5 }, { "toppleBonus", 15 }, { "toppleSpreadDeg", 70 }, { "toppleDistMin", 1 },
		{ "toppleDistMax", 2.2 }, { "bumpChainLimit", 2 }, { "bumpsPerSecond", 40 }, { "bumpDistMin", 1 }, { "bumpDistMax", 1.6 },
		{ "bumpSpreadDeg", 20 }, { "bumpScoreMult", 0.5 }, { "bumpContact", 1.15 }, { "bumpNormalWeight", 0.6 }, { "bumpHopMult", 1.0 },
		{ "bumpDurationMult", 0.8 }, { "bumpRecoil", 0.12 }, { "bounceChance", 0.5 }, { "bounceDistMin", 0.8 }, { "bounceDistMax", 1.5 },
		{ "bounceSpreadDeg", 25 }, { "bounceTime", 0.3 }, { "bounceHopMult", 0.35 }, { "hitWobble", 0.14 }, { "clashEnabled", 1 },
		{ "clashDistance", 0.55 }, { "clashDeflect", 1.0 }, { "clashBonus", 25 }, { "midAirCollide", 1 }, { "midAirContact", 0.95 },
		{ "midAirDeflect", 0.4 }, { "midAirMaxHits", 2 }, { "pointsPerPip", 10 }, { "sessionSeconds", 30 }, { "volume", 0.5 },
		{ "landTicks", 1 },
	};

	double ValueOf(const DiceConfig& c, const Field& f)
	{
		if (f.kind) return GetEnum(c, f.kind);
		if (f.f) return c.*f.f;
		if (f.i) return c.*f.i;
		return c.*f.b ? 1 : 0;
	}

	void TestConfigData()
	{
		const std::vector<Field>& fields = Fields();
		bool sameNames = fields.size() == std::size(kB12), defaults = true;
		const DiceConfig c;
		for (size_t k = 0; k < std::min(fields.size(), std::size(kB12)); ++k)
		{
			sameNames &= std::string(fields[k].key) == kB12[k].key;
			if (fields[k].kind) defaults &= std::string(EnumName(fields[k].kind, GetEnum(c, fields[k].kind))) == kB12[k].name;
			else defaults &= std::fabs(ValueOf(c, fields[k]) - kB12[k].value) < 1e-6;
		}
		Check(sameNames, "the config has exactly B12's " + std::to_string(std::size(kB12)) + " tunables, by its names, in its order");
		Check(defaults, "every tunable defaults to B12's value");

		// Every field changed, through JSON and back.
		DiceConfig changed;
		Random pick(4);
		for (const Field& f : fields)
		{
			if (f.kind) SetEnum(changed, f.kind, EnumCount(f.kind) - 1);
			else if (f.f) changed.*f.f = static_cast<float>(pick.Range(0.1, 9.9));
			else if (f.i) changed.*f.i = 2 + static_cast<int>(pick() * 7);
			else changed.*f.b = !(changed.*f.b);
		}
		const std::string text = ConfigToJson(changed);
		DiceConfig back;
		std::vector<std::string> problems;
		bool roundTrip = ConfigFromJson(text, back, problems) && problems.empty();
		for (const Field& f : fields) roundTrip &= ValueOf(back, f) == ValueOf(changed, f);
		Check(roundTrip, "every tunable survives JSON and back (presets, hot reload)");
		bool named = true;
		for (const Expected& e : kB12) named &= text.find(std::string("\"") + e.key + "\"") != std::string::npos;
		Check(named && text.find("\"square\"") == std::string::npos && text.find("\"scatter\"") != std::string::npos, "the JSON uses B12's names and the enums' strings");

		DiceConfig partial;
		partial.hoverRadius = 2.5f;
		problems.clear();
		const bool ok = ConfigFromJson(R"({ "luck": 1, "wobble": 3, "layout": "triangle", "landTicks": 1 })", partial, problems);
		Check(ok && partial.luck == 1.0f && partial.hoverRadius == 2.5f && partial.layout == Layout::Square && partial.landTicks && problems.size() == 3,
			"a partial preset changes only its keys; unknown keys and bad values are reported, not applied");
	}

	// The panel is the reference's CONTROLS list: same keys, kinds, ranges, steps, rebuilds.
	void TestControls()
	{
		struct Want { const char* key; int kind; double min, max, step; bool rebuild; };
		const Want reference[] = {
			{ nullptr, Control::Header }, { "layout", Control::Select, 0, 0, 0, true }, { "boardShape", Control::Select, 0, 0, 0, true },
			{ "boardSize", Control::Range, 6, 80, 1, true }, { "scatterSpacing", Control::Range, 0.7, 2, 0.05, true }, { "density", Control::Range, 0.05, 1, 0.05, true },
			{ nullptr, Control::Header }, { "hoverRadius", Control::Range, 0.3, 6, 0.1 }, { "rollDistMin", Control::Range, 0, 8, 0.1 },
			{ "rollDistMax", Control::Range, 0, 8, 0.1 }, { "rollDuration", Control::Range, 0.15, 1.5, 0.05 }, { "hopHeight", Control::Range, 0, 3, 0.05 },
			{ "tumblePerCell", Control::Range, 0, 3, 0.25 }, { "squash", Control::Range, 0, 0.6, 0.02 }, { "cooldown", Control::Range, 0, 3, 0.05 },
			{ "luck", Control::Range, 0, 1, 0.05 }, { nullptr, Control::Header }, { "collisionMode", Control::Select },
			{ "stackMax", Control::Range, 2, 6, 1 }, { "stackChance", Control::Range, 0, 1, 0.05 }, { "bounceChance", Control::Range, 0, 1, 0.05 },
			{ "bounceDistMax", Control::Range, 0.5, 4, 0.1 }, { "bounceTime", Control::Range, 0.1, 0.8, 0.05 }, { "bumpChainLimit", Control::Range, 0, 10, 1 },
			{ "bumpsPerSecond", Control::Range, 1, 200, 1 }, { "bumpNormalWeight", Control::Range, 0, 1, 0.05 }, { "bumpSpreadDeg", Control::Range, 0, 90, 1 },
			{ "bumpHopMult", Control::Range, 0, 1.5, 0.05 }, { "bumpDistMax", Control::Range, 0.5, 5, 0.1 }, { "clashEnabled", Control::Check },
			{ "clashDistance", Control::Range, 0.1, 1.5, 0.05 }, { "midAirCollide", Control::Check }, { "midAirDeflect", Control::Range, 0, 1.2, 0.05 },
			{ nullptr, Control::Header }, { "volume", Control::Range, 0, 1, 0.05 }, { "landTicks", Control::Check },
		};
		const std::vector<Control>& controls = Controls();
		bool same = controls.size() == std::size(reference);
		for (size_t k = 0; k < std::min(controls.size(), std::size(reference)); ++k)
		{
			const Control& c = controls[k];
			const Want& w = reference[k];
			same &= static_cast<int>(c.kind) == w.kind && ((c.key == nullptr) == (w.key == nullptr)) && (!w.key || std::string(c.key) == w.key);
			if (c.kind == Control::Range) same &= Near(c.min, w.min, 1e-6) && Near(c.max, w.max, 1e-6) && Near(c.step, w.step, 1e-6);
			same &= c.rebuild == w.rebuild;
			if (c.key) same &= FindField(c.key) != nullptr;
			if (c.kind == Control::Select) same &= static_cast<int>(c.options.size()) == EnumCount(FindField(c.key)->kind);
		}
		Check(same, "the panel has the reference's " + std::to_string(std::size(reference)) + " controls (keys, kinds, ranges, steps, rebuilds), each bound to its tunable");
		Check(Near(SnapToStep(1.234, 0, 3, 0.25), 1.25) && Near(SnapToStep(0.33, 0.3, 6, 0.1), 0.3) && Near(SnapToStep(9, 0, 8, 0.1), 8)
			&& Near(SnapToStep(0.07, 0.05, 1, 0.05), 0.05) && Near(SnapToStep(77.6, 6, 80, 1), 78),
			"sliders snap like HTML ranges (min + k x step, clamped)");
	}

	void TestPresets()
	{
		bool all = true;
		int count = 0;
		for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(DICE_CONTENT_DIR) / "Presets"))
		{
			std::ifstream file(entry.path());
			std::stringstream text;
			text << file.rdbuf();
			DiceConfig config;
			std::vector<std::string> problems;
			const bool ok = ConfigFromJson(text.str(), config, problems) && problems.empty();
			if (!ok) std::printf("       %s: %s\n", entry.path().filename().string().c_str(), problems.empty() ? "not JSON" : problems.front().c_str());
			all &= ok;
			++count;
		}
		Check(all && count > 0, std::to_string(count) + " shipped presets load without problems");
	}

	// "Every B12 control works at runtime": while dice fly, every 20 frames one control is set to
	// a random value in its range (snapped to its step), the board rebuilt when the control says
	// so, as the panel does. Every control is hit many times; the invariants must hold.
	void TestControlsAtRuntime()
	{
		std::string broken;
		int changes = 0, rebuilds = 0;
		std::vector<int> hits(Controls().size(), 0);
		for (uint64_t seed : { 1ull, 2ull, 3ull })
		{
			DiceSim sim;
			sim.rand.Seed(seed);
			sim.config.boardSize = 16;
			sim.BuildBoard();
			int stackLimit = sim.config.stackMax;
			Random pick(seed * 13);
			const double h = 8;
			for (int frame = 0; frame < 60 * 40 && broken.empty(); ++frame)
			{
				if (frame % 20 == 0)
				{
					size_t k;
					do k = static_cast<size_t>(pick() * Controls().size()); while (Controls()[k].kind == Control::Header);
					const Control& c = Controls()[k];
					const Field* f = FindField(c.key);
					if (c.kind == Control::Select) SetEnum(sim.config, f->kind, static_cast<int>(pick() * EnumCount(f->kind)));
					else if (c.kind == Control::Check) sim.config.*f->b = pick() < 0.5;
					else
					{
						double value = SnapToStep(pick.Range(c.min, c.max), c.min, c.max, c.step);
						if (std::string(c.key) == "boardSize") value = std::min(value, 30.0);   // keep the fuzz quick
						if (f->f) sim.config.*f->f = static_cast<float>(value);
						else sim.config.*f->i = static_cast<int>(std::lround(value));
					}
					++hits[k];
					++changes;
					stackLimit = std::max(stackLimit, sim.config.stackMax);
					if (c.rebuild) { sim.BuildBoard(); stackLimit = sim.config.stackMax; ++rebuilds; }
				}
				const double t = frame / 60.0, hh = sim.N / 2.0 * 0.9;
				sim.Frame(1.0 / 60, frame % 600 < 500, hh * std::sin(t * 0.9 + seed), hh * std::sin(t * 1.3));
				broken = Invariants(sim, stackLimit);
				sim.popupsOut.clear(); sim.sparksOut.clear(); sim.soundsOut.clear();
			}
		}
		int least = 1 << 30;
		for (size_t k = 0; k < hits.size(); ++k) if (Controls()[k].kind != Control::Header) least = std::min(least, hits[k]);
		Check(broken.empty() && least > 0, std::to_string(changes) + " random control changes while rolling (" + std::to_string(rebuilds) + " rebuilds, every control at least "
			+ std::to_string(least) + " times): invariants hold" + (broken.empty() ? "" : ": " + broken));
	}
}

// ---- Upgrades (DiceUpgrades.h, not in the reference) -------------------------------------------
void TestUpgrades()
{
	// The shipped tree loads cleanly.
	UpgradeTree tree;
	std::vector<std::string> problems;
	std::ifstream file(std::string(DICE_CONTENT_DIR) + "/Upgrades.json", std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	const bool loaded = LoadUpgradeTree(text.str(), tree, problems);
	std::string listed;
	for (const std::string& problem : problems) listed += "\n    " + problem;
	Check(loaded && tree.nodes.size() >= 6, "upgrades: Content/Upgrades.json loads without problems (" + std::to_string(tree.nodes.size()) + " nodes)" + listed);

	// Prices: cost x growth^level, nothing past the last level.
	UpgradeNode node;
	node.levels = 3;
	node.cost = 100;
	node.costGrowth = 2;
	Check(NextCost(node, 0) == 100 && NextCost(node, 1) == 200 && NextCost(node, 2) == 400 && NextCost(node, 3) == 0,
		"upgrades: level n+1 costs cost x growth^n (100, 200, 400), a maxed node nothing");

	// States and buying, on a small tree: a -> b (needs a 2).
	UpgradeTree small;
	problems.clear();
	LoadUpgradeTree(R"({ "start": { "density": 0.3 }, "nodes": [
		{ "id": "a", "levels": 2, "cost": 10, "costGrowth": 2, "effects": [ { "stat": "density", "add": 0.25 } ] },
		{ "id": "b", "levels": 1, "cost": 50, "effects": [ { "stat": "goldShare", "add": 0.5 } ], "requires": [ { "id": "a", "level": 2 } ] } ] })",
		small, problems);
	Check(problems.empty(), "upgrades: a small tree parses");
	Progress progress;
	progress.coins = 25;
	const UpgradeNode& a = *small.Find("a");
	const UpgradeNode& b = *small.Find("b");
	Check(StateOf(small, progress, a) == UpgradeState::Buyable && StateOf(small, progress, b) == UpgradeState::Locked, "upgrades: a node with no requirement is buyable, one whose requirement isn't met is locked");
	Check(Buy(small, progress, "a") && progress.coins == 15 && LevelOf(progress, "a") == 1, "upgrades: buying spends the price and adds a level");
	Check(!Buy(small, progress, "b"), "upgrades: a locked node can't be bought");
	Check(StateOf(small, progress, a) == UpgradeState::TooExpensive && !Buy(small, progress, "a") && progress.coins == 15,
		"upgrades: too few coins for the next level (20): too expensive, nothing spent");
	progress.coins = 40;
	Check(Buy(small, progress, "a") && progress.coins == 20, "upgrades: the second level costs cost x growth (20)");
	Check(StateOf(small, progress, a) == UpgradeState::Maxed && StateOf(small, progress, b) == UpgradeState::TooExpensive && !Buy(small, progress, "b"),
		"upgrades: a node with every level is maxed; met requirement but too few coins: too expensive, not bought");
	std::map<std::string, double> stats = StatsFor(small, progress);
	Check(Near(stats["density"], 0.8) && Near(stats["goldShare"], 0) && Near(stats["luck"], 0) && Near(stats["sessionSeconds"], 30),
		"upgrades: stats are the start values plus each owned level's effects (density 0.3 + 2 x 0.25), the rest their defaults");
	progress.levels["a"] = 50;
	Check(Near(StatsFor(small, progress)["density"], 0.8), "upgrades: levels past a node's last count as its last");
	Progress big;
	big.levels["a"] = 2;
	UpgradeTree generous = small;
	generous.nodes[0].effects[0].add = 5;
	Check(Near(StatsFor(generous, big)["density"], 1.0), "upgrades: stats are clamped to their range (density at most 1)");

	// Progress survives a save and load.
	Progress saved;
	saved.coins = 123;
	saved.earned = 456;
	saved.levels = { { "a", 2 }, { "b", 1 } };
	Progress reloaded;
	Check(LoadProgress(SaveProgress(saved), reloaded) && reloaded.coins == 123 && reloaded.earned == 456 && reloaded.levels == saved.levels,
		"upgrades: progress survives SaveProgress / LoadProgress");
	Check(!LoadProgress("not json", reloaded) && reloaded.coins == 0, "upgrades: a broken save starts over");

	// The tree's own round trip (what the editor will write).
	UpgradeTree again;
	problems.clear();
	Check(LoadUpgradeTree(SaveUpgradeTree(tree), again, problems) && again.nodes.size() == tree.nodes.size()
		&& SaveUpgradeTree(again) == SaveUpgradeTree(tree), "upgrades: SaveUpgradeTree / LoadUpgradeTree round-trip the shipped tree");

	// Mistakes in a tree are reported.
	auto problemsIn = [](const std::string& json)
	{
		UpgradeTree t;
		std::vector<std::string> found;
		LoadUpgradeTree(json, t, found);
		return found.size();
	};
	Check(problemsIn("{ nope") > 0, "upgrades: bad JSON is reported");
	Check(problemsIn(R"({ "nodes": [ { "id": "a", "effects": [ { "stat": "speed", "add": 1 } ] } ] })") > 0, "upgrades: an unknown stat is reported");
	Check(problemsIn(R"({ "nodes": [ { "id": "a", "requires": [ { "id": "ghost" } ] } ] })") > 0, "upgrades: a requirement naming a missing node is reported");
	Check(problemsIn(R"({ "nodes": [ { "id": "a", "requires": [ { "id": "b" } ] }, { "id": "b", "requires": [ { "id": "a" } ] } ] })") > 0, "upgrades: a requirement cycle is reported");
	Check(problemsIn(R"({ "nodes": [ { "id": "a" }, { "id": "a" } ] })") > 0, "upgrades: a duplicate id is reported");
	Check(CoinsFor(tree, 1000) == static_cast<long long>(1000 * tree.coinsPerPoint) && CoinsFor(tree, -5) == 0, "upgrades: a session banks score x coinsPerPoint coins");

	// Die types in the simulation: a lone die on a 1-cell board.
	auto loneDie = [](double goldShare, double luckyShare, uint32_t seed)
	{
		DiceSim sim;
		sim.rand.Seed(seed);
		sim.config.boardSize = 1;
		sim.config.density = 1;
		sim.kinds.goldShare = goldShare;
		sim.kinds.luckyShare = luckyShare;
		sim.BuildBoard();
		return sim;
	};
	{
		DiceSim plain = loneDie(0, 0, 5);
		DiceSim reference;
		reference.rand.Seed(5);
		reference.config.boardSize = 1;
		reference.config.density = 1;
		reference.BuildBoard();
		Check(plain.diceCount == 1 && plain.D.kind[0] == Kind_Normal && plain.D.face[0] == reference.D.face[0], "die types: with no shares every die is normal and the deal is unchanged");
	}
	{
		DiceSim sim = loneDie(1, 0, 7);
		sim.StartSession();
		bool goldOk = sim.diceCount == 1 && sim.D.kind[0] == Kind_Gold;
		long long expected = 0;
		bool goldPopups = true;
		for (int roll = 0; roll < 20; ++roll)
		{
			sim.Launch(0, roll * 0.7, 1.5, 0, 0, 0, sim.sessionId);
			for (int f = 0; f < 120 && sim.D.state[0] != S_COOLDOWN; ++f) sim.Frame(1.0 / 60, false, 0, 0);
			expected += static_cast<long long>(sim.D.face[0] * sim.config.pointsPerPip * 2);
			for (const PopupRequest& pop : sim.popupsOut) goldPopups &= (pop.style & Pop_Gold) != 0;
			sim.popupsOut.clear();
			for (int f = 0; f < 60; ++f) sim.Frame(1.0 / 60, false, 0, 0);   // cooldown
		}
		Check(goldOk && sim.score == expected && goldPopups, "die types: a golden die scores face x points x goldMultiplier (2), its popups are gold (" + std::to_string(sim.score) + ")");
	}
	{
		DiceSim sim = loneDie(0, 1, 9);
		sim.kinds.luckyLuck = 1.5;
		double sum = 0;
		const int rolls = 400;
		for (int roll = 0; roll < rolls; ++roll)
		{
			sim.Launch(0, roll * 0.7, 1.5, 0, 0, 0, 0);
			for (int f = 0; f < 120 && sim.D.state[0] != S_COOLDOWN; ++f) sim.Frame(1.0 / 60, false, 0, 0);
			sum += sim.D.face[0];
			for (int f = 0; f < 60; ++f) sim.Frame(1.0 / 60, false, 0, 0);
		}
		const double mean = sum / rolls, expectedMean = sim.ExpectedFace(1.5);
		Check(sim.D.kind[0] == Kind_Lucky && std::fabs(mean - expectedMean) < 0.2 && expectedMean > 4.5,
			"die types: a lucky die rolls with luckyLuck added (mean face " + std::to_string(mean) + ", expected " + std::to_string(expectedMean) + ")");
	}
	{
		DiceSim sim;
		sim.rand.Seed(11);
		sim.config.boardSize = 40;
		sim.config.density = 1;
		sim.kinds.goldShare = 0.2;
		sim.kinds.luckyShare = 0.1;
		sim.BuildBoard();
		int gold = 0, lucky = 0;
		for (int i = 0; i < sim.diceCount; ++i) { gold += sim.D.kind[i] == Kind_Gold; lucky += sim.D.kind[i] == Kind_Lucky; }
		const double g = double(gold) / sim.diceCount, l = double(lucky) / sim.diceCount;
		Check(std::fabs(g - 0.2) < 0.03 && std::fabs(l - 0.1) < 0.03, "die types: the deal gives each type its share (gold " + std::to_string(g) + ", lucky " + std::to_string(l) + ")");
	}
}

// ---- The HUD document (Content/UI/Hud.rml, Hud.rcss) loads cleanly -----------------------------
// RmlUi with a render interface that draws nothing: the document and its style sheet parse
// without a warning, every binding finds its variable, and the styles come out as meant. The
// "dice" model here mirrors DiceModule::CreateHud (DiceHud.cpp): keep the two in step.
namespace
{
	class NullUiRenderer final : public Rml::RenderInterface
	{
	public:
		int draws = 0;
		Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
		void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override { ++draws; }
		void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
		Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
		void ReleaseTexture(Rml::TextureHandle) override {}
		void EnableScissorRegion(bool) override {}
		void SetScissorRegion(Rml::Rectanglei) override {}
		// Effects: handles that work (the real renderer is UiRenderer.cpp).
		Rml::CompiledFilterHandle CompileFilter(const Rml::String&, const Rml::Dictionary&) override { return 1; }
		void ReleaseFilter(Rml::CompiledFilterHandle) override {}
		Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override { return 1; }
		void ReleaseShader(Rml::CompiledShaderHandle) override {}
		Rml::LayerHandle PushLayer() override { return 1; }
		Rml::TextureHandle SaveLayerAsTexture() override { return 1; }
		Rml::CompiledFilterHandle SaveLayerAsMaskImage() override { return 1; }
	};

	class CollectingSystem final : public Rml::SystemInterface
	{
	public:
		std::vector<std::string> problems;
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
		{
			if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING) problems.push_back(message);
			return true;
		}
	};
}

void TestHudDocument()
{
	NullUiRenderer renderer;
	CollectingSystem system;
	Rml::SetRenderInterface(&renderer);
	Rml::SetSystemInterface(&system);
	Check(Rml::Initialise(), "hud: RmlUi starts");
	const std::string content = DICE_CONTENT_DIR;
	// As DiceModule::CreateHud registers them: Segoe UI as "system-ui" (the bytes live until Shutdown).
	std::vector<std::vector<Rml::byte>> fontData;
	for (auto [file, weight] : { std::pair<const char*, int>{ "segoeui.ttf", 400 }, { "seguisb.ttf", 600 }, { "segoeuib.ttf", 700 }, { "seguibl.ttf", 900 } })
	{
		std::ifstream in(std::string("C:/Windows/Fonts/") + file, std::ios::binary);
		std::vector<Rml::byte>& data = fontData.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		Rml::LoadFontFace(Rml::Span<const Rml::byte>(data.data(), data.size()), "system-ui", Rml::Style::FontStyle::Normal, static_cast<Rml::Style::FontWeight>(weight));
	}

	Rml::Context* context = Rml::CreateContext("test", Rml::Vector2i(1280, 720));
	std::string score = "120", time = "30.0", startLabel = "Start", fps = "60 fps", total = "120", summary = "3 rolls";
	bool ended = false;
	Rml::DataModelConstructor model = context->CreateDataModel("dice");
	model.Bind("score", &score);
	model.Bind("time", &time);
	model.Bind("startLabel", &startLabel);
	model.Bind("fps", &fps);
	model.Bind("ended", &ended);
	model.Bind("total", &total);
	model.Bind("summary", &summary);
	std::string coins = "260", coinsEarned = "+12 coins";
	bool showCoins = true, canUpgrade = true;
	model.Bind("coins", &coins);
	model.Bind("showCoins", &showCoins);
	model.Bind("canUpgrade", &canUpgrade);
	model.Bind("coinsEarned", &coinsEarned);
	int starts = 0;
	model.BindEventCallback("start", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ++starts; });
	int upgradeOpens = 0;
	model.BindEventCallback("upgrades", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ++upgradeOpens; });
	Rml::DataModelHandle handle = model.GetModelHandle();

	Rml::ElementDocument* document = context->LoadDocument(content + "/UI/Hud.rml");
	Check(document != nullptr, "hud: Hud.rml loads");
	if (!document) { Rml::Shutdown(); return; }
	document->Show();
	// A popup as DiceModule::UpdateHud makes and moves them (the .pop and .six styles, a transform).
	Rml::Element* popups = document->GetElementById("popups");
	Rml::Element* popup = nullptr;
	if (popups)
	{
		popup = popups->AppendChild(document->CreateElement("div"));
		popup->SetClass("pop", true);
		popup->SetClass("six", true);
		popup->SetInnerRML("+60");
		popup->SetProperty(Rml::PropertyId::Transform, Rml::Transform::MakeProperty({ Rml::Transforms::Translate2D(400, 300, Rml::Unit::PX),
			Rml::Transforms::Translate2D(-50.0f, -50.0f, Rml::Unit::PERCENT), Rml::Transforms::Scale2D(1.1f) }));
		popup->SetProperty(Rml::PropertyId::Opacity, Rml::Property(0.5f, Rml::Unit::NUMBER));
	}
	context->Update();
	context->Render();
	ended = true;
	handle.DirtyVariable("ended");
	context->Update();
	context->Render();

	std::string problems;
	for (const std::string& problem : system.problems) problems += "\n    " + problem;
	Check(system.problems.empty(), "hud: the document, style sheet and bindings give no warnings" + problems);

	Rml::Element* hud = document->GetElementById("hud");
	const Rml::Colourb background = hud ? hud->GetComputedValues().background_color() : Rml::Colourb(0, 0, 0, 0);
	// 78% of 255 = 198.9, which RmlUi rounds down.
	Check(hud && background.alpha >= 198 && background.alpha <= 199 && background.red == 20,
		"hud: the score box is #14161a at 78% (rgba() alpha in percent, not 0..1): alpha " + std::to_string(background.alpha));
	Check(hud && hud->GetBox().GetSize().x > 200 && hud->GetBox().GetSize().y > 40, "hud: the score box has a size (flex layout, fonts found)");
	Rml::Element* card = document->QuerySelector("#endCard .card");
	Check(card != nullptr && card->IsVisible(), "hud: the end card appears when the session ends");
	Check(popup && popup->GetComputedValues().font_size() == 17.0f && popup->GetComputedValues().opacity() == 0.5f && popup->GetComputedValues().has_local_transform(),
		"hud: a popup takes the .pop.six style (17 px), its opacity and its transform");
	Rml::ElementList buttons;
	document->GetElementsByTagName(buttons, "button");
	for (Rml::Element* button : buttons) button->Click();
	context->Update();
	Check(starts == 2, "hud: Start and Restart call the \"start\" event");
	Check(upgradeOpens == 2, "hud: the HUD's and the end card's Upgrades buttons call the \"upgrades\" event");
	Check(renderer.draws > 0, "hud: something is drawn");

	// The upgrade screen (Upgrades.rml), with the "upgrades" model DiceModule::CreateUpgradesUi binds.
	{
		struct NodeView { std::string name = "More dice", level = "1 / 7", cost = "39", left = "0dp", top = "55dp"; bool locked = false, expensive = false, buyable = true, maxed = false, flash = false; };
		struct LinkView { std::string left = "150dp", top = "87dp", width = "60dp", transform = "rotate(-0.5rad)"; bool met = true; };
		std::vector<NodeView> nodes(3);
		std::vector<LinkView> links(2);
		bool open = true;
		std::string upgradeCoins = "260", detailName = "Upgrades", detailText = "text", detailNext = "next";
		Rml::DataModelConstructor upgrades = context->CreateDataModel("upgrades");
		if (Rml::StructHandle<NodeView> node = upgrades.RegisterStruct<NodeView>())
		{
			node.RegisterMember("name", &NodeView::name);
			node.RegisterMember("level", &NodeView::level);
			node.RegisterMember("cost", &NodeView::cost);
			node.RegisterMember("left", &NodeView::left);
			node.RegisterMember("top", &NodeView::top);
			node.RegisterMember("locked", &NodeView::locked);
			node.RegisterMember("expensive", &NodeView::expensive);
			node.RegisterMember("buyable", &NodeView::buyable);
			node.RegisterMember("maxed", &NodeView::maxed);
			node.RegisterMember("flash", &NodeView::flash);
		}
		upgrades.RegisterArray<std::vector<NodeView>>();
		if (Rml::StructHandle<LinkView> link = upgrades.RegisterStruct<LinkView>())
		{
			link.RegisterMember("left", &LinkView::left);
			link.RegisterMember("top", &LinkView::top);
			link.RegisterMember("width", &LinkView::width);
			link.RegisterMember("transform", &LinkView::transform);
			link.RegisterMember("met", &LinkView::met);
		}
		upgrades.RegisterArray<std::vector<LinkView>>();
		upgrades.Bind("nodes", &nodes);
		upgrades.Bind("links", &links);
		upgrades.Bind("open", &open);
		upgrades.Bind("coins", &upgradeCoins);
		upgrades.Bind("detailName", &detailName);
		upgrades.Bind("detailText", &detailText);
		upgrades.Bind("detailNext", &detailNext);
		int bought = -1, hovered = -2, closes = 0;
		upgrades.BindEventCallback("buy", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) { bought = args.empty() ? -1 : args[0].Get<int>(-1); });
		upgrades.BindEventCallback("hover", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) { hovered = args.empty() ? -2 : args[0].Get<int>(-2); });
		upgrades.BindEventCallback("close", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { ++closes; });
		system.problems.clear();
		Rml::ElementDocument* screen = context->LoadDocument(content + "/UI/Upgrades.rml");
		if (screen) screen->Show();
		context->Update();
		context->Render();
		std::string upgradeProblems;
		for (const std::string& problem : system.problems) upgradeProblems += "\n    " + problem;
		Check(screen && system.problems.empty(), "hud: the upgrade screen loads without warnings" + upgradeProblems);
		Rml::ElementList shownNodes;
		if (screen) screen->QuerySelectorAll(shownNodes, ".node");
		int visibleNodes = 0;
		Rml::Element* lastNode = nullptr;   // (data-for keeps a hidden template among them)
		for (Rml::Element* element : shownNodes)
			if (element->IsVisible()) { ++visibleNodes; lastNode = element; }
		if (lastNode) lastNode->Click();
		context->Update();
		Check(visibleNodes == 3 && bought == 2, "hud: the screen shows a node per entry, and clicking one buys it by index (" + std::to_string(bought) + ")");
	}

	// The effects gallery (DICE_DEV_GALLERY) parses too: every effect's syntax is RmlUi's.
	system.problems.clear();
	Rml::ElementDocument* gallery = context->LoadDocument(content + "/UI/Gallery.rml");
	if (gallery) gallery->Show();
	context->Update();
	context->Render();
	problems.clear();
	for (const std::string& problem : system.problems) problems += "\n    " + problem;
	Check(gallery && system.problems.empty(), "hud: the effects gallery loads without warnings" + problems);

	Rml::Shutdown();
}

int main()
{
	TestFaceUp();
	TestAtlasAndMesh();
	TestTumbleDirection();
	TestMirror();
	TestBoard();
	TestNearestSlot();
	TestHover();
	TestRollContinuity();
	TestLuck();
	TestRollInvariants();
	TestSession();
	TestMidAir();
	TestBumps();
	TestLayouts();
	TestAllLayouts();
	TestStackInvariants();
	TestStackRules();
	TestConfigData();
	TestControls();
	TestPresets();
	TestControlsAtRuntime();
	TestSounds();
	TestUpgrades();
	TestHudDocument();
	std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "OK", gFailures, gFailures == 1 ? "" : "s");
	return gFailures ? 1 : 0;
}
