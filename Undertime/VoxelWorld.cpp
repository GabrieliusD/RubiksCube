#include "VoxelWorld.h"
#include "ColorUtil.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

namespace
{
	constexpr int V = cfg::kVoxelsPerMeter;
	constexpr float S = cfg::kVoxelSize;

	// Palette layout: 0 = air, then the stone depth bands, the bedrock bands, then one entry per ore.
	constexpr int kStoneFirst = 1;
	constexpr int kBedrockFirst = kStoneFirst + cfg::kStoneBands;
	constexpr int kOreFirst = kBedrockFirst + cfg::kStoneBands;
	// Then two cracked copies (stone bands + ores) for damaged voxels.
	constexpr int kCrackSet = cfg::kStoneBands + static_cast<int>(Block::Count);
	constexpr int kCrackFirst = kOreFirst + static_cast<int>(Block::Count);
	static_assert(kCrackFirst + 2 * kCrackSet <= 256, "palette overflow");

	int DepthBand(int voxelZ)
	{
		return std::clamp(voxelZ * cfg::kStoneBands / cfg::kVoxZ, 0, cfg::kStoneBands - 1);
	}

	XMFLOAT3 StoneSrgb(float depth01)
	{
		return color::Lerp(color::Srgb(cfg::kStoneNear), color::Srgb(cfg::kStoneDeep), std::clamp(depth01, 0.0f, 1.0f));
	}
}

void VoxelWorld::GenerateBasic()
{
	mVoxels.assign(static_cast<size_t>(cfg::kVoxX) * cfg::kVoxY * cfg::kVoxZ, Block::Stone);

	// The layout is defined in metres; every voxel takes the material of its metre cell.
	for (int z = 0; z < cfg::kVoxZ; ++z)
	for (int y = 0; y < cfg::kVoxY; ++y)
	for (int x = 0; x < cfg::kVoxX; ++x)
	{
		const int mx = x / V, my = y / V, mz = z / V;
		Block block = Block::Stone;

		const bool bedrock = my == 0 || my == cfg::kGridY - 1 || mx == 0 || mx == cfg::kGridX - 1 || mz == cfg::kGridZ - 1;
		const bool entrance = mx == cfg::kCenterX && my >= 1 && my <= cfg::kEntranceHeight && mz >= cfg::kEntranceZ0 && mz <= cfg::kEntranceZ1;
		const bool chamber = std::abs(mx - cfg::kCenterX) <= cfg::kChamberHalfWidth && my >= 1 && my <= cfg::kChamberHeight
			&& mz >= cfg::kChamberZ0 && mz <= cfg::kChamberZ1;

		if (bedrock) block = Block::Bedrock;
		else if (entrance || chamber) block = Block::Air;
		mVoxels[Index(x, y, z)] = block;
	}

	// Ore veins and relics are placed with the seeded RNG in milestone 2.
}

bool VoxelWorld::InGrid(int x, int y, int z)
{
	return x >= 0 && x < cfg::kVoxX && y >= 0 && y < cfg::kVoxY && z >= 0 && z < cfg::kVoxZ;
}

VoxelWorld::Block VoxelWorld::Get(int x, int y, int z) const
{
	return InGrid(x, y, z) ? mVoxels[Index(x, y, z)] : Block::Air;
}

void VoxelWorld::Set(int x, int y, int z, Block block)
{
	if (!InGrid(x, y, z)) return;
	mVoxels[Index(x, y, z)] = block;
	if (!mDamage.empty()) mDamage.erase(static_cast<uint32_t>(Index(x, y, z)));
}

float VoxelWorld::Damage(int x, int y, int z) const
{
	if (mDamage.empty() || !InGrid(x, y, z)) return 0.0f;
	auto it = mDamage.find(static_cast<uint32_t>(Index(x, y, z)));
	return it == mDamage.end() ? 0.0f : it->second;
}

