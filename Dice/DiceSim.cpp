#include "DiceSim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dice
{
	Vec3 FaceNormal(int face)
	{
		switch (face)
		{
		case 1: return { 0, 1, 0 };
		case 6: return { 0, -1, 0 };
		case 2: return { 1, 0, 0 };
		case 5: return { -1, 0, 0 };
		case 3: return { 0, 0, 1 };
		default: return { 0, 0, -1 };   // 4
		}
	}

	const Quat& FaceUp(int face)
	{
		static const Quat table[7] = {
			Quat{},
			Quat::FromUnitVectors(FaceNormal(1), { 0, 1, 0 }), Quat::FromUnitVectors(FaceNormal(2), { 0, 1, 0 }),
			Quat::FromUnitVectors(FaceNormal(3), { 0, 1, 0 }), Quat::FromUnitVectors(FaceNormal(4), { 0, 1, 0 }),
			Quat::FromUnitVectors(FaceNormal(5), { 0, 1, 0 }), Quat::FromUnitVectors(FaceNormal(6), { 0, 1, 0 }),
		};
		return table[std::clamp(face, 1, 6)];
	}

	Quat RestQuat(int face, double yaw)
	{
		return Quat::AxisAngle({ 0, 1, 0 }, yaw).Multiply(FaceUp(face));
	}

	int FaceOnTop(const Quat& q)
	{
		int best = 1;
		double bestY = -2;
		for (int f = 1; f <= 6; ++f)
		{
			const double y = q.Rotate(FaceNormal(f)).y;
			if (y > bestY) { bestY = y; best = f; }
		}
		return best;
	}

	void DiceState::Allocate(int n)
	{
		const size_t c = static_cast<size_t>(n);
		state.assign(c, S_READY); cell.assign(c, 0);
		px.assign(c, 0); py.assign(c, 0); pz.assign(c, 0); q.assign(c * 4, 0);
		sx.assign(c, 0); sz.assign(c, 0); mx.assign(c, 0); mz.assign(c, 0); um.assign(c, 0); ex.assign(c, 0); ez.assign(c, 0);
		t0.assign(c, 0); dur.assign(c, 0); hop.assign(c, 0);
		qs.assign(c * 4, 0); qe.assign(c * 4, 0);
		ax.assign(c, 0); az.assign(c, 0); ang.assign(c, 0);
		face.assign(c, 1);
		timer.assign(c, 0); landT.assign(c, 0); readyT.assign(c, 0); hitT.assign(c, 0);
		chain.assign(c, 0); bumped.assign(c, 0); clashed.assign(c, 0); ease.assign(c, 0);
		session.assign(c, 0); h0.assign(c, 0); hits.assign(c, 0); lastHit.assign(c, -1);
		offX.assign(c, 0); offZ.assign(c, 0); bu.assign(c, 0); arc.assign(c, 1);
		below.assign(c, -1); level.assign(c, 0); by.assign(c, 0); sb.assign(c, 0); eb.assign(c, 0);
		inActive.assign(c, 0);
	}

	DiceSim::DiceSim()
	{
		Reserve(kMaxDice);
	}

	void DiceSim::Reserve(int capacity)
	{
		mCapacity = capacity;
		D.Allocate(capacity);
		slotX.assign(capacity, 0); slotZ.assign(capacity, 0);
		slotOwner.assign(capacity, -1);
		hashHead.assign(capacity, -1); hashNext.assign(capacity, -1);
		active.assign(capacity, 0);
		mBucketHead.assign(capacity, -1); mBucketNext.assign(capacity, -1); mBucketCell.assign(capacity, 0);
	}

	int DiceSim::HashCoord(double v) const
	{
		return std::min(N - 1, std::max(0, static_cast<int>(std::floor(v + N / 2.0))));
	}

	bool DiceSim::InShape(double x, double z) const
	{
		const double R = N / 2.0 - 0.3, d = std::hypot(x, z);
		switch (config.boardShape)
		{
		case BoardShape::Circle: return d <= R;
		case BoardShape::Ring: return d <= R && d >= R * 0.45;
		case BoardShape::Islands:
			for (const auto& island : kIslands)
				if (std::hypot(x - island[0] * N, z - island[1] * N) <= island[2] * N) return true;
			return false;
		default: return true;
		}
	}

	void DiceSim::AddSlot(double x, double z)
	{
		if (slotCount >= mCapacity || !InShape(x, z)) return;
		slotX[slotCount] = static_cast<float>(x);
		slotZ[slotCount] = static_cast<float>(z);
		++slotCount;
	}

	void DiceSim::GenSquare()
	{
		const double h = N / 2.0;
		for (int cz = 0; cz < N; ++cz)
			for (int cx = 0; cx < N; ++cx) AddSlot(cx - h + 0.5, cz - h + 0.5);
	}

	// Offset rows, every neighbour 1 cell away.
	void DiceSim::GenHex()
	{
		const double h = N / 2.0, dz = std::sqrt(3.0) / 2;
		const int rows = static_cast<int>(std::floor((N - 1) / dz)) + 1;
		const double z0 = -(rows - 1) * dz / 2;
		for (int r = 0; r < rows; ++r)
		{
			const int odd = r & 1, cols = odd ? N - 1 : N;
			for (int c = 0; c < cols; ++c) AddSlot(c - h + 0.5 + (odd ? 0.5 : 0), z0 + r * dz);
		}
	}

	// Bridson Poisson-disk sampling.
	void DiceSim::GenScatter()
	{
		const double minD = std::max(0.7, static_cast<double>(config.scatterSpacing)), lo = -N / 2.0 + 0.4, size = N - 0.8;
		const double cs = minD / std::sqrt(2.0);
		const int gw = static_cast<int>(std::ceil(size / cs));
		std::vector<int> grid(static_cast<size_t>(gw) * gw, -1), open;
		std::vector<double> px, pz;
		auto tryAdd = [&](double x, double z)
		{
			if (x < lo || z < lo || x > lo + size || z > lo + size) return false;
			const int gx = std::min(gw - 1, static_cast<int>((x - lo) / cs)), gz = std::min(gw - 1, static_cast<int>((z - lo) / cs));
			for (int j = std::max(0, gz - 2); j <= std::min(gw - 1, gz + 2); ++j)
				for (int k = std::max(0, gx - 2); k <= std::min(gw - 1, gx + 2); ++k)
				{
					const int p = grid[static_cast<size_t>(j) * gw + k];
					if (p >= 0 && (px[p] - x) * (px[p] - x) + (pz[p] - z) * (pz[p] - z) < minD * minD) return false;
				}
			grid[static_cast<size_t>(gz) * gw + gx] = static_cast<int>(px.size());
			open.push_back(static_cast<int>(px.size()));
			px.push_back(x);
			pz.push_back(z);
			return true;
		};
		tryAdd(lo + rand() * size, lo + rand() * size);
		while (!open.empty())
		{
			const int k = static_cast<int>(rand() * open.size()), p = open[k];
			bool ok = false;
			for (int t = 0; t < 30 && !ok; ++t)
			{
				const double a = rand() * kPi * 2, r = minD * (1 + rand());
				ok = tryAdd(px[p] + std::cos(a) * r, pz[p] + std::sin(a) * r);
			}
			if (!ok) { open[k] = open.back(); open.pop_back(); }
		}
		for (size_t n = 0; n < px.size(); ++n) AddSlot(px[n], pz[n]);
	}

	void DiceSim::GenerateSlots()
	{
		slotCount = 0;
		if (config.layout == Layout::Hex) GenHex();
		else if (config.layout == Layout::Scatter) GenScatter();
		else GenSquare();
		std::fill(hashHead.begin(), hashHead.begin() + N * N, -1);
		for (int s = 0; s < slotCount; ++s)
		{
			const int h = HashOf(slotX[s], slotZ[s]);
			hashNext[s] = hashHead[h];
			hashHead[h] = s;
		}
	}

	int DiceSim::NearestSlot(double x, double z, bool freeOnly)
	{
		const int hx = HashCoord(x), hz = HashCoord(z);
		int best = -1, foundRing = -1;
		double bd = 1e18;
		for (int r = 0; r < N; ++r)
		{
			for (int dz = -r; dz <= r; ++dz)
			{
				const int zz = hz + dz;
				if (zz < 0 || zz >= N) continue;
				const int step = (dz == -r || dz == r) ? 1 : 2 * r;   // only the ring's perimeter
				for (int dx = -r; dx <= r; dx += step ? step : 1)
				{
					const int xx = hx + dx;
					if (xx < 0 || xx >= N) continue;
					for (int s = hashHead[zz * N + xx]; s >= 0; s = hashNext[s])
					{
						if (freeOnly && slotOwner[s] >= 0) continue;
						const double ex = slotX[s] - x, ez = slotZ[s] - z;
						const double d = ex * ex + ez * ez + (freeOnly ? rand() * 0.3 : 0);
						if (d < bd) { bd = d; best = s; }
					}
				}
			}
			if (best >= 0 && foundRing < 0) foundRing = r;
			if (foundRing >= 0 && r > foundRing) break;   // one extra ring catches closer corners
		}
		return best;
	}

	double DiceSim::Reflect(double v, double max)
	{
		if (max <= 0) return 0;
		for (int k = 0; k < 4 && (v < 0 || v > max); ++k)
		{
			if (v < 0) v = -v;
			if (v > max) v = 2 * max - v;
		}
		return std::min(max, std::max(0.0, v));
	}

	void DiceSim::FaceWeights(double w[7], double& sum) const
	{
		w[0] = 0;
		sum = 0;
		for (int f = 1; f <= 6; ++f)
		{
			w[f] = std::exp(config.luck * 3 * (f - 3.5) / 2.5);
			sum += w[f];
		}
	}

	int DiceSim::PickFace()
	{
		double w[7], sum;
		FaceWeights(w, sum);
		double r = rand() * sum;
		for (int f = 1; f <= 6; ++f)
		{
			r -= w[f];
			if (r <= 0) return f;
		}
		return 6;
	}

	double DiceSim::ExpectedFace() const
	{
		double w[7], sum, ev = 0;
		FaceWeights(w, sum);
		for (int f = 1; f <= 6; ++f) ev += f * w[f];
		return ev / sum;
	}

	void DiceSim::PathXZ(int i, double u, double& x, double& z) const
	{
		const double h = PathFrac(i, u);
		const double um = D.um[i], h0 = D.h0[i];
		if (h < um)
		{
			const double f = (h - h0) / (um - h0);
			x = D.sx[i] + (D.mx[i] - D.sx[i]) * f;
			z = D.sz[i] + (D.mz[i] - D.sz[i]) * f;
		}
		else
		{
			const double f = (h - um) / (1 - um);
			x = D.mx[i] + (D.ex[i] - D.mx[i]) * f;
			z = D.mz[i] + (D.ez[i] - D.mz[i]) * f;
		}
	}

	// Leave the slot (or stack) die i rests in / has claimed. If it was on top of another die,
	// that die becomes the slot's top and gets a fresh cooldown.
	void DiceSim::LeaveSlot(int i)
	{
		const int c = D.cell[i];
		if (slotOwner[c] == i)
		{
			const int b = D.below[i];
			slotOwner[c] = b;
			if (b >= 0 && (D.state[b] == S_READY || D.state[b] == S_COOLDOWN)) { D.state[b] = S_COOLDOWN; D.timer[b] = now + config.cooldown; }
		}
		D.below[i] = -1;
		D.level[i] = 0;
	}

	// End point inside slot c (with jitter); keeps the path straight unless clashed.
	void DiceSim::SetEnd(int i, int c)
	{
		D.ex[i] = static_cast<float>(slotX[c] + Jit());
		D.ez[i] = static_cast<float>(slotZ[c] + Jit());
		if (!D.clashed[i])
		{
			D.mx[i] = (D.sx[i] + D.ex[i]) / 2;
			D.mz[i] = (D.sz[i] + D.ez[i]) / 2;
			D.um[i] = 0.5f;
		}
	}

	int DiceSim::NearestFree(int target)
	{
		if (slotOwner[target] < 0) return target;
		// A die needing a slot never owns one at that moment, so a free slot always exists.
		const int c = NearestSlot(slotX[target], slotZ[target], true);
		return c >= 0 ? c : target;
	}

	// B10.1. M3 has the claim and the nudge; stack (M8), bounce-off and push (M6) come later.
	double DiceSim::ContactU(int i, double ox, double oz) const
	{
		const double R = static_cast<double>(config.dieSize) * config.bumpContact;
		double pu = 0, pd = std::hypot(D.sx[i] - ox, D.sz[i] - oz);
		if (pd <= R) return 0;
		for (int s = 1; s <= 40; ++s)
		{
			const double u = s / 40.0;
			double x, z;
			PathXZ(i, u, x, z);
			const double d = std::hypot(x - ox, z - oz);
			if (d <= R) return pu + (u - pu) * ((pd - R) / std::max(1e-6, pd - d));
			pu = u;
			pd = d;
		}
		return 1;
	}

	// B10.1, in order: claim, stack (or topple), bounce-off, push, nudge.
	int DiceSim::ResolveTarget(int i, int target, double travel, int chain, int session)
	{
		const int occ = slotOwner[target];
		if (occ < 0 || occ == i) { slotOwner[target] = i; return target; }
		const bool occResting = D.state[occ] == S_READY || D.state[occ] == S_COOLDOWN;

		// Stack mode: settle on top, or topple a full stack.
		if (config.collisionMode == CollisionMode::Stack && occResting)
		{
			if (D.level[occ] + 1 < config.stackMax)
			{
				if (rand() < config.stackChance) return StackOn(i, occ, target);
			}
			else if (chain < config.bumpChainLimit && bumpTokens >= 1)
				return Topple(i, target, travel, chain, session);
		}

		// Bump mode, random outcome A: ricochet. The incoming die bounces off and the occupant stays put.
		if (config.collisionMode == CollisionMode::Bump && !D.bumped[i] && !D.clashed[i] && rand() < config.bounceChance && occResting)
			return BounceOff(i, occ, travel);

		// Random outcome B: push the occupant away.
		if (config.collisionMode == CollisionMode::Bump && chain < config.bumpChainLimit && bumpTokens >= 1 && occResting)
		{
			// Take the slot. The occupant is shoved at the moment the two dice touch, along the
			// contact normal, so they never sit inside each other.
			bumpTokens -= 1;
			stats.bumps++;
			if (session && session == sessionId) sStats.bumps++;
			slotOwner[target] = i;
			// Take the occupant's place, including its spot in a stack.
			D.below[i] = D.below[occ];
			D.level[i] = D.level[occ];
			D.eb[i] = D.by[occ];

			// The incoming die loses momentum: it settles a little short of the slot centre.
			const double cx = std::cos(travel), cz = std::sin(travel), sj = config.slotJitter * 0.3;
			D.ex[i] = static_cast<float>(slotX[target] - cx * config.bumpRecoil + (rand() * 2 - 1) * sj);
			D.ez[i] = static_cast<float>(slotZ[target] - cz * config.bumpRecoil + (rand() * 2 - 1) * sj);
			if (!D.clashed[i]) { D.mx[i] = (D.sx[i] + D.ex[i]) / 2; D.mz[i] = (D.sz[i] + D.ez[i]) / 2; D.um[i] = 0.5f; }

			// When and where they touch.
			const double ox = D.px[occ], oz = D.pz[occ];
			const double uc = ContactU(i, ox, oz);
			double pcx, pcz;
			PathXZ(i, uc, pcx, pcz);
			double nx = ox - pcx, nz = oz - pcz, nl = std::hypot(nx, nz);
			if (nl < 1e-4) { nx = cx; nz = cz; nl = 1; }
			nx /= nl; nz /= nl;
			const double wN = config.bumpNormalWeight;
			const double pushAngle = std::atan2(nz * wN + cz * (1 - wN), nx * wN + cx * (1 - wN));
			const double tHit = D.t0[i] + uc * D.dur[i];

			const double spread = config.bumpSpreadDeg * kPi / 180;
			const double lo = std::min(config.bumpDistMin, config.bumpDistMax), hi = std::max(config.bumpDistMin, config.bumpDistMax);
			Launch(occ, pushAngle + (rand() * 2 - 1) * spread, rand.Range(lo, hi), chain + 1, 1, std::max(0.001, tHit - now), session);

			Event e;
			e.type = Event::Bump;
			e.t = tHit;
			e.x = static_cast<float>((pcx + ox) / 2);
			e.z = static_cast<float>((pcz + oz) / 2);
			e.y = mHalf * 1.4f;
			e.session = 0;
			events.push_back(e);
			return target;
		}
		// Nudge (also the fallback when bumps are limited).
		const int c = NearestFree(target);
		slotOwner[c] = i;
		return c;
	}

	// Stack: claim the slot as its new top and land on the top face of die T.
	int DiceSim::StackOn(int i, int T, int target)
	{
		slotOwner[target] = i;
		D.below[i] = T;
		D.level[i] = static_cast<uint8_t>(D.level[T] + 1);
		D.eb[i] = D.by[T] + config.dieSize;
		D.ex[i] = static_cast<float>(D.px[T] + (rand() * 2 - 1) * 0.05);   // near-perfect alignment, slight wobble
		D.ez[i] = static_cast<float>(D.pz[T] + (rand() * 2 - 1) * 0.05);
		if (!D.clashed[i]) { D.mx[i] = (D.sx[i] + D.ex[i]) / 2; D.mz[i] = (D.sz[i] + D.ez[i]) / 2; D.um[i] = 0.5f; }
		stats.stacks++;
		return target;
	}

	// Topple: the incoming die hits a full stack. Every die above the bottom one is knocked off
	// (from the top down, at the moment of contact), then the incoming die settles on the bottom die.
	int DiceSim::Topple(int i, int target, double travel, int chain, int session)
	{
		bumpTokens -= 1;
		std::vector<int> list;
		int d = slotOwner[target];
		while (D.below[d] >= 0) { list.push_back(d); d = D.below[d]; }
		const int bottom = d;
		for (int die : list) D.state[die] = S_ROLLING;   // nothing may stack on or bump them mid-topple (B13.1)
		const double ox = D.px[bottom], oz = D.pz[bottom];
		const double uc = ContactU(i, ox, oz);
		const double tHit = D.t0[i] + uc * D.dur[i];
		// Claim the slot first, as a push does (deviation from the reference, which does this
		// last): a knocked-off die whose throw reflects off the board edge back onto this same
		// slot would otherwise stack on the bottom die again, and this die's stackOn would then
		// overwrite the slot's top, leaving that die in no chain (two dice on one spot, one of
		// them never hoverable again). Now it finds the slot taken and nudges. Docs/Port.md.
		StackOn(i, bottom, target);
		const double spread = config.toppleSpreadDeg * kPi / 180;
		const double lo = std::min(config.toppleDistMin, config.toppleDistMax), hi = std::max(config.toppleDistMin, config.toppleDistMax);
		for (size_t k = 0; k < list.size(); ++k)   // top die first; each launch uncovers the next
		{
			const int die = list[k];
			const double dist = rand.Range(lo, hi) + 0.4 * (D.level[die] - 1);
			Launch(die, travel + (rand() * 2 - 1) * spread, dist, chain + 1, 1, std::max(0.001, tHit - now) + k * 0.04, session);
		}
		stats.topples++;
		Event e;
		e.type = Event::Topple;
		e.t = tHit;
		e.x = static_cast<float>(ox);
		e.z = static_cast<float>(oz);
		e.y = static_cast<float>(mHalf + config.dieSize * list.size() * 0.6);
		e.n = static_cast<int>(list.size());
		e.session = session;
		events.push_back(e);
		return target;
	}

	// Ricochet: fly to the contact point, mirror the travel direction off the occupant's surface
	// normal, and hop a short way to the nearest free slot. The roll is lengthened so the hit
	// happens at the same time and place it would have, then the bounce leg takes bounceTime.
	int DiceSim::BounceOff(int i, int occ, double travel)
	{
		const double ox = D.px[occ], oz = D.pz[occ];
		const double uc = ContactU(i, ox, oz);   // the path currently aims at the occupied slot
		double cx, cz;
		PathXZ(i, uc, cx, cz);
		const double dx = std::cos(travel), dz = std::sin(travel);
		double nx = cx - ox, nz = cz - oz, nl = std::hypot(nx, nz);
		if (nl < 1e-4) { nx = -dx; nz = -dz; nl = 1; }
		nx /= nl; nz /= nl;
		const double dot = dx * nx + dz * nz;
		const double a = std::atan2(dz - 2 * dot * nz, dx - 2 * dot * nx) + (rand() * 2 - 1) * config.bounceSpreadDeg * kPi / 180;
		const double lo = std::min(config.bounceDistMin, config.bounceDistMax), hi = std::max(config.bounceDistMin, config.bounceDistMax);
		const double dist = rand.Range(lo, hi);
		const int c = NearestSlot(cx + std::cos(a) * dist, cz + std::sin(a) * dist, true);
		slotOwner[c] = i;

		const double oldDur = D.dur[i], tc = uc * oldDur, newDur = tc + config.bounceTime;
		D.dur[i] = static_cast<float>(newDur);
		D.bu[i] = static_cast<float>(std::max(0.01, tc / newDur));
		D.arc[i] = static_cast<float>(newDur / oldDur);
		D.mx[i] = static_cast<float>(cx); D.mz[i] = static_cast<float>(cz); D.um[i] = D.bu[i];
		D.ex[i] = static_cast<float>(slotX[c] + Jit());
		D.ez[i] = static_cast<float>(slotZ[c] + Jit());
		D.clashed[i] = 1;   // the path isn't straight: the launch-time clash maths skips it
		const double tHit = D.t0[i] + tc;
		D.hitT[occ] = tHit;
		Event e;
		e.type = Event::Bump;
		e.t = tHit;
		e.x = static_cast<float>((cx + ox) / 2);
		e.z = static_cast<float>((cz + oz) / 2);
		e.y = mHalf * 1.4f;
		e.session = 0;
		events.push_back(e);
		stats.bounces++;
		return c;
	}

	void DiceSim::Launch(int i, double angle, double dist, int chain, int bumped, double delay, int session)
	{
		if (D.inActive[i]) reentryDetected = true;   // B13.1: must never happen
		const int from = D.cell[i];
		// Mark it as moving right away: a push chain planned below must never be able to push
		// this same die again (it would be launched twice).
		D.state[i] = S_ROLLING;
		LeaveSlot(i);
		D.sb[i] = D.by[i];
		D.eb[i] = 0;   // starts at its stack height, lands on the board unless re-planned
		const double edge = -N / 2.0 + 0.5, span = N - 1;   // reflect off the board edge
		const double tx = edge + Reflect(slotX[from] - edge + std::cos(angle) * dist, span);
		const double tz = edge + Reflect(slotZ[from] - edge + std::sin(angle) * dist, span);
		int target = NearestSlot(tx, tz, false);   // holes in the shape snap to their rim
		if (target < 0) target = from;

		const double t0 = now + delay;
		double dur = std::max(0.08, config.rollDuration * (1 + (rand() * 2 - 1) * config.durationJitter));
		if (bumped) dur *= config.bumpDurationMult;
		const double sx = D.px[i], sz = D.pz[i];
		D.sx[i] = static_cast<float>(sx); D.sz[i] = static_cast<float>(sz);
		D.t0[i] = t0; D.dur[i] = static_cast<float>(dur);
		D.chain[i] = static_cast<uint8_t>(chain); D.bumped[i] = static_cast<uint8_t>(bumped); D.clashed[i] = 0; D.session[i] = session;
		D.ease[i] = static_cast<uint8_t>(bumped);
		D.h0[i] = 0; D.hits[i] = 0; D.lastHit[i] = -1; D.offX[i] = 0; D.offZ[i] = 0;
		D.bu[i] = 0; D.arc[i] = 1;
		D.face[i] = static_cast<uint8_t>(PickFace());
		D.um[i] = 0.5f;

		// Mid-air clash check against already-airborne dice (may deflect our target). Pushed
		// dice skip it: their eased motion doesn't fit the straight-line maths.
		if (config.clashEnabled && !bumped)
		{
			const int deflected = CheckClash(i, target, t0, dur, session);
			if (deflected >= 0) target = deflected;
		}

		// Claim the landing slot (nudge or bump if taken). The end point is set first so a
		// bump can work out exactly where and when the two dice touch.
		double travel = std::atan2(slotZ[target] - sz, slotX[target] - sx);
		if (target == from) travel = angle;
		SetEnd(i, target);
		const int resolved = ResolveTarget(i, target, travel, chain, session);
		if (resolved != target && !D.bu[i]) SetEnd(i, resolved);   // a ricochet already set its own end
		target = resolved;
		D.cell[i] = target;
		const double ex = D.ex[i], ez = D.ez[i];

		// Tumble plan: rotate about the horizontal axis perpendicular to travel, pre-wind the
		// start so the motion ends exactly on the final orientation (a ricochet tumbles along
		// its incoming leg, up to the hit point).
		double dx = (D.bu[i] ? D.mx[i] : ex) - sx, dz = (D.bu[i] ? D.mz[i] : ez) - sz;
		const double L = std::hypot(dx, dz);
		if (L < 1e-3) { dx = std::cos(angle); dz = std::sin(angle); }
		else { dx /= L; dz /= L; }
		D.ax[i] = static_cast<float>(dz);
		D.az[i] = static_cast<float>(-dx);   // up x dir
		const double ang = std::max(1.0, std::round(L * config.tumblePerCell)) * kPi / 2;
		D.ang[i] = static_cast<float>(ang);
		PutQ(D.qe.data(), i, RestQuat(D.face[i], rand() * kPi * 2));
		PutQ(D.qs.data(), i, Quat::AxisAngle({ dz, 0, -dx }, ang).Multiply(GetQ(D.q.data(), i)));
		D.hop[i] = static_cast<float>(config.hopHeight * (0.55 + 0.45 * std::min(1.0, L / 3)) * (bumped ? config.bumpHopMult : 1));

		D.state[i] = delay > 0 ? S_PENDING : S_ROLLING;
		active[activeCount++] = i;
		D.inActive[i] = 1;
		stats.rolls++;
		if (session && session == sessionId) { sessionAirborne++; sStats.rolls++; }
	}

	// Closest approach of two straight, constant-speed ground paths over their shared airborne
	// window. Returns a deflected target slot for die i, or -1.
	int DiceSim::CheckClash(int i, int target, double t0, double dur, int session)
	{
		const double sx = D.sx[i], sz = D.sz[i];
		const double ex = slotX[target], ez = slotZ[target];
		const double vix = (ex - sx) / dur, viz = (ez - sz) / dur;
		const double R2 = static_cast<double>(config.clashDistance) * config.clashDistance;
		for (int k = 0; k < activeCount; ++k)
		{
			const int j = active[k];
			if (j == i || D.clashed[j]) continue;   // one clash per die per roll
			const double t0j = D.t0[j], dj = D.dur[j];
			const double lo = std::max({ t0 + 0.15 * dur, t0j + 0.15 * dj, now });   // both clearly in the air
			const double hi = std::min(t0 + 0.85 * dur, t0j + 0.85 * dj);
			if (lo >= hi) continue;
			const double vjx = (D.ex[j] - D.sx[j]) / dj, vjz = (D.ez[j] - D.sz[j]) / dj;
			// Relative position r(t) = r0 + w t.
			const double r0x = sx - vix * t0 - D.sx[j] + vjx * t0j;
			const double r0z = sz - viz * t0 - D.sz[j] + vjz * t0j;
			const double wx = vix - vjx, wz = viz - vjz, ww = wx * wx + wz * wz;
			double tc = ww > 1e-9 ? -(r0x * wx + r0z * wz) / ww : lo;
			tc = std::min(hi, std::max(lo, tc));
			const double rx = r0x + wx * tc, rz = r0z + wz * tc;
			if (rx * rx + rz * rz > R2) continue;

			// Clash! Both dice bend their path at the clash point.
			const double pix = sx + vix * (tc - t0), piz = sz + viz * (tc - t0);
			const double pjx = D.sx[j] + vjx * (tc - t0j), pjz = D.sz[j] + vjz * (tc - t0j);
			double nx = pix - pjx, nz = piz - pjz, nl = std::hypot(nx, nz);
			if (nl < 1e-4) { nx = -viz; nz = vix; nl = std::hypot(nx, nz); if (nl == 0) nl = 1; }
			nx /= nl; nz /= nl;
			const double def = config.clashDeflect;
			D.clashed[i] = 1; D.mx[i] = static_cast<float>(pix); D.mz[i] = static_cast<float>(piz); D.um[i] = static_cast<float>((tc - t0) / dur);
			D.clashed[j] = 1; D.mx[j] = static_cast<float>(pjx); D.mz[j] = static_cast<float>(pjz); D.um[j] = static_cast<float>((tc - t0j) / dj);
			D.lastHit[i] = j; D.lastHit[j] = i;   // the per-frame check won't hit this pair again
			// Re-plan j's landing (nudge only, to keep it simple and bounded).
			LeaveSlot(j);
			D.eb[j] = 0;   // also undoes a planned stack landing
			const int cj = NearestSlot(D.ex[j] - nx * def, D.ez[j] - nz * def, true);
			slotOwner[cj] = j;
			D.cell[j] = cj;
			D.ex[j] = static_cast<float>(slotX[cj] + Jit());
			D.ez[j] = static_cast<float>(slotZ[cj] + Jit());
			const double uj = D.um[j];
			Event e;
			e.type = Event::Clash;
			e.t = tc;
			e.x = static_cast<float>((pix + pjx) / 2);
			e.z = static_cast<float>((piz + pjz) / 2);
			e.y = static_cast<float>(mHalf + D.hop[j] * 4 * uj * (1 - uj));
			e.session = session ? session : D.session[j];
			events.push_back(e);
			return NearestSlot(ex + nx * def, ez + nz * def, false);
		}
		return -1;
	}

	// Per-frame mid-air contacts. Airborne dice are bucketed by the cell they're over, so each
	// die only checks its 3 x 3 neighbourhood. Dice that touch veer apart (a sideways via point)
	// and curve back to their already-claimed slots, so slot ownership and faces are untouched.
	void DiceSim::ResolveMidAir()
	{
		if (!config.midAirCollide || activeCount < 2) return;
		const double R = static_cast<double>(config.dieSize) * config.midAirContact, R2 = R * R, H = config.dieSize * 0.9;
		for (int k = 0; k < activeCount; ++k)
		{
			const int i = active[k];
			if (D.state[i] != S_ROLLING) continue;
			const int c = HashOf(D.px[i], D.pz[i]);
			mBucketCell[i] = c;   // stored: separation moves dice, so recomputing it later would be wrong (B13.8)
			mBucketNext[i] = mBucketHead[c];
			mBucketHead[c] = i;
		}
		for (int k = 0; k < activeCount; ++k)
		{
			const int i = active[k];
			if (D.state[i] != S_ROLLING) continue;
			const int c = mBucketCell[i], cx = c % N, cz = c / N;
			for (int oz = -1; oz <= 1; ++oz)
				for (int ox = -1; ox <= 1; ++ox)
				{
					const int x = cx + ox, z = cz + oz;
					if (x < 0 || z < 0 || x >= N || z >= N) continue;
					for (int j = mBucketHead[z * N + x]; j >= 0; j = mBucketNext[j])
					{
						if (j <= i) continue;   // each pair once
						double nx = D.px[i] - D.px[j], nz = D.pz[i] - D.pz[j];
						const double d2 = nx * nx + nz * nz;
						if (d2 > R2 || std::fabs(D.py[i] - D.py[j]) > H) continue;
						double nl = std::sqrt(d2);
						if (nl < 1e-4) { nx = -(D.ez[i] - D.sz[i]); nz = D.ex[i] - D.sx[i]; nl = std::hypot(nx, nz); if (nl == 0) nl = 1; }
						nx /= nl; nz /= nl;

						// 1) Never interpenetrate: push both apart by half the overlap (every frame).
						const double push = (R - std::min(nl, R)) * 0.5;
						D.offX[i] = static_cast<float>(D.offX[i] + nx * push); D.offZ[i] = static_cast<float>(D.offZ[i] + nz * push);
						D.px[i] = static_cast<float>(D.px[i] + nx * push); D.pz[i] = static_cast<float>(D.pz[i] + nz * push);
						D.offX[j] = static_cast<float>(D.offX[j] - nx * push); D.offZ[j] = static_cast<float>(D.offZ[j] - nz * push);
						D.px[j] = static_cast<float>(D.px[j] - nx * push); D.pz[j] = static_cast<float>(D.pz[j] - nz * push);

						// 2) First touch of this pair: glance off (the path bends), clack and sparks.
						if (D.lastHit[i] == j || D.lastHit[j] == i) continue;
						if (D.hits[i] >= config.midAirMaxHits || D.hits[j] >= config.midAirMaxHits) continue;
						const double ui = (now - D.t0[i]) / D.dur[i], uj = (now - D.t0[j]) / D.dur[j];
						if (ui > 0.92 || uj > 0.92) continue;   // about to land: let it settle
						const bool fresh = config.clashEnabled && !D.clashed[i] && !D.clashed[j];
						Glance(i, ui, nx, nz, j);
						Glance(j, uj, -nx, -nz, i);
						const float mx = (D.px[i] + D.px[j]) / 2, mz = (D.pz[i] + D.pz[j]) / 2, my = mHalf + (D.py[i] + D.py[j]) / 2;
						if (fresh) AwardClash(mx, my, mz, D.session[i] ? D.session[i] : D.session[j]);
						else { Spark(mx, my, mz, 6, 0.9f, 0.9f, 0.85f); soundsOut.push_back(Sound::Knock); }
					}
				}
		}
		for (int k = 0; k < activeCount; ++k)
		{
			const int i = active[k];
			if (D.state[i] == S_ROLLING) mBucketHead[mBucketCell[i]] = -1;
		}
	}

	// Re-plans the rest of die i's path from where it is now: veer along (nx, nz), then land as planned.
	void DiceSim::Glance(int i, double u, double nx, double nz, int other)
	{
		const double h = PathFrac(i, u);
		const double cx = D.px[i], cz = D.pz[i];
		D.sx[i] = static_cast<float>(cx); D.sz[i] = static_cast<float>(cz); D.h0[i] = static_cast<float>(h);
		D.offX[i] = 0; D.offZ[i] = 0;   // the current offset is now baked into the new start point
		D.um[i] = static_cast<float>(h + 0.4 * (1 - h));
		D.mx[i] = static_cast<float>(cx + (D.ex[i] - cx) * 0.4 + nx * config.midAirDeflect);
		D.mz[i] = static_cast<float>(cz + (D.ez[i] - cz) * 0.4 + nz * config.midAirDeflect);
		D.clashed[i] = 1;   // the path isn't straight any more: the launch-time clash maths skips it
		D.hits[i]++;
		D.lastHit[i] = other;
	}

	void DiceSim::UpdateRolls(double dt)
	{
		const double decay = std::exp(-dt * 5);
		int w = 0;
		for (int k = 0; k < activeCount; ++k)
		{
			const int i = active[k];
			if (D.state[i] == S_PENDING)
			{
				if (now < D.t0[i]) { active[w++] = i; continue; }
				D.state[i] = S_ROLLING;
			}
			const double u = (now - D.t0[i]) / D.dur[i];
			if (u >= 1) { D.inActive[i] = 0; Land(i); continue; }

			// Ground path: start -> via -> end (via is the midpoint unless clashed).
			double x, z;
			PathXZ(i, u, x, z);
			D.offX[i] = static_cast<float>(D.offX[i] * decay);
			D.offZ[i] = static_cast<float>(D.offZ[i] * decay);
			const double env = std::min(1.0, (1 - u) / 0.15);   // the offset fades to 0 so landing is exact
			D.px[i] = static_cast<float>(x + D.offX[i] * env);
			D.pz[i] = static_cast<float>(z + D.offZ[i] * env);

			// Tumble: eased so the spin settles into the landing.
			const double ur = 1 - std::pow(1 - u, 1.7);
			const double phi = D.ang[i] * (ur - 1);
			const Quat spin = Quat::AxisAngle({ D.ax[i], 0, D.az[i] }, phi);
			PutQ(D.q.data(), i, spin.Multiply(GetQ(D.qs.data(), i).Slerp(GetQ(D.qe.data(), i), ur)));

			// Hop arc, plus lift so a tilted cube's corner doesn't sink into the board.
			const double lift = mHalf * (std::fabs(std::cos(phi)) + std::fabs(std::sin(phi)) - 1);
			const double bu = D.bu[i], hop = D.hop[i];
			double y;
			if (bu > 0)   // ricochet: the main arc until the hit, then a small hop
			{
				if (u <= bu) { const double a = u * D.arc[i]; y = hop * 4 * a * (1 - a); }
				else
				{
					const double a = bu * D.arc[i], yb = hop * 4 * a * (1 - a), f = (u - bu) / (1 - bu);
					y = yb * (1 - f) + hop * config.bounceHopMult * 4 * f * (1 - f);
				}
			}
			else
			{
				const double h = PathFrac(i, u);   // pushed dice: the same arc shape, front-loaded
				y = hop * 4 * h * (1 - h);
			}
			// Stack heights: climb early when going up, stay high until clear when coming down.
			const double sb = D.sb[i], eb = D.eb[i];
			const double base = eb == sb ? sb : sb + (eb - sb) * (eb > sb ? 1 - (1 - u) * (1 - u) : u * u);
			D.py[i] = static_cast<float>(base + y + lift);
			active[w++] = i;
		}
		activeCount = w;
	}

	void DiceSim::Land(int i)
	{
		D.state[i] = S_COOLDOWN;
		D.timer[i] = now + config.cooldown;
		D.landT[i] = now;
		D.px[i] = D.ex[i];
		D.pz[i] = D.ez[i];
		D.py[i] = D.by[i] = D.eb[i];
		for (int n = 0; n < 4; ++n) D.q[i * 4 + n] = D.qe[i * 4 + n];   // exactly the pre-decided face
		stats.landings++;

		const int face = D.face[i];
		long long pts = static_cast<long long>(face * config.pointsPerPip);
		if (D.bumped[i]) pts = std::llround(pts * config.bumpScoreMult);
		const int lvl = D.level[i];
		const double mult = 1 + config.stackScorePerLevel * lvl;
		if (lvl > 0) pts = std::llround(pts * mult);
		const bool mine = D.session[i] != 0 && D.session[i] == sessionId;
		const bool counts = mine && (phase == Phase::Running || phase == Phase::Finishing);
		if (counts) { score += pts; if (face == 6) sStats.sixes++; }
		if (mine && sessionAirborne > 0) sessionAirborne--;
		std::string text = "+" + std::to_string(pts);
		if (lvl > 0)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), " \xC3\x97%.1f", mult);   // " x1.5" (U+00D7)
			text += buffer;
		}
		uint8_t style = counts ? 0 : Pop_Free;
		if (D.bumped[i]) style |= Pop_Bump;
		if (lvl > 0) style |= Pop_Stack;
		if (face == 6) style |= Pop_Six;
		Popup(D.px[i], 0.9f + D.by[i], D.pz[i], std::move(text), style);
		soundsOut.push_back(Sound::Tick);
	}

	void DiceSim::StartSession()
	{
		sessionId++;
		phase = Phase::Running;
		timeLeft = config.sessionSeconds;
		score = 0;
		sessionAirborne = 0;
		sStats = {};
	}

	void DiceSim::UpdateSession(double dt)
	{
		if (phase == Phase::Running)
		{
			timeLeft -= dt;
			if (timeLeft <= 0) { timeLeft = 0; phase = Phase::Finishing; }
		}
		if (phase == Phase::Finishing && sessionAirborne <= 0
			&& std::none_of(events.begin(), events.end(), [&](const Event& e) { return e.session == sessionId; }))
			phase = Phase::Ended;
	}

	void DiceSim::ProcessEvents()
	{
		for (int k = static_cast<int>(events.size()) - 1; k >= 0; --k)
		{
			const Event e = events[k];
			if (now < e.t) continue;
			events.erase(events.begin() + k);
			if (e.type == Event::Bump) { Spark(e.x, e.y, e.z, 7, 0.9f, 0.9f, 0.85f); soundsOut.push_back(Sound::Knock); continue; }
			if (e.type == Event::Topple)
			{
				const long long bonus = static_cast<long long>(config.toppleBonus * e.n);
				const bool counts = SessionCounts(e.session);
				if (counts) score += bonus;
				Spark(e.x, e.y, e.z, 16, 0.6f, 0.9f, 1.0f);
				soundsOut.push_back(Sound::Clack);
				soundsOut.push_back(Sound::Knock);
				Popup(e.x, e.y + 0.9f, e.z, "TOPPLE \xC3\x97" + std::to_string(e.n) + " +" + std::to_string(bonus), static_cast<uint8_t>(Pop_Topple | (counts ? 0 : Pop_Free)));
				continue;
			}
			AwardClash(e.x, e.y, e.z, e.session);
		}
	}

	void DiceSim::AwardClash(float x, float y, float z, int session)
	{
		stats.clashes++;
		const bool counts = SessionCounts(session);
		if (counts) { score += static_cast<long long>(config.clashBonus); sStats.clashes++; }
		Spark(x, y, z);
		soundsOut.push_back(Sound::Clack);
		Popup(x, y + 0.8f, z, "CLASH +" + std::to_string(static_cast<long long>(config.clashBonus)), static_cast<uint8_t>(Pop_Clash | (counts ? 0 : Pop_Free)));
	}

	void DiceSim::Frame(double realDt, bool inside, double hitX, double hitZ)
	{
		const double dt = std::min(0.05, std::max(0.0, realDt));
		now += dt;
		bumpTokens = std::min<double>(config.bumpsPerSecond, bumpTokens + dt * config.bumpsPerSecond);
		UpdateSession(dt);
		ProcessEvents();
		UpdateHover(inside, hitX, hitZ);
		if (stormPerSecond > 0 && diceCount > 0)
		{
			mStormCarry += stormPerSecond * diceCount * dt;
			const double lo = std::min(config.rollDistMin, config.rollDistMax), hi = std::max(config.rollDistMin, config.rollDistMax);
			for (; mStormCarry >= 1; mStormCarry -= 1)
			{
				const int i = static_cast<int>(rand() * diceCount);
				if (D.state[i] == S_READY && slotOwner[D.cell[i]] == i) Launch(i, rand() * kPi * 2, rand.Range(lo, hi), 0, 0, 0, 0);
			}
		}
		UpdateRolls(dt);
		ResolveMidAir();
		ExpireCooldowns();
	}

	void DiceSim::UpdateHover(bool inside, double hitX, double hitZ)
	{
		hoverVisible = false;
		hoverCell = -1;
		if (!inside) return;
		hoverVisible = true;
		hoverX = hitX;
		hoverZ = hitZ;
		if (phase == Phase::Finishing) return;   // the ring shows (grey), nothing launches

		const double R = config.hoverRadius, R2 = R * R;
		const double lo = std::min(config.rollDistMin, config.rollDistMax), hi = std::max(config.rollDistMin, config.rollDistMax);
		const int session = phase == Phase::Running ? sessionId : 0;
		if (std::fabs(hitX) < N / 2.0 && std::fabs(hitZ) < N / 2.0)
		{
			const int s = NearestSlot(hitX, hitZ, false);
			if (s >= 0 && (slotX[s] - hitX) * (slotX[s] - hitX) + (slotZ[s] - hitZ) * (slotZ[s] - hitZ) < 1) hoverCell = s;
		}
		// Visit only the hash cells under the ring, then test each slot's distance.
		const int x0 = HashCoord(hitX - R), x1 = HashCoord(hitX + R);
		const int z0 = HashCoord(hitZ - R), z1 = HashCoord(hitZ + R);
		for (int cz = z0; cz <= z1; ++cz)
			for (int cx = x0; cx <= x1; ++cx)
				for (int s = hashHead[cz * N + cx]; s >= 0; s = hashNext[s])
				{
					const double dx = slotX[s] - hitX, dz = slotZ[s] - hitZ;
					if (dx * dx + dz * dz > R2) continue;
					const int o = slotOwner[s];
					if (o < 0 || D.state[o] != S_READY) continue;
					++hoverResponses;
					Launch(o, rand() * kPi * 2, rand.Range(lo, hi), 0, 0, 0, session);
				}
	}

	void DiceSim::ExpireCooldowns()
	{
		for (int i = 0; i < diceCount; ++i)
			if (D.state[i] == S_COOLDOWN && now >= D.timer[i]) { D.state[i] = S_READY; D.readyT[i] = now; }
	}

	void DiceSim::DieColor(int i, float& r, float& g, float& b) const
	{
		r = g = b = 1;
		const uint8_t st = D.state[i];
		if (st == S_COOLDOWN) r = g = b = 0.6f;
		else if (st == S_READY)
		{
			const float glowT = std::max(0.01f, config.readyGlowTime);
			const float gl = static_cast<float>(1 - (now - D.readyT[i]) / glowT);
			if (gl > 0) { r = 1 + 1.2f * gl; g = 1 + 0.8f * gl; b = 1 - 0.4f * gl; }
		}
	}

	void DiceSim::BuildBoard(int forcedFace)
	{
		mHalf = config.dieSize / 2;
		N = std::clamp(config.boardSize, 1, kMaxProfileSide);
		if (CapacityFor(N) > mCapacity) Reserve(CapacityFor(N));   // every array is rewritten below
		GenerateSlots();
		std::fill(slotOwner.begin(), slotOwner.end(), -1);
		diceCount = 0;
		hoverCell = -1;
		activeCount = 0;
		std::fill(D.inActive.begin(), D.inActive.end(), uint8_t(0));
		events.clear();
		sessionAirborne = 0;
		popupsOut.clear();
		sparksOut.clear();
		soundsOut.clear();
		for (int c = 0; c < slotCount; ++c)
		{
			if (rand() >= config.density) continue;
			const int i = diceCount++;
			slotOwner[c] = i;
			D.cell[i] = c;
			D.px[i] = static_cast<float>(slotX[c] + Jit());
			D.pz[i] = static_cast<float>(slotZ[c] + Jit());
			D.py[i] = 0;
			D.face[i] = static_cast<uint8_t>(1 + static_cast<int>(rand() * 6));
			if (forcedFace >= 1 && forcedFace <= 6) D.face[i] = static_cast<uint8_t>(forcedFace);
			PutQ(D.q.data(), i, RestQuat(D.face[i], rand() * kPi * 2));
			D.state[i] = S_READY;
			D.landT[i] = -10; D.readyT[i] = -10; D.hitT[i] = -10;
			D.below[i] = -1; D.level[i] = 0; D.by[i] = 0;
		}
	}
}
