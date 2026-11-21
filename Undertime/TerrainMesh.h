#pragma once
// Blocky terrain meshes: a square patch of ground columns (each `block` metres wide, its top
// rounded to whole blocks) turned into one mesh with greedy-merged faces, plus the same
// triangles for collision. Water surfaces the same way. Pure C++ (no engine), for tests.
#include "Valley.h"
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <array>
#include <vector>

// The columns of one chunk plus a one-column border (for the faces at its edges).
struct TerrainPatch
{
	float x0 = 0, z0 = 0;      // world position of the first inner column's corner
	float block = 0.25f;       // column width and height step, metres
	int n = 0;                 // inner columns per side
	std::vector<float> top;    // (n + 2)^2, x fastest; [0] is the border column at (x0 - block, z0 - block)
	std::vector<float> bottom; // where each column's ground starts (-1e9 normally; the grid top over the cave)
	std::vector<float> water;  // water surface, or Valley::kNoWater
	std::vector<Ground> ground;

	int Index(int i, int j) const { return (i + 1) + (n + 2) * (j + 1); }   // i, j in -1..n
	float Top(int i, int j) const { return top[Index(i, j)]; }
};

// Samples the valley into a patch (heights rounded to whole blocks).
TerrainPatch SampleTerrain(const Valley& valley, float x0, float z0, float block, int n);

// The ground mesh: block tops, and block sides where a neighbour is lower (grass and snow
// get a one-block band of their colour on top of dirt or rock). `skirt[side]` (-x, +x, -z,
// +z): extra metres the sides along that edge reach down, hiding cracks against a
// neighbouring patch of a different block size. Vertex colour = the ground colour (linear),
// uv.y = the block size (for the voxel material's per-block shading).
// `shapeOnly`: ignore the ground types (one colour, no bands), so faces merge further;
// for collision, which only needs the shape.
MeshData BuildTerrainMesh(const TerrainPatch& patch, const std::array<DirectX::XMFLOAT3, static_cast<int>(Ground::Count)>& colours,
	const std::array<float, 4>& skirt = { 0, 0, 0, 0 }, bool shapeOnly = false);

// Appends `part` to `mesh` (merging chunks into fewer, larger meshes).
void AppendMesh(MeshData& mesh, const MeshData& part);

// Water surfaces: tops where there's water above the ground, and falls where the water
// steps down to a neighbour's lower water. Empty when the patch has no water.
MeshData BuildWaterMesh(const TerrainPatch& patch, const DirectX::XMFLOAT3& colour);

// The mesh's triangles as a flat list (3 points each), for PhysicsWorld::CreateStaticMesh.
std::vector<DirectX::XMFLOAT3> MeshTriangles(const MeshData& mesh);
