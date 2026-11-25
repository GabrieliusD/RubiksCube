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

	MeshData Shovel()
	{
		MeshData mesh;
		const XMFLOAT4 wood = color::WithAlpha(color::Linear(0x6b4a2b));
		const XMFLOAT4 iron = color::WithAlpha(color::Linear(0x7c8288));
		// Handle along +Y with a grip across the bottom, flat blade at the top.
		AddBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.018f, 0.30f, 0.018f }, wood);
		AddBox(mesh, { 0.0f, -0.30f, 0.0f }, { 0.05f, 0.015f, 0.015f }, wood);
		AddBox(mesh, { 0.0f, 0.31f, 0.0f }, { 0.03f, 0.03f, 0.012f }, iron);      // socket
		AddBox(mesh, { 0.0f, 0.42f, 0.01f }, { 0.08f, 0.09f, 0.006f }, iron);     // blade
		AddBox(mesh, { -0.078f, 0.42f, 0.025f }, { 0.006f, 0.085f, 0.015f }, iron);   // raised sides
		AddBox(mesh, { 0.078f, 0.42f, 0.025f }, { 0.006f, 0.085f, 0.015f }, iron);
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

	MeshData Hammer()
	{
		MeshData mesh;
		const XMFLOAT4 wood = color::WithAlpha(color::Linear(0x6b4a2b));
		const XMFLOAT4 iron = color::WithAlpha(color::Linear(0x8a8f96));
		AddBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.02f, 0.24f, 0.02f }, wood);
		AddBox(mesh, { 0.0f, 0.25f, 0.03f }, { 0.035f, 0.035f, 0.08f }, iron);       // head
		AddBox(mesh, { 0.0f, 0.26f, -0.07f }, { 0.012f, 0.03f, 0.03f }, iron);       // claw
		return mesh;
	}

	MeshData Axe()
	{
		MeshData mesh;
		const XMFLOAT4 wood = color::WithAlpha(color::Linear(0x6b4a2b));
		const XMFLOAT4 iron = color::WithAlpha(color::Linear(0x8a8f96));
		AddBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.022f, 0.32f, 0.022f }, wood);
		AddBox(mesh, { 0.0f, 0.28f, 0.06f }, { 0.012f, 0.05f, 0.06f }, iron);       // the head
		AddBox(mesh, { 0.0f, 0.28f, 0.13f }, { 0.008f, 0.08f, 0.012f }, iron);      // the edge
		return mesh;
	}
}
