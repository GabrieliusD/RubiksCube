#include "Renderer/DebugDraw.h"
#include <algorithm>

using namespace DirectX;

uint32_t DebugDraw::Color(float r, float g, float b, float a)
{
	auto to8 = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return to8(r) | (to8(g) << 8) | (to8(b) << 16) | (to8(a) << 24);
}

void DebugDraw::Line(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t color)
{
	if (!enabled) return;
	mVertices.push_back({ a, color });
	mVertices.push_back({ b, color });
}

void DebugDraw::Corners(const XMFLOAT3 c[8], uint32_t color)
{
	// DirectXCollision corner order: 0-3 near face, 4-7 far face.
	static const int edges[12][2] =
	{
		{0,1},{1,2},{2,3},{3,0},
		{4,5},{5,6},{6,7},{7,4},
		{0,4},{1,5},{2,6},{3,7},
	};
	for (const auto& e : edges)
	{
		Line(c[e[0]], c[e[1]], color);
	}
}

void DebugDraw::Box(const BoundingBox& box, uint32_t color)
{
	XMFLOAT3 corners[8];
	box.GetCorners(corners);
	Corners(corners, color);
}

void DebugDraw::Box(const BoundingOrientedBox& box, uint32_t color)
{
	XMFLOAT3 corners[8];
	box.GetCorners(corners);
	Corners(corners, color);
}

void DebugDraw::Axes(const XMFLOAT3& o, float length)
{
	Line(o, { o.x + length, o.y, o.z }, Color(1, 0.2f, 0.2f));
	Line(o, { o.x, o.y + length, o.z }, Color(0.2f, 1, 0.2f));
	Line(o, { o.x, o.y, o.z + length }, Color(0.3f, 0.5f, 1));
}

void DebugDraw::Ray(const XMFLOAT3& origin, const XMFLOAT3& direction, float length, uint32_t color)
{
	Line(origin, { origin.x + direction.x * length, origin.y + direction.y * length, origin.z + direction.z * length }, color);
}
