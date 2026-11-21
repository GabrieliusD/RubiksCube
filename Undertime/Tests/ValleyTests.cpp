// Tests for the valley (heights, river, lake, trees) and its blocky terrain meshes.
#include "../Valley.h"
#include "../TerrainMesh.h"
#include "../Config.h"
#include <cmath>
#include <cstdio>
#include <set>

using namespace DirectX;

namespace
{
	int gValleyFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gValleyFailures;
	}

	bool WindingMatchesNormals(const MeshData& mesh)
	{
		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			const Vertex& a = mesh.vertices[mesh.indices[i]];
			const XMVECTOR pa = XMLoadFloat3(&a.position);
			const XMVECTOR pb = XMLoadFloat3(&mesh.vertices[mesh.indices[i + 1]].position);
			const XMVECTOR pc = XMLoadFloat3(&mesh.vertices[mesh.indices[i + 2]].position);
			if (!(XMVectorGetX(XMVector3Dot(XMVector3Cross(pb - pa, pc - pa), XMLoadFloat3(&a.normal))) > 0.0f)) return false;
		}
		return true;
	}

	std::array<XMFLOAT3, static_cast<int>(Ground::Count)> Colours()
	{
		std::array<XMFLOAT3, static_cast<int>(Ground::Count)> c{};
		for (int i = 0; i < static_cast<int>(Ground::Count); ++i) c[i] = { 0.1f * (i + 1), 0.5f, 0.5f };
		return c;
	}

	// Area of the faces pointing along `normal` (quads: 4 vertices each, in order).
	float FaceArea(const MeshData& mesh, const XMFLOAT3& normal)
	{
		float area = 0.0f;
		for (size_t v = 0; v + 3 < mesh.vertices.size(); v += 4)
		{
			const XMFLOAT3& n = mesh.vertices[v].normal;
			if (n.x != normal.x || n.y != normal.y || n.z != normal.z) continue;
			const XMVECTOR a = XMLoadFloat3(&mesh.vertices[v].position), b = XMLoadFloat3(&mesh.vertices[v + 1].position), c = XMLoadFloat3(&mesh.vertices[v + 3].position);
			area += XMVectorGetX(XMVector3Length(XMVector3Cross(b - a, c - a)));
		}
		return area;
	}
}

