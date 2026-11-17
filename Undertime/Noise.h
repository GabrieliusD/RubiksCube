#pragma once
// Seeded 3D gradient (Perlin) noise with fBm, plus a lattice cache for sampling a
// noise field cheaply at millions of voxels.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

class Perlin3
{
public:
	explicit Perlin3(uint64_t seed)
	{
		std::iota(mPerm, mPerm + 256, 0);
		std::mt19937_64 rng(seed);
		std::shuffle(mPerm, mPerm + 256, rng);
		for (int i = 0; i < 256; ++i) mPerm[256 + i] = mPerm[i];
	}

	// Roughly in [-1, 1].
	float Noise(float x, float y, float z) const
	{
		const int X = FastFloor(x), Y = FastFloor(y), Z = FastFloor(z);
		x -= X; y -= Y; z -= Z;
		const int xi = X & 255, yi = Y & 255, zi = Z & 255;
		const float u = Fade(x), v = Fade(y), w = Fade(z);
		const int A = mPerm[xi] + yi, AA = mPerm[A] + zi, AB = mPerm[A + 1] + zi;
		const int B = mPerm[xi + 1] + yi, BA = mPerm[B] + zi, BB = mPerm[B + 1] + zi;
		return Lerp(w,
			Lerp(v, Lerp(u, Grad(mPerm[AA], x, y, z), Grad(mPerm[BA], x - 1, y, z)),
				Lerp(u, Grad(mPerm[AB], x, y - 1, z), Grad(mPerm[BB], x - 1, y - 1, z))),
			Lerp(v, Lerp(u, Grad(mPerm[AA + 1], x, y, z - 1), Grad(mPerm[BA + 1], x - 1, y, z - 1)),
				Lerp(u, Grad(mPerm[AB + 1], x, y - 1, z - 1), Grad(mPerm[BB + 1], x - 1, y - 1, z - 1))));
	}

	// Fractal sum, normalised to roughly [-1, 1].
	float Fbm(float x, float y, float z, int octaves, float lacunarity = 2.0f, float gain = 0.5f) const
	{
		float sum = 0.0f, amplitude = 1.0f, norm = 0.0f;
		for (int i = 0; i < octaves; ++i)
		{
			sum += amplitude * Noise(x, y, z);
			norm += amplitude;
			x *= lacunarity; y *= lacunarity; z *= lacunarity;
			amplitude *= gain;
		}
		return sum / norm;
	}

private:
	static int FastFloor(float v) { const int i = static_cast<int>(v); return v < i ? i - 1 : i; }
	static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
	static float Lerp(float t, float a, float b) { return a + t * (b - a); }
	static float Grad(int hash, float x, float y, float z)
	{
		const int h = hash & 15;
		const float u = h < 8 ? x : y;
		const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
		return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
	}

	int mPerm[512];
};

// A noise field sampled once per lattice point (every `spacing` metres) over a box and
// trilinearly interpolated, so per-voxel lookups cost a few multiplies.
class NoiseLattice
{
public:
	template <typename Fn>
	void Build(float sizeX, float sizeY, float sizeZ, float spacing, Fn&& sample)
	{
		mSpacing = spacing;
		mNx = static_cast<int>(std::ceil(sizeX / spacing)) + 2;
		mNy = static_cast<int>(std::ceil(sizeY / spacing)) + 2;
		mNz = static_cast<int>(std::ceil(sizeZ / spacing)) + 2;
		mValues.resize(static_cast<size_t>(mNx) * mNy * mNz);
		for (int z = 0; z < mNz; ++z)
			for (int y = 0; y < mNy; ++y)
				for (int x = 0; x < mNx; ++x)
					mValues[Index(x, y, z)] = sample(x * spacing, y * spacing, z * spacing);
	}

	// Position in metres.
	float Sample(float x, float y, float z) const
	{
		const float fx = std::clamp(x / mSpacing, 0.0f, mNx - 1.001f);
		const float fy = std::clamp(y / mSpacing, 0.0f, mNy - 1.001f);
		const float fz = std::clamp(z / mSpacing, 0.0f, mNz - 1.001f);
		const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), z0 = static_cast<int>(fz);
		const float tx = fx - x0, ty = fy - y0, tz = fz - z0;
		auto at = [&](int dx, int dy, int dz) { return mValues[Index(x0 + dx, y0 + dy, z0 + dz)]; };
		const float c00 = at(0, 0, 0) + (at(1, 0, 0) - at(0, 0, 0)) * tx;
		const float c10 = at(0, 1, 0) + (at(1, 1, 0) - at(0, 1, 0)) * tx;
		const float c01 = at(0, 0, 1) + (at(1, 0, 1) - at(0, 0, 1)) * tx;
		const float c11 = at(0, 1, 1) + (at(1, 1, 1) - at(0, 1, 1)) * tx;
		const float c0 = c00 + (c10 - c00) * ty;
		const float c1 = c01 + (c11 - c01) * ty;
		return c0 + (c1 - c0) * tz;
	}

private:
	size_t Index(int x, int y, int z) const { return static_cast<size_t>(x) + static_cast<size_t>(mNx) * (y + static_cast<size_t>(mNy) * z); }

	std::vector<float> mValues;
	float mSpacing = 1.0f;
	int mNx = 0, mNy = 0, mNz = 0;
};
