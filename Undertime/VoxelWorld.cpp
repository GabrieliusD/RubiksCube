#include "VoxelWorld.h"
#include "ColorUtil.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
using cfg::Block;

void VoxelWorld::Generate(uint32_t /*seed*/)
{
	mCells.assign(static_cast<size_t>(cfg::kGridX) * cfg::kGridY * cfg::kGridZ, Block::Stone);

	for (int z = 0; z < cfg::kGridZ; ++z)
	for (int y = 0; y < cfg::kGridY; ++y)
	for (int x = 0; x < cfg::kGridX; ++x)
	{
		const bool bedrock = y == 0 || y == cfg::kGridY - 1 || x == 0 || x == cfg::kGridX - 1 || z == cfg::kGridZ - 1;
		if (bedrock) mCells[Index(x, y, z)] = Block::Bedrock;
	}

	// Entrance passage: 1 wide x 2 tall in the centre column.
	for (int z = cfg::kEntranceZ0; z <= cfg::kEntranceZ1; ++z)
		for (int y = 1; y <= cfg::kEntranceHeight; ++y)
			Set(cfg::kCenterX, y, z, Block::Air);

	// First chamber: 5 wide x 3 tall.
	for (int z = cfg::kChamberZ0; z <= cfg::kChamberZ1; ++z)
		for (int y = 1; y <= cfg::kChamberHeight; ++y)
			for (int x = cfg::kCenterX - cfg::kChamberHalfWidth; x <= cfg::kCenterX + cfg::kChamberHalfWidth; ++x)
				Set(x, y, z, Block::Air);

	// Ore veins and relics are placed with the seeded RNG in milestone 2.
}

bool VoxelWorld::InGrid(int x, int y, int z)
{
	return x >= 0 && x < cfg::kGridX && y >= 0 && y < cfg::kGridY && z >= 0 && z < cfg::kGridZ;
}

VoxelWorld::Block VoxelWorld::Get(int x, int y, int z) const
{
	return InGrid(x, y, z) ? mCells[Index(x, y, z)] : Block::Air;
}

void VoxelWorld::Set(int x, int y, int z, Block block)
{
	if (InGrid(x, y, z)) mCells[Index(x, y, z)] = block;
}

bool VoxelWorld::IsSolid(int x, int y, int z) const
{
	if (z >= 0)
	{
		// Inside the mountain: the grid, surrounded by solid rock.
		if (InGrid(x, y, z)) return mCells[Index(x, y, z)] != Block::Air;
		return true;   // solid mountain rock all around the grid
	}
	// Outside: ground below y = 1, invisible walls beyond the play area.
	if (y < static_cast<int>(cfg::kGroundY)) return true;
	const float cx = x + 0.5f - (cfg::kCenterX + 0.5f);
	const float cz = z + 0.5f;
	return std::fabs(cx) > cfg::kOutsideExtent || cz < -cfg::kOutsideExtent;
}

bool VoxelWorld::BoxOverlapsSolid(const XMFLOAT3& mn, const XMFLOAT3& mx) const
{
	const float eps = 1e-4f;
	const int x0 = static_cast<int>(std::floor(mn.x)), x1 = static_cast<int>(std::floor(mx.x - eps));
	const int y0 = static_cast<int>(std::floor(mn.y)), y1 = static_cast<int>(std::floor(mx.y - eps));
	const int z0 = static_cast<int>(std::floor(mn.z)), z1 = static_cast<int>(std::floor(mx.z - eps));
	for (int z = z0; z <= z1; ++z)
		for (int y = y0; y <= y1; ++y)
			for (int x = x0; x <= x1; ++x)
				if (IsSolid(x, y, z)) return true;
	return false;
}

VoxelWorld::RayHit VoxelWorld::Raycast(const XMFLOAT3& origin, const XMFLOAT3& dir, float maxDistance) const
{
	RayHit result;
	const float o[3] = { origin.x, origin.y, origin.z };
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

	float t = 0.0f;
	int lastAxis = -1;
	while (t <= maxDistance)
	{
		if (IsSolid(cell[0], cell[1], cell[2]))
		{
			result.hit = true;
			result.inGrid = InGrid(cell[0], cell[1], cell[2]);
			result.x = cell[0];
			result.y = cell[1];
			result.z = cell[2];
			result.distance = t;
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

float VoxelWorld::EffectiveHardness(int x, int y, int z) const
{
	const float hardness = cfg::Info(Get(x, y, z)).hardness;
	return hardness * (1.0f + static_cast<float>(z) / cfg::kHardnessDepthScale);
}

XMFLOAT4 VoxelWorld::CellColor(Block block, int x, int y, int z) const
{
	const float depth = std::clamp(static_cast<float>(z) / (cfg::kGridZ - 1), 0.0f, 1.0f);
	const float jitter = color::Hash01(x, y, z) * 2.0f - 1.0f;
	XMFLOAT3 srgb;
	float brightness = 1.0f;
	switch (block)
	{
	case Block::Stone:
		srgb = color::Lerp(color::Srgb(cfg::kStoneNear), color::Srgb(cfg::kStoneDeep), depth);
		brightness = 1.0f + jitter * cfg::kStoneJitter;
		break;
	case Block::Bedrock:
		srgb = color::Lerp(color::Srgb(cfg::kStoneNear), color::Srgb(cfg::kStoneDeep), depth);
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
	const int z0 = chunk * cfg::kChunkDepth;
	const int z1 = std::min(z0 + cfg::kChunkDepth, cfg::kGridZ);
	for (int z = z0; z < z1; ++z)
	for (int y = 0; y < cfg::kGridY; ++y)
	for (int x = 0; x < cfg::kGridX; ++x)
	{
		const Block block = mCells[Index(x, y, z)];
		if (block == Block::Air) continue;

		const XMFLOAT4 colour = CellColor(block, x, y, z);
		const XMVECTOR center = XMVectorSet(x + 0.5f, y + 0.5f, z + 0.5f, 0.0f);
		for (const FaceDir& f : kDirs)
		{
			if (IsSolid(x + f.dx, y + f.dy, z + f.dz)) continue;   // hidden face

			const XMVECTOR n = XMLoadFloat3(&f.normal) * 0.5f;
			const XMVECTOR u = XMLoadFloat3(&f.u) * 0.5f;
			const XMVECTOR v = XMLoadFloat3(&f.v) * 0.5f;
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