int ValleyTests()
{
	const Valley valley;
	const float mouthX = cfg::kCenterX + 0.5f;

	// ---- Heights ---------------------------------------------------------------------------
	Check(std::fabs(valley.Height(mouthX, -6.0f) - cfg::kGroundY) < 1e-3f && std::fabs(valley.Height(mouthX + 15.0f, -15.0f) - cfg::kGroundY) < 1e-3f,
		"valley: the ground around the cave mouth is flat at kGroundY");
	bool overCave = true;
	for (float z = 0.5f; z < cfg::kGridZ; z += 7.0f)
		for (float x = 0.5f; x < cfg::kGridX; x += 5.0f) overCave &= valley.Height(x, z) >= cfg::kGridY + 1.0f;
	Check(overCave, "valley: the mountain covers the cave grid everywhere");
	Check(valley.Height(cfg::kValleyCenter[0] + cfg::kValleyRadius[0] * 1.5f, cfg::kValleyCenter[1]) > 35.0f
		&& valley.Height(cfg::kValleyCenter[0], cfg::kValleyCenter[1] - cfg::kValleyRadius[1] * 1.5f) > 35.0f,
		"valley: mountains rise beyond the valley's edge");
	float lowest = 1e9f, highest = -1e9f;
	for (float z = -250.0f; z < -40.0f; z += 9.0f)
		for (float x = -120.0f; x < 150.0f; x += 9.0f)
		{
			if (valley.WaterLevel(x, z) != Valley::kNoWater) continue;
			const float h = valley.BaseHeight(x, z);
			lowest = std::min(lowest, h);
			highest = std::max(highest, h);
		}
	Check(lowest > cfg::kGroundY - 4.0f && highest < cfg::kGroundY + 14.0f, "valley: the floor rolls gently (within a few metres of the mouth)");

	// ---- River and lake --------------------------------------------------------------------
	{
		bool downhill = true;
		std::set<float> levels;
		for (float s = 1.0f; s < 700.0f; s += 1.0f)
		{
			downhill &= valley.RiverLevelAt(s) <= valley.RiverLevelAt(s - 1.0f);
			levels.insert(valley.RiverLevelAt(s));
		}
		Check(downhill, "river: the water never flows uphill");
		Check(levels.size() >= 4, "river: it steps down in several places (falls)");
		const float x = cfg::kRiverPoints[5][0], z = cfg::kRiverPoints[5][1];
		Check(valley.WaterLevel(x, z) != Valley::kNoWater && valley.Height(x, z) < valley.WaterLevel(x, z), "river: its bed is under water");
		Check(valley.WaterLevel(x + 20.0f, z + 20.0f) == Valley::kNoWater, "river: no water away from it");
		Check(valley.Height(cfg::kLakeCenter[0], cfg::kLakeCenter[1]) < valley.WaterLevel(cfg::kLakeCenter[0], cfg::kLakeCenter[1]),
			"lake: its bed is under water");
	}

	// ---- Trees ---------------------------------------------------------------------------------
	{
		const std::vector<TreeSpot> trees = valley.Trees();
		bool clear = true, dry = true, fineFlag = true;
		int near = 0;
		for (const TreeSpot& t : trees)
		{
			for (const auto& c : cfg::kClearings) clear &= (t.x - c[0]) * (t.x - c[0]) + (t.z - c[1]) * (t.z - c[1]) >= c[2] * c[2];
			clear &= !Valley::OverCave(t.x, t.z);
			dry &= valley.WaterLevel(t.x, t.z) == Valley::kNoWater;
			const bool inFine = valley.Detailed(t.x, t.z);
			fineFlag &= inFine != t.distant;
			near += t.distant ? 0 : 1;
		}
		std::printf("        (%zu trees, %d in the detailed valley)\n", trees.size(), near);
		Check(near > 300 && trees.size() < 20000, "trees: a forest, but a bounded one");
		Check(clear, "trees: none in clearings or over the cave");
		Check(dry, "trees: none in water");
		Check(fineFlag, "trees: far ones are exactly those outside the detailed valley");
		bool offPaths = true;
		for (const TreeSpot& t : trees) offPaths &= Valley::PathDistance(t.x, t.z) >= cfg::kPathHalfWidth + 2.0f;
		Check(offPaths, "trees: none on the trails");
	}

	// ---- Building pads -------------------------------------------------------------------------
	{
		Valley padded;
		const float x = -70.5f, z = -24.0f;   // a slope below the founders' house
		GroundPad pad{ x, z, 6.0f, 5.0f, std::round(padded.Height(x, z) * 4.0f) / 4.0f };
		padded.SetPads({ pad });
		bool flat = true;
		for (float dz = -4.5f; dz <= 4.5f; dz += 1.5f)
			for (float dx = -5.5f; dx <= 5.5f; dx += 1.5f) flat &= std::fabs(padded.Height(x + dx, z + dz) - pad.height) < 1e-4f;
		Check(flat, "pads: the ground under a building is level");
		const float far = x + 6.0f + cfg::kPadBlend + 1.0f;
		Check(std::fabs(padded.Height(far, z) - valley.Height(far, z)) < 1e-4f, "pads: the land beyond the blend is untouched");
	}

	// ---- Trails ----------------------------------------------------------------------------------
	{
		const auto& seg = cfg::kPathSegments[1];
		const float x = 0.5f * (seg[0] + seg[2]), z = 0.5f * (seg[1] + seg[3]);
		Check(Valley::PathDistance(x, z) < 1e-3f, "trails: a segment's midpoint is on the trail");
		Check(valley.GroundAt(x, z, valley.Height(x, z), 0.0f, 0.25f) == Ground::Path, "trails: the ground there is trodden path");
	}

	// ---- Terrain meshes ------------------------------------------------------------------------
	{
		const auto colours = Colours();
		// Flat ground at the mouth: tops cover every column once, and merge into few quads.
		const TerrainPatch flat = SampleTerrain(valley, mouthX - 8.0f, -20.0f, 0.25f, 64);
		const MeshData flatMesh = BuildTerrainMesh(flat, colours);
		Check(std::fabs(FaceArea(flatMesh, { 0, 1, 0 }) - 16.0f * 16.0f) < 1e-2f, "terrain: block tops cover the patch exactly");
		std::printf("        (flat 16 m patch: %zu quads)\n", flatMesh.vertices.size() / 4);
		Check(flatMesh.vertices.size() / 4 < 120, "terrain: flat ground merges into a few dozen quads");
		Check(WindingMatchesNormals(flatMesh), "terrain: triangles face along their normals");

		// Rolling valley floor: still tops covering every column, sides where it steps.
		const TerrainPatch floor = SampleTerrain(valley, -60.0f, -220.0f, 0.25f, 64);
		const MeshData floorMesh = BuildTerrainMesh(floor, colours);
		Check(std::fabs(FaceArea(floorMesh, { 0, 1, 0 }) - 256.0f) < 1e-2f && WindingMatchesNormals(floorMesh), "terrain: a rolling patch is covered and well wound");
		std::printf("        (a 16 m valley-floor chunk: %zu triangles)\n", floorMesh.indices.size() / 3);
		Check(floorMesh.indices.size() / 3 < 30000, "terrain: a valley-floor chunk stays under 30k triangles");
		Check(MeshTriangles(floorMesh).size() == floorMesh.indices.size(), "terrain: collision has the mesh's triangles");

		// The cliff over the cave mouth starts at the grid's top, leaving the grid's face open.
		const TerrainPatch mouth = SampleTerrain(valley, 16.0f, -8.0f, 0.25f, 64);
		const MeshData mouthMesh = BuildTerrainMesh(mouth, colours);
		bool aboveGrid = true, any = false;
		for (const Vertex& v : mouthMesh.vertices)
		{
			if (v.normal.z != -1.0f || std::fabs(v.position.z) > 1e-3f || v.position.x < 0.0f || v.position.x > cfg::kGridX) continue;
			any = true;
			aboveGrid &= v.position.y >= cfg::kGridY - 1e-3f;
		}
		Check(any && aboveGrid, "terrain: the cliff over the cave mouth starts at the grid's top");

		// Skirts: the -x edge reaches 2 m below the tops there.
		const MeshData skirted = BuildTerrainMesh(flat, colours, { 2.0f, 0, 0, 0 });
		float skirtLow = 1e9f;
		for (const Vertex& v : skirted.vertices) if (v.normal.x == -1.0f) skirtLow = std::min(skirtLow, v.position.y);
		Check(std::fabs(skirtLow - (cfg::kGroundY - 2.0f)) < 1e-3f, "terrain: skirts hide cracks along an edge");

		// Water: a patch on the river has a surface at the water level.
		const float rx = cfg::kRiverPoints[5][0], rz = cfg::kRiverPoints[5][1];
		const TerrainPatch river = SampleTerrain(valley, rx - 8.0f, rz - 8.0f, 0.25f, 64);
		const MeshData water = BuildWaterMesh(river, { 0, 0, 1 });
		const float level = valley.WaterLevel(rx, rz);
		bool atLevel = false;
		for (const Vertex& v : water.vertices) atLevel |= v.normal.y == 1.0f && std::fabs(v.position.y - level) < 1e-3f;
		Check(!water.vertices.empty() && atLevel && WindingMatchesNormals(water), "water: a river patch has its surface at the water level");
		Check(BuildWaterMesh(flat, { 0, 0, 1 }).vertices.empty(), "water: none on dry ground");
	}

	return gValleyFailures;
}
