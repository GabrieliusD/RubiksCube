#include "Valley.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
	float Smooth(float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	float Hash01(int x, int z, uint32_t salt)
	{
		uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u + salt * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
	}

	constexpr int kRiverPointCount = static_cast<int>(std::size(cfg::kRiverPoints));
	constexpr float kMouthX = cfg::kCenterX + 0.5f;
}

Valley::Valley() : mNoise(0x5A11E7ull)
{
	// River length along its points.
	mRiverLength.resize(kRiverPointCount, 0.0f);
	for (int i = 1; i < kRiverPointCount; ++i)
	{
		const float dx = cfg::kRiverPoints[i][0] - cfg::kRiverPoints[i - 1][0], dz = cfg::kRiverPoints[i][1] - cfg::kRiverPoints[i - 1][1];
		mRiverLength[i] = mRiverLength[i - 1] + std::sqrt(dx * dx + dz * dz);
	}
	// Water level every metre: a little under the ground along the centre line, never rising
	// downstream, in whole blocks, so the river steps down in small waterfalls.
	const int samples = static_cast<int>(mRiverLength.back()) + 2;
	mRiverLevel.resize(samples);
	float level = 1e9f;
	int segment = 0;
	for (int s = 0; s < samples; ++s)
	{
		const float along = std::min(static_cast<float>(s), mRiverLength.back());
		while (segment + 2 < kRiverPointCount && mRiverLength[segment + 1] < along) ++segment;
		const float t = (along - mRiverLength[segment]) / std::max(mRiverLength[segment + 1] - mRiverLength[segment], 1e-3f);
		const float x = cfg::kRiverPoints[segment][0] + (cfg::kRiverPoints[segment + 1][0] - cfg::kRiverPoints[segment][0]) * t;
		const float z = cfg::kRiverPoints[segment][1] + (cfg::kRiverPoints[segment + 1][1] - cfg::kRiverPoints[segment][1]) * t;
		level = std::min(level, BaseHeight(x, z) - 0.6f);
		mRiverLevel[s] = std::floor(level * cfg::kVoxelsPerMeter) / cfg::kVoxelsPerMeter;
	}
	mLakeLevel = std::floor((BaseHeight(cfg::kLakeCenter[0], cfg::kLakeCenter[1]) - 0.5f) * cfg::kVoxelsPerMeter) / cfg::kVoxelsPerMeter;
}

bool Valley::OverCave(float x, float z)
{
	return x >= 0.0f && x < cfg::kGridX && z >= 0.0f && z < cfg::kGridZ;
}

float Valley::ValleyDistance(float x, float z) const
{
	const float dx = (x - cfg::kValleyCenter[0]) / cfg::kValleyRadius[0];
	const float dz = (z - cfg::kValleyCenter[1]) / cfg::kValleyRadius[1];
	const float a = std::atan2(dz, dx);
	const float wobble = 1.0f + 0.07f * std::sin(3.0f * a + 0.5f) + 0.05f * std::sin(5.0f * a + 1.3f) + 0.06f * Noise2(x / 70.0f, z / 70.0f, 2);
	return std::sqrt(dx * dx + dz * dz) * wobble;
}

