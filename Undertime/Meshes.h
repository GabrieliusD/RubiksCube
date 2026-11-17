#pragma once
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <cstdint>

// Procedural meshes for UNDERTIME's scenery and view models (vertex-coloured, linear colours).
namespace meshes
{
	// Adds an axis-aligned box (centre, half extents) with flat normals.
	void AddBox(MeshData& mesh, const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents, const DirectX::XMFLOAT4& color);

	// Flat ground at y = 1 around the mountain, with a hole where the grid is.
	MeshData Ground();
	// Low-poly mountain shell over the grid, with the entrance face left open.
	MeshData Mountain();
	// A simple tree (trunk + two cone layers) standing at the origin.
	MeshData Tree(float height, uint32_t seed);

	// View models in camera-local space (+Z forward, +Y up).
	MeshData Pick();
	MeshData Lantern();
}