int VoxelWorld::CrackLevel(int x, int y, int z) const
{
	// Cracks spread as a speckle: more voxels darken (and some darken further) as damage grows.
	const float d = Damage(x, y, z);
	if (d <= 0.0f) return 0;
	const float h = color::Hash01(x, y, z, 977u);
	if (h < (d - 0.45f) * 1.6f) return 2;
	return h < d * 1.7f ? 1 : 0;
}

bool VoxelWorld::IsSolid(int x, int y, int z) const
{
	if (z >= 0)
	{
		if (InGrid(x, y, z)) return mVoxels[Index(x, y, z)] != Block::Air;
		return true;   // solid mountain rock all around the grid
	}
	// Outside: ground below y = 1 m, invisible walls beyond the play area.
	if (y < static_cast<int>(cfg::kGroundY * V)) return true;
	const float cx = (x + 0.5f) * S - (cfg::kCenterX + 0.5f);
	const float cz = (z + 0.5f) * S;
	return std::fabs(cx) > cfg::kOutsideExtent || cz < -cfg::kOutsideExtent;
}

bool VoxelWorld::BoxOverlapsSolid(const XMFLOAT3& mn, const XMFLOAT3& mx) const
{
	const float eps = 1e-4f;
	const int x0 = static_cast<int>(std::floor(mn.x * V)), x1 = static_cast<int>(std::floor(mx.x * V - eps));
	const int y0 = static_cast<int>(std::floor(mn.y * V)), y1 = static_cast<int>(std::floor(mx.y * V - eps));
	const int z0 = static_cast<int>(std::floor(mn.z * V)), z1 = static_cast<int>(std::floor(mx.z * V - eps));
	for (int z = z0; z <= z1; ++z)
		for (int y = y0; y <= y1; ++y)
			for (int x = x0; x <= x1; ++x)
				if (IsSolid(x, y, z)) return true;
	return false;
}

VoxelWorld::RayHit VoxelWorld::Raycast(const XMFLOAT3& origin, const XMFLOAT3& dir, float maxDistance) const
{
	RayHit result;
	// Work in voxel units; t stays proportional to metres (t_m = t_v * S).
	const float o[3] = { origin.x * V, origin.y * V, origin.z * V };
	const float d[3] = { dir.x, dir.y, dir.z };
	int cell[3] = { static_cast<int>(std::floor(o[0])), static_cast<int>(std::floor(o[1])), static_cast<int>(std::floor(o[2])) };
	int step[3];
	float tMax[3], tDelta[3];
	for (int a = 0; a < 3; ++a)
	{
		if (std::fabs(d[a]) < 1e-9f)
		{
			step[a] = 0;
			tMax[a] = tDelta[a] = 1e30f;
			continue;
		}
		step[a] = d[a] > 0 ? 1 : -1;
		tDelta[a] = std::fabs(1.0f / d[a]);
		const float boundary = d[a] > 0 ? cell[a] + 1.0f : static_cast<float>(cell[a]);
		tMax[a] = (boundary - o[a]) / d[a];
	}

	const float maxT = maxDistance * V;
	float t = 0.0f;
	int lastAxis = -1;
	while (t <= maxT)
	{
		if (IsSolid(cell[0], cell[1], cell[2]))
		{
			result.hit = true;
			result.inGrid = InGrid(cell[0], cell[1], cell[2]);
			result.x = cell[0];
			result.y = cell[1];
			result.z = cell[2];
			result.distance = t * S;
			result.point = { origin.x + dir.x * result.distance, origin.y + dir.y * result.distance, origin.z + dir.z * result.distance };
			if (lastAxis >= 0) result.normal[lastAxis] = -step[lastAxis];
			return result;
		}
		lastAxis = (tMax[0] < tMax[1]) ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		t = tMax[lastAxis];
		tMax[lastAxis] += tDelta[lastAxis];
		cell[lastAxis] += step[lastAxis];
	}
	return result;
}

float VoxelWorld::EffectiveHardness(Block block, int voxelZ) const
{
	const float metresDeep = static_cast<float>(voxelZ) * S;
	return cfg::Info(block).hardness * (1.0f + metresDeep / cfg::kHardnessDepthScale);
}

