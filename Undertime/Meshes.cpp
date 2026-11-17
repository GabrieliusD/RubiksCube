#include "Meshes.h"
#include "Config.h"
#include "ColorUtil.h"
#include <cmath>
#include <algorithm>
#include <vector>

using namespace DirectX;

namespace
{
	void AddTriangle(MeshData& mesh, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c, const XMFLOAT4& colour)
	{
		const XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b), vc = XMLoadFloat3(&c);
		XMFLOAT3 normal;
		XMStoreFloat3(&normal, XMVector3Normalize(XMVector3Cross(vb - va, vc - va)));
		const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
		for (const XMFLOAT3* p : { &a, &b, &c })
		{
			Vertex v;
			v.position = *p;
			v.normal = normal;
			v.color = colour;
			mesh.vertices.push_back(v);
		}
		mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2 });
	}

	void AddQuad(MeshData& mesh, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c, const XMFLOAT3& d, const XMFLOAT4& colour)
	{
		AddTriangle(mesh, a, b, c, colour);
		AddTriangle(mesh, a, c, d, colour);
	}

	// Cone or cylinder around +Y from y0 to y1.
	void AddCone(MeshData& mesh, float y0, float y1, float r0, float r1, int slices, const XMFLOAT4& colour)
	{
		for (int i = 0; i < slices; ++i)
		{
			const float a0 = XM_2PI * i / slices, a1 = XM_2PI * (i + 1) / slices;
			const XMFLOAT3 b0{ std::cos(a0) * r0, y0, std::sin(a0) * r0 }, b1{ std::cos(a1) * r0, y0, std::sin(a1) * r0 };
			const XMFLOAT3 t0{ std::cos(a0) * r1, y1, std::sin(a0) * r1 }, t1{ std::cos(a1) * r1, y1, std::sin(a1) * r1 };
			if (r1 > 0.0f) AddQuad(mesh, b0, t0, t1, b1, colour);
			else AddTriangle(mesh, b0, t0, b1, colour);   // pointed top: the quad would be degenerate
			AddTriangle(mesh, { 0, y0, 0 }, b0, b1, colour);
			if (r1 > 0.0f) AddTriangle(mesh, { 0, y1, 0 }, t1, t0, colour);
		}
	}

	float MountainHeight(float x, float z)
	{
		// A broad hump centred over the grid, roughened with a few sines; never lower than
		// the grid ceiling above the grid itself.
		const float dx = x - (cfg::kCenterX + 0.5f);
		const float dz = z - 55.0f;
		const float hump = std::exp(-(dx * dx) / (2.0f * 32.0f * 32.0f)) * std::exp(-(dz * dz) / (2.0f * 58.0f * 58.0f));
		const float rough = 2.5f * std::sin(x * 0.21f + z * 0.13f) + 1.8f * std::sin(x * 0.07f - z * 0.19f + 1.3f);
		float h = cfg::kGroundY + 48.0f * hump + rough * hump;
		const bool overGrid = x >= -1.0f && x <= cfg::kGridX + 1.0f && z <= cfg::kGridZ + 1.0f;
		if (overGrid) h = std::max(h, static_cast<float>(cfg::kGridY) + 1.5f);
		return std::max(h, cfg::kGroundY);
	}
}

namespace meshes
{
	void AddBox(MeshData& mesh, const XMFLOAT3& c, const XMFLOAT3& h, const XMFLOAT4& colour)
	{
		const XMFLOAT3 p[8] =
		{
			{ c.x - h.x, c.y - h.y, c.z - h.z }, { c.x + h.x, c.y - h.y, c.z - h.z },
			{ c.x + h.x, c.y + h.y, c.z - h.z }, { c.x - h.x, c.y + h.y, c.z - h.z },
			{ c.x - h.x, c.y - h.y, c.z + h.z }, { c.x + h.x, c.y - h.y, c.z + h.z },
			{ c.x + h.x, c.y + h.y, c.z + h.z }, { c.x - h.x, c.y + h.y, c.z + h.z },
		};
		AddQuad(mesh, p[0], p[3], p[2], p[1], colour);   // -Z
		AddQuad(mesh, p[5], p[6], p[7], p[4], colour);   // +Z
		AddQuad(mesh, p[4], p[7], p[3], p[0], colour);   // -X
		AddQuad(mesh, p[1], p[2], p[6], p[5], colour);   // +X
		AddQuad(mesh, p[3], p[7], p[6], p[2], colour);   // +Y
		AddQuad(mesh, p[4], p[0], p[1], p[5], colour);   // -Y
	}

	MeshData Ground()
	{
		MeshData mesh;
		const XMFLOAT4 grass = color::WithAlpha(color::Linear(cfg::kGrass));
		const float y = cfg::kGroundY;
		const float minX = -90.0f, maxX = 110.0f, minZ = -90.0f, maxZ = 220.0f;
		const float gx0 = 0.0f, gx1 = static_cast<float>(cfg::kGridX), gz1 = static_cast<float>(cfg::kGridZ);
		// Four quads around the grid footprint, so the tunnel floor never z-fights with the ground.
		AddQuad(mesh, { minX, y, minZ }, { minX, y, 0.0f }, { maxX, y, 0.0f }, { maxX, y, minZ }, grass);
		AddQuad(mesh, { minX, y, 0.0f }, { minX, y, maxZ }, { gx0, y, maxZ }, { gx0, y, 0.0f }, grass);
		AddQuad(mesh, { gx1, y, 0.0f }, { gx1, y, maxZ }, { maxX, y, maxZ }, { maxX, y, 0.0f }, grass);
		AddQuad(mesh, { gx0, y, gz1 }, { gx0, y, maxZ }, { gx1, y, maxZ }, { gx1, y, gz1 }, grass);
		return mesh;
	}

