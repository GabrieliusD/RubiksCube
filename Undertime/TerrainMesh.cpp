#include "TerrainMesh.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <functional>

using namespace DirectX;

namespace
{
	constexpr float kNone = -1e9f;

	void AddQuad(MeshData& mesh, const XMFLOAT3& o, const XMFLOAT3& e1, const XMFLOAT3& e2, const XMFLOAT3& normal,
		const XMFLOAT3& colour, float block)
	{
		// e1 x e2 points along `normal` (the engine's front-face convention).
		const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
		const XMFLOAT3 p[4] = { o, { o.x + e1.x, o.y + e1.y, o.z + e1.z }, { o.x + e1.x + e2.x, o.y + e1.y + e2.y, o.z + e1.z + e2.z }, { o.x + e2.x, o.y + e2.y, o.z + e2.z } };
		for (const XMFLOAT3& q : p)
		{
			Vertex v;
			v.position = q;
			v.normal = normal;
			v.color = { colour.x, colour.y, colour.z, 1.0f };
			v.uv = { 0.0f, block };
			mesh.vertices.push_back(v);
		}
		mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
	}

	// A vertical face along one side of a column, from y0 to y1. d: 0 -x, 1 +x, 2 -z, 3 +z;
	// `plane` is the face's x (d 0, 1) or z (d 2, 3); a0..a1 runs along the other axis.
	void AddSide(MeshData& mesh, int d, float plane, float a0, float a1, float y0, float y1, const XMFLOAT3& colour, float block)
	{
		switch (d)
		{
		case 0: AddQuad(mesh, { plane, y0, a0 }, { 0, 0, a1 - a0 }, { 0, y1 - y0, 0 }, { -1, 0, 0 }, colour, block); break;
		case 1: AddQuad(mesh, { plane, y0, a0 }, { 0, y1 - y0, 0 }, { 0, 0, a1 - a0 }, { 1, 0, 0 }, colour, block); break;
		case 2: AddQuad(mesh, { a0, y0, plane }, { 0, y1 - y0, 0 }, { a1 - a0, 0, 0 }, { 0, 0, -1 }, colour, block); break;
		default: AddQuad(mesh, { a0, y0, plane }, { a1 - a0, 0, 0 }, { 0, y1 - y0, 0 }, { 0, 0, 1 }, colour, block); break;
		}
	}

	// Greedy rectangles over an n x n grid of keys (0 = nothing): emit(i, j, w, h, key).
	void Greedy2D(std::vector<uint64_t>& keys, int n, const std::function<void(int, int, int, int, uint64_t)>& emit)
	{
		for (int j = 0; j < n; ++j)
		{
			for (int i = 0; i < n; )
			{
				const uint64_t key = keys[i + n * j];
				if (!key) { ++i; continue; }
				int w = 1;
				while (i + w < n && keys[i + w + n * j] == key) ++w;
				int h = 1;
				for (bool grow = true; grow && j + h < n; )
				{
					for (int k = 0; k < w && grow; ++k) grow = keys[i + k + n * (j + h)] == key;
					if (grow) ++h;
				}
				for (int jj = 0; jj < h; ++jj)
					for (int ii = 0; ii < w; ++ii) keys[i + ii + n * (j + jj)] = 0;
				emit(i, j, w, h, key);
				i += w;
			}
		}
	}

	// Neighbour offsets for the four sides: -x, +x, -z, +z.
	const int kDi[4] = { -1, 1, 0, 0 };
	const int kDj[4] = { 0, 0, -1, 1 };
}