VoxelWorld::CarveResult VoxelWorld::Carve(const XMFLOAT3& center, float pickSpeed, int mbx, int mby, int mbz, uint32_t salt)
{
	CarveResult result;
	const float pickFactor = pickSpeed / cfg::kPickSpeed[0];
	const float maxRadius = cfg::kBiteRadius * cfg::kBiteMaxScale * (1.0f + cfg::kBiteRoughness);

	const int x0 = std::max(0, static_cast<int>(std::floor((center.x - maxRadius) * V)));
	const int y0 = std::max(0, static_cast<int>(std::floor((center.y - maxRadius) * V)));
	const int z0 = std::max(0, static_cast<int>(std::floor((center.z - maxRadius) * V)));
	const int x1 = std::min(cfg::kVoxX - 1, static_cast<int>(std::floor((center.x + maxRadius) * V)));
	const int y1 = std::min(cfg::kVoxY - 1, static_cast<int>(std::floor((center.y + maxRadius) * V)));
	const int z1 = std::min(cfg::kVoxZ - 1, static_cast<int>(std::floor((center.z + maxRadius) * V)));

	auto grow = [](Box& box, int x, int y, int z)
	{
		if (box.Empty())
		{
			box.min[0] = box.max[0] = x; box.min[1] = box.max[1] = y; box.min[2] = box.max[2] = z;
			return;
		}
		box.min[0] = std::min(box.min[0], x); box.max[0] = std::max(box.max[0], x);
		box.min[1] = std::min(box.min[1], y); box.max[1] = std::max(box.max[1], y);
		box.min[2] = std::min(box.min[2], z); box.max[2] = std::max(box.max[2], z);
	};
	auto remove = [&](int x, int y, int z, Block block)
	{
		mVoxels[Index(x, y, z)] = Block::Air;
		mDamage.erase(static_cast<uint32_t>(Index(x, y, z)));
		++result.removed;
		result.voxels.push_back({ x, y, z, block });
		++result.removedByType[static_cast<int>(block)];
		if (result.dirty.Empty())
		{
			result.dirty.min[0] = result.dirty.max[0] = x;
			result.dirty.min[1] = result.dirty.max[1] = y;
			result.dirty.min[2] = result.dirty.max[2] = z;
		}
		else
		{
			result.dirty.min[0] = std::min(result.dirty.min[0], x); result.dirty.max[0] = std::max(result.dirty.max[0], x);
			result.dirty.min[1] = std::min(result.dirty.min[1], y); result.dirty.max[1] = std::max(result.dirty.max[1], y);
			result.dirty.min[2] = std::min(result.dirty.min[2], z); result.dirty.max[2] = std::max(result.dirty.max[2], z);
		}
	};

	for (int z = z0; z <= z1; ++z)
	for (int y = y0; y <= y1; ++y)
	for (int x = x0; x <= x1; ++x)
	{
		const Block block = mVoxels[Index(x, y, z)];
		if (block == Block::Air || block == Block::Bedrock) continue;

		const float dx = (x + 0.5f) * S - center.x;
		const float dy = (y + 0.5f) * S - center.y;
		const float dz = (z + 0.5f) * S - center.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

		const float rough = 1.0f + cfg::kBiteRoughness * (color::Hash01(x, y, z, salt) * 2.0f - 1.0f);
		const float radius = cfg::kBiteRadius * std::min(std::sqrt(pickFactor), cfg::kBiteMaxScale) * rough;
		float falloff = std::clamp((1.0f - distance / radius) * cfg::kBiteFalloff, 0.0f, 1.0f);
		// The voxel you aimed at always takes a full hit, so even very hard rock yields to persistence.
		if (x == mbx && y == mby && z == mbz) falloff = 1.0f;
		if (falloff <= 0.0f) continue;

		const uint32_t key = static_cast<uint32_t>(Index(x, y, z));
		const float damage = (mDamage.count(key) ? mDamage[key] : 0.0f) + cfg::kPickPower * pickFactor / std::max(EffectiveHardness(block, z), 0.01f) * falloff;
		if (damage >= 0.999f)
		{
			remove(x, y, z, block);
		}
		else
		{
			mDamage[key] = damage;
			grow(result.cracked, x, y, z);
		}
	}
	return result;
}

