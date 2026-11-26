// Ssao.hlsl
// Screen-space ambient occlusion (RenderSettings::ssao), from the view depth only.
//   CSMain: for each pixel, a dozen points in the hemisphere over its surface (normal
//           rebuilt from the depth); the ones behind what the depth buffer shows are
//           occluded. Writes the raw result.
//   CSBlur: smooths it over 5x5 pixels, keeping depth edges sharp.
// The tonemap multiplies the world by the result (Tonemap.hlsl).
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<SsaoConstants> gDraw : register(b0);

static const uint kSamples = 12;
// Points in the unit hemisphere around +z, more of them near the centre.
static const float3 kKernel[kSamples] =
{
	float3( 0.52,  0.12, 0.35), float3(-0.31,  0.44, 0.21), float3( 0.07, -0.56, 0.48),
	float3(-0.62, -0.24, 0.30), float3( 0.20,  0.71, 0.55), float3( 0.81, -0.33, 0.20),
	float3(-0.14,  0.09, 0.15), float3(-0.45, -0.70, 0.40), float3( 0.33,  0.30, 0.82),
	float3(-0.88,  0.31, 0.25), float3( 0.11, -0.19, 0.70), float3( 0.60,  0.55, 0.12),
};

// View-space depth (distance along the view direction) from a depth-buffer value.
float ViewZ(float depth)
{
	return gView.proj[3][2] / (depth - gView.proj[2][2]);
}

float3 ViewPosition(float2 uv, float depth)
{
	const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	const float4 p = mul(ndc, gView.invProj);
	return p.xyz / p.w;
}

float Hash12(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
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
		dest[id.xy] = 1.0;   // sky
		return;
	}
	const float2 texel = 1.0 / float2(gDraw.width, gDraw.height);
	const float2 uv = (id.xy + 0.5) * texel;
	const float3 p = ViewPosition(uv, depth);

	// The normal from the neighbours on the side with the smaller depth step (no smearing
	// across edges).
	const uint2 maxId = uint2(gDraw.width - 1, gDraw.height - 1);
	const float3 pr = ViewPosition(uv + float2(texel.x, 0), depthMap[min(id.xy + uint2(1, 0), maxId)]);
	const float3 pl = ViewPosition(uv - float2(texel.x, 0), depthMap[uint2(max(int(id.x) - 1, 0), id.y)]);
	const float3 pd = ViewPosition(uv + float2(0, texel.y), depthMap[min(id.xy + uint2(0, 1), maxId)]);
	const float3 pu = ViewPosition(uv - float2(0, texel.y), depthMap[uint2(id.x, max(int(id.y) - 1, 0))]);
	const float3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
	const float3 dy = abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu;
	float3 n = normalize(cross(dy, dx));
	if (dot(n, p) > 0.0) n = -n;   // towards the camera

	// A basis around the normal, turned by a per-pixel angle (the blur hides the pattern).
	const float angle = Hash12(float2(id.xy % 4) + 0.5) * 6.2831853;
	const float3 helper = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
	float3 t = normalize(cross(helper, n));
	float3 b = cross(n, t);
	const float c = cos(angle), s = sin(angle);
	const float3 t2 = t * c + b * s;
	b = cross(n, t2);
	t = t2;

	float occlusion = 0.0;
	[unroll] for (uint i = 0; i < kSamples; ++i)
	{
		const float3 k = kKernel[i];
		const float3 q = p + (t * k.x + b * k.y + n * k.z) * gDraw.radius;
		const float4 clip = mul(float4(q, 1.0), gView.proj);
		const float2 quv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
		if (any(quv < 0.0) || any(quv > 1.0))
			continue;
		const float sceneZ = ViewZ(depthMap[uint2(quv * float2(gDraw.width, gDraw.height))]);
		const float inRange = smoothstep(0.0, 1.0, gDraw.radius / max(abs(p.z - sceneZ), 1e-4));
		occlusion += (sceneZ <= q.z - gDraw.bias ? 1.0 : 0.0) * inRange;
	}
	dest[id.xy] = 1.0 - occlusion / kSamples;
}

[numthreads(8, 8, 1)]
void CSBlur(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
		return;
	Texture2D<float> source = ResourceDescriptorHeap[gDraw.sourceIndex];
	Texture2D<float> depthMap = ResourceDescriptorHeap[gDraw.depthIndex];
	RWTexture2D<float> dest = ResourceDescriptorHeap[gDraw.destIndex];
	const float depth = depthMap[id.xy];
	if (depth >= 1.0)
	{
		dest[id.xy] = 1.0;
		return;
	}
	const float z = ViewZ(depth);
	float sum = 0.0, weight = 0.0;
	[unroll] for (int y = -2; y <= 2; ++y)
		[unroll] for (int x = -2; x <= 2; ++x)
		{
			const int2 at = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(gDraw.width - 1, gDraw.height - 1));
			const float d = depthMap[at];
			const float w = d >= 1.0 ? 0.0 : exp(-abs(ViewZ(d) - z) / (0.05 * z + 0.05));
			sum += source[at] * w;
			weight += w;
		}
	dest[id.xy] = weight > 0.0 ? sum / weight : source[id.xy];
}