float Valley::BaseHeight(float x, float z) const
{
	const float e = ValleyDistance(x, z);

	// The valley floor: rolling ground that rises into foothills at the edge, flat around the
	// cave mouth.
	float floor = cfg::kGroundY + cfg::kValleyFloorBumps * Noise2(x / 75.0f, z / 75.0f, 3)
		+ cfg::kValleyFoothills * Smooth(0.55f, 1.0f, e);
	const float toMouth = std::sqrt((x - kMouthX) * (x - kMouthX) + z * z);
	floor = cfg::kGroundY + (floor - cfg::kGroundY) * Smooth(cfg::kValleyFlatAtMouth, cfg::kValleyFlatAtMouth + 35.0f, toMouth);

	// The ring of mountains.
	const float ring = Smooth(0.9f, 1.45f, e) * cfg::kMountainHeight * (0.55f + 0.9f * (0.5f + 0.5f * Noise2(x / 120.0f, z / 120.0f, 3)));

	// The mountain the cave is in, north of the mouth: cliffs rising from z = 0.
	const float g = std::exp(-((x - kMouthX) / 85.0f) * ((x - kMouthX) / 85.0f));
	const float north = Smooth(-1.0f, 8.0f, z) * (cfg::kMountainOverCave + 0.3f * std::max(z, 0.0f)) * (0.5f + 0.5f * g);

	float h = std::max({ floor, ring, north });
	// Rough rock wherever the ground rises above the floor.
	const float rise = std::clamp((h - floor) / 12.0f, 0.0f, 1.0f);
	h += rise * 7.0f * Noise2(x / 22.0f, z / 22.0f, 3);
	// Strata: steep ground breaks into layers, each a riser and a near-flat ledge (the
	// ground picks grass on the ledges and rock on the risers), like the cave's face.
	const float strata = Smooth(cfg::kStrataFrom, cfg::kStrataFull, rise);
	if (strata > 0.0f)
	{
		const float thickness = cfg::kStrataHeight * (1.0f + 0.3f * Noise2(x / 45.0f, z / 45.0f, 2));
		const float warp = cfg::kStrataWarp * Noise2(x / 13.0f + 7.0f, z / 13.0f, 2);
		const float s = (h + warp) / thickness;
		const float layer = std::floor(s);
		const float shaped = (layer + std::pow(s - layer, cfg::kStrataSharpness)) * thickness - warp;
		h += (shaped - h) * strata;
	}
	if (OverCave(x, z))
	{
		// The natural shafts open through the mountain top: no ground over them (down to the
		// grid's top, where their grates rest).
		for (const cfg::Shaft& s : cfg::kShafts)
			if ((x - s.x) * (x - s.x) + (z - s.z) * (z - s.z) < (cfg::kShaftRadius + cfg::kShaftHoleMargin) * (cfg::kShaftRadius + cfg::kShaftHoleMargin))
				return static_cast<float>(cfg::kGridY);
		return std::max(h, cfg::kGridY + 2.0f + 2.0f * (0.5f + 0.5f * Noise2(x / 9.0f, z / 9.0f, 2)));
	}

	// Around the cave's face (the grid's front, which the cave generator shapes into rock):
	// buttresses that stand over its side edges and bury the grid's sides, and rubble at its
	// foot, except on the way in.
	const float outside = std::max({ cfg::kButtressInset - x, x - (cfg::kGridX - cfg::kButtressInset), 0.0f });
	const float front = -cfg::kButtressFront * (0.65f + 0.35f * Noise2(x / 4.0f, 3.7f, 2));
	if (outside > 0.0f && z >= front && z < cfg::kGridZ)
	{
		const float drop = std::max(0.0f, outside - cfg::kButtressInset - 1.5f) * cfg::kButtressFall;
		h = std::max(h, cfg::kGridY + 2.0f + 1.5f * Noise2(x / 6.0f, z / 6.0f, 2) - drop);
	}
	if (z < 0.0f && z > -cfg::kTalusReach && x > 0.0f && x < cfg::kGridX)
	{
		const float off = std::fabs(x - kMouthX) - cfg::kMouthPathHalfWidth;
		if (off > 0.0f)
		{
			const float t = 1.0f + z / cfg::kTalusReach;
			h = std::max(h, cfg::kGroundY + cfg::kTalusHeight * t * t * (0.6f + 0.4f * Noise2(x / 2.5f, z / 2.5f, 2)) * Smooth(0.0f, 2.0f, off));
		}
	}
	return h;
}

float Valley::RiverDistance(float x, float z, float* along) const
{
	float best = 1e9f, bestAlong = 0.0f;
	for (int i = 0; i + 1 < kRiverPointCount; ++i)
	{
		const float ax = cfg::kRiverPoints[i][0], az = cfg::kRiverPoints[i][1];
		const float vx = cfg::kRiverPoints[i + 1][0] - ax, vz = cfg::kRiverPoints[i + 1][1] - az;
		const float len2 = vx * vx + vz * vz;
		const float t = std::clamp(((x - ax) * vx + (z - az) * vz) / len2, 0.0f, 1.0f);
		const float dx = x - ax - vx * t, dz = z - az - vz * t;
		const float d = std::sqrt(dx * dx + dz * dz);
		if (d < best)
		{
			best = d;
			bestAlong = mRiverLength[i] + t * std::sqrt(len2);
		}
	}
	// A slightly wandering edge (only near the river, where it matters).
	if (best < cfg::kRiverHalfWidth + cfg::kRiverBank + 3.0f) best += 0.8f * Noise2(x / 9.0f, z / 9.0f, 1);
	if (along) *along = bestAlong;
	return best;
}

