#pragma once
#include "CherryAPI.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>
#include <cstdint>

// Immediate-mode world-space debug lines. Anything added during a frame is drawn
// on top of the scene (depth-tested) at the end of that frame, then cleared.
class CHERRY_API DebugDraw
{
public:
	struct LineVertex
	{
		DirectX::XMFLOAT3 position;
		uint32_t color;
	};

	static uint32_t Color(float r, float g, float b, float a = 1.0f);

	void Line(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t color);
	void Box(const DirectX::BoundingBox& box, uint32_t color);
	void Box(const DirectX::BoundingOrientedBox& box, uint32_t color);
	// Eight box corners in DirectXCollision order (near face 0-3, far face 4-7).
	void Corners(const DirectX::XMFLOAT3 corners[8], uint32_t color);
	void Axes(const DirectX::XMFLOAT3& origin, float length);
	void Ray(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction, float length, uint32_t color);

	const std::vector<LineVertex>& Vertices() const { return mVertices; }
	void Clear() { mVertices.clear(); }
	bool enabled = true;

private:
	std::vector<LineVertex> mVertices;
};
