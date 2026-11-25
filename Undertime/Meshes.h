#pragma once
#include <Renderer/RenderTypes.h>
#include <DirectXMath.h>
#include <cstdint>

// Procedural meshes for UNDERTIME's scenery and view models (vertex-coloured, linear colours).
namespace meshes
{
	// Adds an axis-aligned box (centre, half extents) with flat normals.
	void AddBox(MeshData& mesh, const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents, const DirectX::XMFLOAT4& color);

	// View models in camera-local space (+Z forward, +Y up).
	MeshData Pick();
	MeshData Shovel();
	MeshData Lantern();
	MeshData Hammer();
	MeshData Axe();
}