void Valley::RiverPointAt(float along, float& x, float& z) const
{
	for (int i = 0; i + 1 < kRiverPointCount; ++i)
	{
		const float length = mRiverLength[i + 1] - mRiverLength[i];
		if (along <= mRiverLength[i + 1] || i + 2 == kRiverPointCount)
		{
			const float t = std::clamp((along - mRiverLength[i]) / std::max(length, 1e-3f), 0.0f, 1.0f);
			x = cfg::kRiverPoints[i][0] + (cfg::kRiverPoints[i + 1][0] - cfg::kRiverPoints[i][0]) * t;
			z = cfg::kRiverPoints[i][1] + (cfg::kRiverPoints[i + 1][1] - cfg::kRiverPoints[i][1]) * t;
			return;
		}
	}
	x = cfg::kRiverPoints[0][0];
	z = cfg::kRiverPoints[0][1];
}

float Valley::RiverLevelAt(float along) const
{
	const int i = std::clamp(static_cast<int>(along), 0, static_cast<int>(mRiverLevel.size()) - 1);
	return mRiverLevel[i];
}

void Valley::Sample(float x, float z, float& height, float& water) const
{
	height = BaseHeight(x, z);
	water = kNoWater;
	if (OverCave(x, z)) return;

	// Building pads, later ones over earlier ones.
	for (const GroundPad& pad : mPads)
	{
		const float d = std::max(std::fabs(x - pad.x) - pad.halfX, std::fabs(z - pad.z) - pad.halfZ);
		if (d < cfg::kPadBlend) height = pad.height + (height - pad.height) * Smooth(0.0f, cfg::kPadBlend, d);
	}

	float along = 0.0f;
	const float d = RiverDistance(x, z, &along);
	if (d < cfg::kRiverHalfWidth + cfg::kRiverBank)
	{
		const float level = RiverLevelAt(along);
		if (d < cfg::kRiverHalfWidth) height = std::min(height, level - cfg::kRiverDepth);
		else height = std::min(height, level + 0.25f + (height - level - 0.25f) * Smooth(cfg::kRiverHalfWidth, cfg::kRiverHalfWidth + cfg::kRiverBank, d));
		if (d < cfg::kRiverHalfWidth + 0.5f) water = level;
		// A low lip along the bank where the land beside it is lower than the water (no
		// standing walls of water), sloping back down into the land.
		if (d >= cfg::kRiverHalfWidth) height = std::max(height, level + 0.25f - 0.6f * std::max(0.0f, d - cfg::kRiverHalfWidth - 1.0f));
	}

	const float lx = (x - cfg::kLakeCenter[0]) / cfg::kLakeRadius[0], lz = (z - cfg::kLakeCenter[1]) / cfg::kLakeRadius[1];
	const float el0 = std::sqrt(lx * lx + lz * lz);
	if (el0 < 1.6f)
	{
		const float el = el0 * (1.0f + 0.08f * Noise2(x / 12.0f, z / 12.0f, 1));
		if (el < 1.0f) height = std::min(height, mLakeLevel - cfg::kLakeDepth * Smooth(1.0f, 0.5f, el) - 0.5f);
		else if (el < 1.4f) height = std::min(height, mLakeLevel + 0.25f + (height - mLakeLevel - 0.25f) * Smooth(1.0f, 1.4f, el));
		if (el < 1.05f && water == kNoWater) water = mLakeLevel;
		if (el >= 1.0f) height = std::max(height, mLakeLevel + 0.25f - 0.6f * std::max(0.0f, (el - 1.0f) * cfg::kLakeRadius[1] - 1.0f));
	}
}

float Valley::Height(float x, float z) const
{
	float height, water;
	Sample(x, z, height, water);
	return height;
}

float Valley::WaterLevel(float x, float z) const
{
	float height, water;
	Sample(x, z, height, water);
	return water;
}

bool Valley::Detailed(float x, float z) const
{
	const float c = cfg::kTerrainChunk;
	const float cx = cfg::kValleyWorld[0] + (std::floor((x - cfg::kValleyWorld[0]) / c) + 0.5f) * c;
	const float cz = cfg::kValleyWorld[1] + (std::floor((z - cfg::kValleyWorld[1]) / c) + 0.5f) * c;
	const float mx = cx - kMouthX;
	if (ValleyDistance(cx, cz) < cfg::kDetailValleyDistance || std::sqrt(mx * mx + cz * cz) < cfg::kDetailMouthReach) return true;
	return RiverChunk(x, z);
}

bool Valley::RiverChunk(float x, float z) const
{
	const float c = cfg::kTerrainChunk;
	const float cx = cfg::kValleyWorld[0] + (std::floor((x - cfg::kValleyWorld[0]) / c) + 0.5f) * c;
	const float cz = cfg::kValleyWorld[1] + (std::floor((z - cfg::kValleyWorld[1]) / c) + 0.5f) * c;
	return RiverDistance(cx, cz) < c * 0.75f + cfg::kRiverHalfWidth + cfg::kRiverBank;
}