TerrainPatch SampleTerrain(const Valley& valley, float x0, float z0, float block, int n)
{
	TerrainPatch patch;
	patch.x0 = x0;
	patch.z0 = z0;
	patch.block = block;
	patch.n = n;
	const int size = (n + 2) * (n + 2);
	patch.top.resize(size);
	patch.bottom.resize(size, kNone);
	patch.water.resize(size, Valley::kNoWater);
	patch.ground.resize(size, Ground::Grass);
	for (int j = -1; j <= n; ++j)
	{
		for (int i = -1; i <= n; ++i)
		{
			const float x = x0 + (i + 0.5f) * block, z = z0 + (j + 0.5f) * block;
			const int k = patch.Index(i, j);
			float height, water;
			valley.Sample(x, z, height, water);
			const float top = std::round(height / block) * block;
			patch.top[k] = top;
			if (Valley::OverCave(x, z)) patch.bottom[k] = static_cast<float>(cfg::kGridY);
			if (water != Valley::kNoWater)
			{
				const float level = std::round(water / block) * block;
				if (level > top) patch.water[k] = level;
			}
		}
	}
	for (int j = 0; j < n; ++j)
	{
		for (int i = 0; i < n; ++i)
		{
			const int k = patch.Index(i, j);
			float drop = 0.0f;
			for (int d = 0; d < 4; ++d) drop = std::max(drop, patch.top[k] - patch.Top(i + kDi[d], j + kDj[d]));
			patch.ground[k] = valley.GroundAt(patch.x0 + (i + 0.5f) * block, patch.z0 + (j + 0.5f) * block, patch.top[k], drop, block);
		}
	}
	return patch;
}

MeshData BuildTerrainMesh(const TerrainPatch& p, const std::array<XMFLOAT3, static_cast<int>(Ground::Count)>& colours, const std::array<float, 4>& skirt, bool shapeOnly)
{
	MeshData mesh;
	const int n = p.n;
	const float b = p.block;
	auto colour = [&](Ground g) { return colours[static_cast<int>(g)]; };

	// Tops: merge columns of the same height and ground.
	std::vector<uint64_t> keys(static_cast<size_t>(n) * n);
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			const int k = p.Index(i, j);
			const int64_t level = std::llround(p.top[k] / b) + (1ll << 30);
			// An empty column (no ground over the cave grid there: a shaft's opening) has no top.
			keys[i + n * j] = p.top[k] <= p.bottom[k] ? 0 : (static_cast<uint64_t>(level) << 8) | (shapeOnly ? 1u : static_cast<uint64_t>(p.ground[k]) + 1);
		}
	Greedy2D(keys, n, [&](int i, int j, int w, int h, uint64_t key)
	{
		const int k = p.Index(i, j);
		const float x = p.x0 + i * b, z = p.z0 + j * b;
		AddQuad(mesh, { x, p.top[k], z }, { 0, 0, h * b }, { w * b, 0, 0 }, { 0, 1, 0 }, colour(p.ground[k]), b);
	});

	// Rock walls show the mountain's strata (Valley.cpp): bands kStrataHeight tall in
	// alternating shades, like the layers of the cave's face.
	auto addRockSide = [&](int d, float plane, float a0, float a1, float y0, float y1)
	{
		const float band = cfg::kStrataHeight;
		static const float kShade[3] = { 1.0f, 0.9f, 0.8f };
		for (float y = y0; y < y1 - 1e-4f; )
		{
			const float layer = std::floor(y / band + 1e-4f);
			const float top = std::min(y1, (layer + 1.0f) * band);
			const int k = static_cast<int>(layer) % 3;
			const XMFLOAT3 base = colour(k == 1 ? Ground::Rock2 : Ground::Rock);
			const float shade = kShade[(k + 3) % 3];
			AddSide(mesh, d, plane, a0, a1, y, top, { base.x * shade, base.y * shade, base.z * shade }, b);
			y = top;
		}
	};
	auto isRock = [](Ground g) { return g == Ground::Rock || g == Ground::Rock2; };

	// Sides: where the neighbour is lower, in runs along the edge with the same top, bottom and ground.
	struct Run { float top, bottom; Ground ground; };
	for (int d = 0; d < 4; ++d)
	{
		const bool alongZ = d < 2;   // faces of x-sides run along z
		for (int line = 0; line < n; ++line)
		{
			auto runAt = [&](int a, Run& run)
			{
				const int i = alongZ ? line : a, j = alongZ ? a : line;
				const int k = p.Index(i, j);
				float bottom = std::max(p.Top(i + kDi[d], j + kDj[d]), p.bottom[k]);
				const bool edge = (d == 0 && i == 0) || (d == 1 && i == n - 1) || (d == 2 && j == 0) || (d == 3 && j == n - 1);
				if (edge && skirt[d] > 0.0f) bottom = std::min(bottom, p.top[k]) - skirt[d];
				run = { p.top[k], bottom, shapeOnly ? Ground::Rock : p.ground[k] };
				return p.top[k] > bottom;
			};
			for (int a = 0; a < n; )
			{
				Run run;
				if (!runAt(a, run)) { ++a; continue; }
				int len = 1;
				Run next;
				while (a + len < n && runAt(a + len, next) && next.top == run.top && next.bottom == run.bottom && next.ground == run.ground) ++len;

				const float plane = alongZ ? p.x0 + (line + (d == 1 ? 1 : 0)) * b : p.z0 + (line + (d == 3 ? 1 : 0)) * b;
				const float a0 = (alongZ ? p.z0 : p.x0) + a * b, a1 = a0 + len * b;
				// Grass and snow show one block of their colour on top of dirt or rock.
				// A grass (or snow) top shows a block-high band of it down the side; only on fine
				// ground: on coarse far terrain the bands are thinner than a pixel and glitter.
				const bool banded = b < 0.5f && (run.ground == Ground::Grass || run.ground == Ground::Grass2 || run.ground == Ground::Snow);
				if (banded && run.top - run.bottom > b * 1.01f)
				{
					// The grass (or snow) block, a block of soil under grass, then rock.
					AddSide(mesh, d, plane, a0, a1, run.top - b, run.top, colour(run.ground), b);
					float y = run.top - b;
					if (run.ground != Ground::Snow && y - run.bottom > 1e-4f)
					{
						const float soil = std::max(run.bottom, y - b);
						AddSide(mesh, d, plane, a0, a1, soil, y, colour(Ground::Dirt), b);
						y = soil;
					}
					if (y - run.bottom > 1e-4f)
					{
						if (y - run.bottom > 1.5f * b) addRockSide(d, plane, a0, a1, run.bottom, y);
						else AddSide(mesh, d, plane, a0, a1, run.bottom, y, colour(Ground::Dirt), b);
					}
				}
				else if (isRock(run.ground) && run.top - run.bottom > cfg::kStrataHeight * 0.5f && !shapeOnly)
				{
					addRockSide(d, plane, a0, a1, run.bottom, run.top);
				}
				else
				{
					AddSide(mesh, d, plane, a0, a1, run.bottom, run.top, colour(run.ground), b);
				}
				a += len;
			}
		}
	}
	return mesh;
}