	MeshData Mountain()
	{
		MeshData mesh;
		const float step = 3.0f;
		const float minX = -48.0f, maxX = 69.0f, maxZ = 150.0f;
		const XMFLOAT3 low = color::Linear(cfg::kMountainRock);
		const XMFLOAT3 high = color::Linear(cfg::kMountainHigh);
		auto shade = [&](float h, float jitter)
		{
			const float t = std::clamp((h - 8.0f) / 40.0f, 0.0f, 1.0f);
			return color::WithAlpha(color::Scale(color::Lerp(low, high, t), 0.9f + 0.2f * jitter));
		};

		// Heightfield slopes from the front face (z = 0) to the back.
		for (float z = 0.0f; z < maxZ; z += step)
		{
			for (float x = minX; x < maxX; x += step)
			{
				const XMFLOAT3 a{ x, MountainHeight(x, z), z };
				const XMFLOAT3 b{ x, MountainHeight(x, z + step), z + step };
				const XMFLOAT3 c{ x + step, MountainHeight(x + step, z + step), z + step };
				const XMFLOAT3 d{ x + step, MountainHeight(x + step, z), z };
				const float j = color::Hash01(static_cast<int>(x), 0, static_cast<int>(z));
				AddTriangle(mesh, a, b, c, shade((a.y + b.y + c.y) / 3.0f, j));
				AddTriangle(mesh, a, c, d, shade((a.y + c.y + d.y) / 3.0f, 1.0f - j));
			}
		}

		// Vertical front face at z = 0, leaving the grid's face (where the crack is) open.
		const XMFLOAT4 cliff = color::WithAlpha(color::Scale(low, 0.85f));
		for (float x = minX; x < maxX; x += step)
		{
			const float h0 = MountainHeight(x, 0.0f), h1 = MountainHeight(x + step, 0.0f);
			const bool overGrid = x >= 0.0f && x + step <= cfg::kGridX;
			const float bottom = overGrid ? static_cast<float>(cfg::kGridY) : cfg::kGroundY;
			AddQuad(mesh, { x, bottom, 0.0f }, { x, h0, 0.0f }, { x + step, h1, 0.0f }, { x + step, bottom, 0.0f }, cliff);
		}
		return mesh;
	}

	MeshData Tree(float height, uint32_t seed)
	{
		MeshData mesh;
		const XMFLOAT4 trunk = color::WithAlpha(color::Linear(cfg::kTreeTrunk));
		const float shade = 0.85f + 0.3f * color::Hash01(static_cast<int>(seed), 7, 3);
		const XMFLOAT4 leaves = color::WithAlpha(color::Scale(color::Linear(cfg::kTreeLeaves), shade));
		AddCone(mesh, 0.0f, height * 0.35f, 0.18f, 0.14f, 6, trunk);
		AddCone(mesh, height * 0.25f, height * 0.75f, height * 0.28f, height * 0.08f, 7, leaves);
		AddCone(mesh, height * 0.55f, height, height * 0.2f, 0.0f, 7, leaves);
		return mesh;
	}

	MeshData Pick()
	{
		MeshData mesh;
		const XMFLOAT4 wood = color::WithAlpha(color::Linear(0x6b4a2b));
		const XMFLOAT4 iron = color::WithAlpha(color::Linear(0x8a8f96));
		// Handle along +Y, head across the top pointing forward/back.
		AddBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.022f, 0.30f, 0.022f }, wood);
		AddBox(mesh, { 0.0f, 0.29f, 0.02f }, { 0.03f, 0.035f, 0.17f }, iron);
		AddBox(mesh, { 0.0f, 0.275f, 0.20f }, { 0.018f, 0.022f, 0.05f }, iron);    // forward spike
		AddBox(mesh, { 0.0f, 0.28f, -0.16f }, { 0.02f, 0.025f, 0.03f }, iron);     // back
		return mesh;
	}

	MeshData Lantern()
	{
		MeshData mesh;
		const XMFLOAT4 brass = color::WithAlpha(color::Linear(0x8c6a2f));
		const XMFLOAT4 glass = color::WithAlpha(color::Scale(color::Linear(0xffd58a), 3.0f));   // bright: reads as glowing
		AddBox(mesh, { 0.0f, -0.075f, 0.0f }, { 0.065f, 0.012f, 0.065f }, brass);  // base
		AddBox(mesh, { 0.0f, 0.075f, 0.0f }, { 0.06f, 0.012f, 0.06f }, brass);     // cap
		AddBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.048f, 0.065f, 0.048f }, glass);     // glowing glass
		for (float sx : { -1.0f, 1.0f })
			for (float sz : { -1.0f, 1.0f })
				AddBox(mesh, { sx * 0.055f, 0.0f, sz * 0.055f }, { 0.008f, 0.07f, 0.008f }, brass);   // posts
		AddBox(mesh, { 0.0f, 0.11f, 0.0f }, { 0.04f, 0.006f, 0.006f }, brass);     // handle
		return mesh;
	}
}