Ground Valley::GroundAt(float x, float z, float height, float drop, float block) const
{
	if (WaterLevel(x, z) > height) return Ground::Gravel;
	// Steep enough for rock: the same slope whatever the block size (1.6 m over 1 m blocks).
	const float steep = block < 0.3f ? cfg::kRockSlope : (block < 0.75f ? 0.9f : 1.6f);
	const float n = Noise2(x / 14.0f, z / 14.0f, 2);
	if (drop >= steep) return n > 0.0f ? Ground::Rock : Ground::Rock2;
	if (height > cfg::kSnowLine + 10.0f * n) return Ground::Snow;
	if (height > cfg::kTreeLine + 6.0f * n) return n > 0.15f ? Ground::Rock2 : Ground::Grass2;
	if (!OverCave(x, z))
	{
		// Trails: worn ground with ragged edges.
		if (PathDistance(x, z) < cfg::kPathHalfWidth + 0.4f * Noise2(x / 3.0f, z / 3.0f, 1)) return Ground::Path;
		if (RiverDistance(x, z) < cfg::kRiverHalfWidth + 1.5f) return Ground::Sand;
		const float lx = (x - cfg::kLakeCenter[0]) / cfg::kLakeRadius[0], lz = (z - cfg::kLakeCenter[1]) / cfg::kLakeRadius[1];
		if (std::sqrt(lx * lx + lz * lz) < 1.15f) return Ground::Sand;
	}
	// Under the woods: a mossy forest floor, with a ragged edge.
	if (height < cfg::kTreeLine && ForestPatch(x, z) + 0.12f * Noise2(x / 4.0f, z / 4.0f, 1) > cfg::kMossPatch && TreeAllowed(x, z)) return Ground::Moss;
	return Noise2(x / 30.0f + 11.0f, z / 30.0f, 2) > 0.1f ? Ground::Grass2 : Ground::Grass;
}

float Valley::ForestPatch(float x, float z) const
{
	return Noise2(x / 55.0f + 3.0f, z / 55.0f, 2) + 0.35f * Smooth(0.4f, 1.0f, ValleyDistance(x, z));
}

float Valley::PathDistance(float x, float z)
{
	float best = 1e9f;
	for (const auto& seg : cfg::kPathSegments)
	{
		const float vx = seg[2] - seg[0], vz = seg[3] - seg[1];
		const float t = std::clamp(((x - seg[0]) * vx + (z - seg[1]) * vz) / (vx * vx + vz * vz), 0.0f, 1.0f);
		const float dx = x - seg[0] - vx * t, dz = z - seg[1] - vz * t;
		best = std::min(best, std::sqrt(dx * dx + dz * dz));
	}
	return best;
}

bool Valley::TreeAllowed(float x, float z) const
{
	if (PathDistance(x, z) < cfg::kPathHalfWidth + 2.5f) return false;
	if (OverCave(x, z) || (z > -2.0f && std::fabs(x - kMouthX) < 40.0f)) return false;
	for (const auto& c : cfg::kClearings)
	{
		const float dx = x - c[0], dz = z - c[1];
		if (dx * dx + dz * dz < c[2] * c[2]) return false;
	}
	if (RiverDistance(x, z) < cfg::kRiverHalfWidth + cfg::kRiverBank + 2.0f) return false;
	const float lx = (x - cfg::kLakeCenter[0]) / cfg::kLakeRadius[0], lz = (z - cfg::kLakeCenter[1]) / cfg::kLakeRadius[1];
	if (std::sqrt(lx * lx + lz * lz) < 1.6f) return false;
	const float h = BaseHeight(x, z);
	if (h > cfg::kTreeLine) return false;
	const float slope = std::max(std::fabs(BaseHeight(x + 1.5f, z) - h), std::fabs(BaseHeight(x, z + 1.5f) - h));
	return slope < 1.2f;
}