// ---- Physics ------------------------------------------------------------------

uint64_t VoxelWorld::Hash() const
{
	uint64_t h = 1469598103934665603ull;
	for (Block b : mVoxels)
	{
		h ^= static_cast<uint64_t>(b);
		h *= 1099511628211ull;
	}
	return h;
}

bool VoxelWorld::TouchesAir(const int lo[3], const int hi[3]) const
{
	for (int z = lo[2] - 1; z <= hi[2]; ++z)
		for (int y = lo[1] - 1; y <= hi[1]; ++y)
			for (int x = lo[0] - 1; x <= hi[0]; ++x)
				if (!IsSolid(x, y, z)) return true;
	return false;
}

void VoxelWorld::BuildCollisionTriangles(int zBegin, int zEnd, std::vector<XMFLOAT3>& out) const
{
	const int lo[3] = { 0, 0, std::max(zBegin, 0) };
	const int hi[3] = { cfg::kVoxX, cfg::kVoxY, std::min(zEnd, cfg::kVoxZ) };
	BuildCollisionTriangles(lo, hi, out);
}

void VoxelWorld::BuildCollisionTriangles(const int loIn[3], const int hiIn[3], std::vector<XMFLOAT3>& out) const
{
	const int lo[3] = { std::max(loIn[0], 0), std::max(loIn[1], 0), std::max(loIn[2], 0) };
	const int hi[3] = { std::min(hiIn[0], cfg::kVoxX), std::min(hiIn[1], cfg::kVoxY), std::min(hiIn[2], cfg::kVoxZ) };
	if (lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]) return;
	std::vector<uint8_t> mask;

	for (int axis = 0; axis < 3; ++axis)
	{
		const int u = (axis + 1) % 3;
		const int v = (axis + 2) % 3;
		const int du = hi[u] - lo[u];
		const int dv = hi[v] - lo[v];
		mask.assign(static_cast<size_t>(du) * dv, 0);

		for (int sign = -1; sign <= 1; sign += 2)
		{
			for (int slice = lo[axis]; slice < hi[axis]; ++slice)
			{
				// Mask of voxels in this slice whose face towards `sign` is exposed.
				bool any = false;
				for (int j = 0; j < dv; ++j)
				{
					for (int i = 0; i < du; ++i)
					{
						int c[3];
						c[axis] = slice; c[u] = lo[u] + i; c[v] = lo[v] + j;
						int n[3] = { c[0], c[1], c[2] };
						n[axis] += sign;
						const bool exposed = mVoxels[Index(c[0], c[1], c[2])] != Block::Air && !IsSolid(n[0], n[1], n[2]);
						mask[static_cast<size_t>(j) * du + i] = exposed ? 1 : 0;
						any |= exposed;
					}
				}
				if (!any) continue;

				// Greedy: grow each rectangle along u, then along v.
				const float plane = (slice + (sign > 0 ? 1 : 0)) * S;
				for (int j = 0; j < dv; ++j)
				{
					for (int i = 0; i < du; )
					{
						if (!mask[static_cast<size_t>(j) * du + i]) { ++i; continue; }
						int w = 1;
						while (i + w < du && mask[static_cast<size_t>(j) * du + i + w]) ++w;
						int h = 1;
						for (bool grow = true; grow && j + h < dv; )
						{
							for (int k = 0; k < w; ++k)
							{
								if (!mask[static_cast<size_t>(j + h) * du + i + k]) { grow = false; break; }
							}
							if (grow) ++h;
						}
						for (int jj = 0; jj < h; ++jj)
							for (int ii = 0; ii < w; ++ii)
								mask[static_cast<size_t>(j + jj) * du + i + ii] = 0;

						auto corner = [&](int cu, int cv)
						{
							float p[3];
							p[axis] = plane;
							p[u] = (lo[u] + cu) * S;
							p[v] = (lo[v] + cv) * S;
							return XMFLOAT3(p[0], p[1], p[2]);
						};
						XMFLOAT3 q[4] = { corner(i, j), corner(i + w, j), corner(i + w, j + h), corner(i, j + h) };
						// (u x v) points along +axis; flip the winding for faces looking down -axis.
						if (sign < 0) std::swap(q[1], q[3]);
						out.insert(out.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
						i += w;
					}
				}
			}
		}
	}
}