MeshData BuildWaterMesh(const TerrainPatch& p, const XMFLOAT3& colour)
{
	MeshData mesh;
	const int n = p.n;
	const float b = p.block;
	std::vector<uint64_t> keys(static_cast<size_t>(n) * n, 0);
	bool any = false;
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			const float w = p.water[p.Index(i, j)];
			if (w == Valley::kNoWater) continue;
			keys[i + n * j] = static_cast<uint64_t>(std::llround(w / b) + (1ll << 30));
			any = true;
		}
	if (!any) return mesh;
	Greedy2D(keys, n, [&](int i, int j, int w, int h, uint64_t)
	{
		AddQuad(mesh, { p.x0 + i * b, p.water[p.Index(i, j)], p.z0 + j * b }, { 0, 0, h * b }, { w * b, 0, 0 }, { 0, 1, 0 }, colour, 0.0f);
	});
	// Falls: where the water steps down to a neighbour's lower surface.
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			const float w = p.water[p.Index(i, j)];
			if (w == Valley::kNoWater) continue;
			for (int d = 0; d < 4; ++d)
			{
				const int k = p.Index(i + kDi[d], j + kDj[d]);
				const float below = std::max(p.water[k], p.top[k]);
				if (below >= w) continue;
				const bool alongZ = d < 2;
				const float plane = alongZ ? p.x0 + (i + (d == 1 ? 1 : 0)) * b : p.z0 + (j + (d == 3 ? 1 : 0)) * b;
				const float a0 = alongZ ? p.z0 + j * b : p.x0 + i * b;
				AddSide(mesh, d, plane, a0, a0 + b, below, w, colour, 0.0f);
			}
		}
	return mesh;
}

void AppendMesh(MeshData& mesh, const MeshData& part)
{
	const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
	mesh.vertices.insert(mesh.vertices.end(), part.vertices.begin(), part.vertices.end());
	mesh.indices.reserve(mesh.indices.size() + part.indices.size());
	for (uint32_t index : part.indices) mesh.indices.push_back(base + index);
}

std::vector<XMFLOAT3> MeshTriangles(const MeshData& mesh)
{
	std::vector<XMFLOAT3> out;
	out.reserve(mesh.indices.size());
	for (uint32_t index : mesh.indices) out.push_back(mesh.vertices[index].position);
	return out;
}