std::vector<OutcropSpot> Valley::Outcrops() const
{
	std::vector<OutcropSpot> spots;
	auto scatter = [&](float spacing, bool distant)
	{
		const uint32_t salt = distant ? 31u : 23u;
		const int ix0 = static_cast<int>(std::floor(cfg::kValleyWorld[0] / spacing)), ix1 = static_cast<int>(std::ceil(cfg::kValleyWorld[2] / spacing));
		const int iz0 = static_cast<int>(std::floor(cfg::kValleyWorld[1] / spacing)), iz1 = static_cast<int>(std::ceil(cfg::kValleyWorld[3] / spacing));
		for (int iz = iz0; iz < iz1; ++iz)
			for (int ix = ix0; ix < ix1; ++ix)
			{
				if (Hash01(ix, iz, salt) > cfg::kOutcropChance) continue;
				const float x = (ix + 0.15f + 0.7f * Hash01(ix, iz, salt + 1)) * spacing;
				const float z = (iz + 0.15f + 0.7f * Hash01(ix, iz, salt + 2)) * spacing;
				if (Detailed(x, z) == distant) continue;
				if (x < cfg::kValleyWorld[0] || x >= cfg::kValleyWorld[2] || z < cfg::kValleyWorld[1] || z >= cfg::kValleyWorld[3]) continue;
				// Not on the cave's face, the trails, the clearings or in water.
				if (z > -14.0f && z < 14.0f && x > -12.0f && x < cfg::kGridX + 12.0f) continue;
				if (PathDistance(x, z) < cfg::kPathHalfWidth + 3.0f || WaterLevel(x, z) != kNoWater) continue;
				bool clear = true;
				for (const auto& c : cfg::kClearings) clear &= (x - c[0]) * (x - c[0]) + (z - c[1]) * (z - c[1]) >= c[2] * c[2];
				if (!clear) continue;
				// Steep ground only.
				const float slope = std::fabs(Height(x + 2.0f, z) - Height(x - 2.0f, z)) + std::fabs(Height(x, z + 2.0f) - Height(x, z - 2.0f));
				if (slope < cfg::kOutcropSlope) continue;
				OutcropSpot spot;
				spot.x = x;
				spot.z = z;
				spot.distant = distant;
				spot.yaw = Hash01(ix, iz, salt + 3) * 6.2831853f;
				const float r = Hash01(ix, iz, salt + 4);
				spot.kind = r < 0.45f ? 0 : r < 0.8f ? 1 : 2;
				spots.push_back(spot);
			}
	};
	scatter(cfg::kOutcropSpacing, false);
	scatter(cfg::kOutcropSpacingFar, true);
	return spots;
}

std::vector<TreeSpot> Valley::Trees() const
{
	std::vector<TreeSpot> trees;
	auto scatter = [&](float spacing, bool distant)
	{
		const int ix0 = static_cast<int>(std::floor(cfg::kValleyWorld[0] / spacing)), ix1 = static_cast<int>(std::ceil(cfg::kValleyWorld[2] / spacing));
		const int iz0 = static_cast<int>(std::floor(cfg::kValleyWorld[1] / spacing)), iz1 = static_cast<int>(std::ceil(cfg::kValleyWorld[3] / spacing));
		const uint32_t salt = distant ? 7u : 3u;
		for (int iz = iz0; iz < iz1; ++iz)
		{
			for (int ix = ix0; ix < ix1; ++ix)
			{
				const float x = (ix + 0.15f + 0.7f * Hash01(ix, iz, salt)) * spacing;
				const float z = (iz + 0.15f + 0.7f * Hash01(ix, iz, salt + 1)) * spacing;
				const bool inFine = Detailed(x, z);
				const bool inWorld = x >= cfg::kValleyWorld[0] && x < cfg::kValleyWorld[2] && z >= cfg::kValleyWorld[1] && z < cfg::kValleyWorld[3];
				if (inFine == distant || !inWorld) continue;
				// Woods in patches, thick towards the valley's edge.
				const float patch = ForestPatch(x, z);
				if (patch < -0.15f || Hash01(ix, iz, salt + 2) > 0.55f + patch) continue;
				if (!TreeAllowed(x, z)) continue;
				TreeSpot tree;
				tree.x = x;
				tree.z = z;
				tree.distant = distant;
				tree.yaw = Hash01(ix, iz, salt + 3) * 6.2831853f;
				const float r = Hash01(ix, iz, salt + 4);
				const bool broadleaf = !distant && BaseHeight(x, z) < cfg::kGroundY + 6.0f && Noise2(x / 40.0f, z / 40.0f + 5.0f, 1) > 0.25f;
				tree.kind = broadleaf ? 3 : (r < 0.3f ? 0 : r < 0.75f ? 1 : 2);
				trees.push_back(tree);
			}
		}
	};
	scatter(cfg::kForestSpacing, false);
	scatter(cfg::kForestSpacingFar, true);
	return trees;
}