// ---- Ray-marched renderer ---------------------------------------------------

std::vector<XMFLOAT4> VoxelWorld::BuildPalette()
{
	std::vector<XMFLOAT4> palette(256, XMFLOAT4(1, 0, 1, 1));
	palette[0] = { 0, 0, 0, 0 };
	for (int band = 0; band < cfg::kStoneBands; ++band)
	{
		const XMFLOAT3 stone = StoneSrgb((band + 0.5f) / cfg::kStoneBands);
		palette[kStoneFirst + band] = color::WithAlpha(stone);
		palette[kBedrockFirst + band] = color::WithAlpha(color::Scale(stone, cfg::kBedrockDarken));
	}
	for (int type = 0; type < static_cast<int>(Block::Count); ++type)
	{
		palette[kOreFirst + type] = color::WithAlpha(color::Srgb(cfg::kBlocks[type].color));
	}
	for (int level = 0; level < 2; ++level)
	{
		const int first = kCrackFirst + level * kCrackSet;
		for (int band = 0; band < cfg::kStoneBands; ++band)
			palette[first + band] = color::WithAlpha(color::Scale(StoneSrgb((band + 0.5f) / cfg::kStoneBands), cfg::kCrackShade[level]));
		for (int type = 0; type < static_cast<int>(Block::Count); ++type)
			palette[first + cfg::kStoneBands + type] = color::WithAlpha(color::Scale(color::Srgb(cfg::kBlocks[type].color), cfg::kCrackShade[level]));
	}
	return palette;
}

uint8_t VoxelWorld::PaletteIndex(Block block, int voxelZ, int crack)
{
	if (crack > 0 && block != Block::Air && block != Block::Bedrock)
	{
		const int first = kCrackFirst + (std::min(crack, 2) - 1) * kCrackSet;
		return static_cast<uint8_t>(block == Block::Stone ? first + DepthBand(voxelZ) : first + cfg::kStoneBands + static_cast<int>(block));
	}
	switch (block)
	{
	case Block::Air:     return 0;
	case Block::Stone:   return static_cast<uint8_t>(kStoneFirst + DepthBand(voxelZ));
	case Block::Bedrock: return static_cast<uint8_t>(kBedrockFirst + DepthBand(voxelZ));
	default:             return static_cast<uint8_t>(kOreFirst + static_cast<int>(block));
	}
}

std::vector<uint8_t> VoxelWorld::PaletteVoxels() const
{
	std::vector<uint8_t> out(mVoxels.size());
	for (int z = 0; z < cfg::kVoxZ; ++z)
		for (int y = 0; y < cfg::kVoxY; ++y)
			for (int x = 0; x < cfg::kVoxX; ++x)
				out[Index(x, y, z)] = PaletteIndex(mVoxels[Index(x, y, z)], z);
	return out;
}

std::vector<uint8_t> VoxelWorld::PaletteVoxels(const Box& box) const
{
	std::vector<uint8_t> out;
	if (box.Empty()) return out;
	out.reserve(static_cast<size_t>(box.max[0] - box.min[0] + 1) * (box.max[1] - box.min[1] + 1) * (box.max[2] - box.min[2] + 1));
	for (int z = box.min[2]; z <= box.max[2]; ++z)
		for (int y = box.min[1]; y <= box.max[1]; ++y)
			for (int x = box.min[0]; x <= box.max[0]; ++x)
				out.push_back(PaletteIndex(Get(x, y, z), z, CrackLevel(x, y, z)));
	return out;
}

