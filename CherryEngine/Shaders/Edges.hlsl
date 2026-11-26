// Edges.hlsl
// Edge highlights (RenderSettings::edgeHighlights): a thin lighter line along the convex
// edges of blocks, so they catch the light and read crisply, found from the view depth.
//   A pixel on a convex edge is nearer than the average of its neighbours on both sides (on
//   a flat surface it's exactly between them: the depth buffer is linear across a plane).
//   Silhouettes (a big jump in depth on one side) are skipped, so nothing gets outlined
//   against the sky. It fades out with distance (far blocks would only shimmer).
// Uses SsaoConstants: radius = the fade distance (metres), bias = how sharp an edge must be.
// The tonemap brightens the world by the result (Tonemap.hlsl).
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<SsaoConstants> gDraw : register(b0);

float ViewZ(float depth)
{
	return gView.proj[3][2] / (depth - gView.proj[2][2]);
}

float Ridge(Texture2D<float> depthMap, int2 at, int2 step, float zc)
{
	const int2 maxId = int2(gDraw.width - 1, gDraw.height - 1);
	const float za = ViewZ(depthMap[clamp(at - step, int2(0, 0), maxId)]);
	const float zb = ViewZ(depthMap[clamp(at + step, int2(0, 0), maxId)]);
	if (abs(za - zb) > 0.08 * zc + 0.05)
		return 0.0;   // a silhouette, not an edge
	const float bulge = 0.5 * (za + zb) - zc;   // > 0: the middle stands out towards the eye
	return saturate(bulge / (gDraw.bias * zc + 0.002));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
		return;
	Texture2D<float> depthMap = ResourceDescriptorHeap[gDraw.depthIndex];
	RWTexture2D<float> dest = ResourceDescriptorHeap[gDraw.destIndex];
	const float depth = depthMap[id.xy];
	if (depth >= 1.0)
	{
		dest[id.xy] = 0.0;
		return;
	}
	const float zc = ViewZ(depth);
	const int2 at = int2(id.xy);
	float ridge = max(Ridge(depthMap, at, int2(1, 0), zc), Ridge(depthMap, at, int2(0, 1), zc));
	ridge = max(ridge, 0.6 * max(Ridge(depthMap, at, int2(1, 1), zc), Ridge(depthMap, at, int2(1, -1), zc)));
	// Close up, two pixels wide (one is lost in the anti-aliasing).
	if (zc < gDraw.radius * 0.35)
		ridge = max(ridge, 0.7 * max(Ridge(depthMap, at, int2(2, 0), zc), Ridge(depthMap, at, int2(0, 2), zc)));
	dest[id.xy] = ridge * (1.0 - smoothstep(gDraw.radius * 0.5, gDraw.radius, zc));
}