// ---- Mesh renderer ------------------------------------------------------------

XMFLOAT4 VoxelWorld::VoxelColor(Block block, int x, int y, int z)
{
	const float depth = static_cast<float>(z) / (cfg::kVoxZ - 1);
	const float jitter = color::Hash01(x, y, z) * 2.0f - 1.0f;
	XMFLOAT3 srgb;
	float brightness = 1.0f;
	switch (block)
	{
	case Block::Stone:
		srgb = StoneSrgb(depth);
		brightness = 1.0f + jitter * cfg::kStoneJitter;
		break;
	case Block::Bedrock:
		srgb = StoneSrgb(depth);
		brightness = cfg::kBedrockDarken * (1.0f + jitter * cfg::kStoneJitter);
		break;
	default:
		srgb = color::Srgb(cfg::Info(block).color);
		brightness = 1.0f + jitter * cfg::kOreJitter;
		break;
	}
	return color::WithAlpha(color::Scale(color::ToLinear(srgb), brightness));
}

MeshData VoxelWorld::BuildChunkMesh(int chunk) const
{
	struct FaceDir { int dx, dy, dz; XMFLOAT3 normal, u, v; };
	// u x v points along the normal; corners go (-u-v), (-u+v), (+u+v), (+u-v).
	static const FaceDir kDirs[6] =
	{
		{  1, 0, 0, { 1, 0, 0}, { 0, 0, 1}, {0, 1, 0} },
		{ -1, 0, 0, {-1, 0, 0}, { 0, 0,-1}, {0, 1, 0} },
		{ 0,  1, 0, { 0, 1, 0}, { 1, 0, 0}, {0, 0, 1} },
		{ 0, -1, 0, { 0,-1, 0}, { 1, 0, 0}, {0, 0,-1} },
		{ 0, 0,  1, { 0, 0, 1}, {-1, 0, 0}, {0, 1, 0} },
		{ 0, 0, -1, { 0, 0,-1}, { 1, 0, 0}, {0, 1, 0} },
	};

	MeshData mesh;
	const int chunkVoxels = cfg::kChunkDepth * V;
	const int z0 = chunk * chunkVoxels;
	const int z1 = std::min(z0 + chunkVoxels, cfg::kVoxZ);
	const float half = S * 0.5f;
	for (int z = z0; z < z1; ++z)
	for (int y = 0; y < cfg::kVoxY; ++y)
	for (int x = 0; x < cfg::kVoxX; ++x)
	{
		const Block block = mVoxels[Index(x, y, z)];
		if (block == Block::Air) continue;

		XMFLOAT4 colour{};
		bool colourReady = false;
		const XMVECTOR center = XMVectorSet((x + 0.5f) * S, (y + 0.5f) * S, (z + 0.5f) * S, 0.0f);
		for (const FaceDir& f : kDirs)
		{
			if (IsSolid(x + f.dx, y + f.dy, z + f.dz)) continue;   // hidden face
			if (!colourReady)
			{
				colour = VoxelColor(block, x, y, z);
				colourReady = true;
			}

			const XMVECTOR n = XMLoadFloat3(&f.normal) * half;
			const XMVECTOR u = XMLoadFloat3(&f.u) * half;
			const XMVECTOR v = XMLoadFloat3(&f.v) * half;
			const XMVECTOR corners[4] = { center + n - u - v, center + n - u + v, center + n + u + v, center + n + u - v };
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			for (int i = 0; i < 4; ++i)
			{
				Vertex vertex;
				XMStoreFloat3(&vertex.position, corners[i]);
				vertex.normal = f.normal;
				vertex.color = colour;
				mesh.vertices.push_back(vertex);
			}
			mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}
	}
	return mesh;
}
