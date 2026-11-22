// Fxaa.hlsl
// Anti-aliasing after the tonemap (FXAA, after Timothy Lottes): along edges found from the
// brightness of neighbouring pixels, blends a few samples along the edge, so stair-stepped
// edges (block edges far away, thin branches) look smooth. Reads the tonemapped image,
// writes the view.
#include "Common.hlsli"

ConstantBuffer<FxaaConstants> gDraw : register(b0);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
		return;
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.sourceIndex];
	RWTexture2D<float4> dest = ResourceDescriptorHeap[gDraw.destIndex];
	const float2 texel = 1.0 / float2(gDraw.width, gDraw.height);
	const float2 uv = (id.xy + 0.5) * texel;

	const float3 rgbM = source.SampleLevel(gLinearClamp, uv, 0).rgb;
	const float lumaNW = Luma(source.SampleLevel(gLinearClamp, uv + float2(-1, -1) * texel, 0).rgb);
	const float lumaNE = Luma(source.SampleLevel(gLinearClamp, uv + float2(1, -1) * texel, 0).rgb);
	const float lumaSW = Luma(source.SampleLevel(gLinearClamp, uv + float2(-1, 1) * texel, 0).rgb);
	const float lumaSE = Luma(source.SampleLevel(gLinearClamp, uv + float2(1, 1) * texel, 0).rgb);
	const float lumaM = Luma(rgbM);
	const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
	const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

	// Flat areas stay as they are.
	if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
	{
		dest[id.xy] = float4(rgbM, 1.0);
		return;
	}

	// The edge's direction, then samples along it.
	float2 dir = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
	const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
	const float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
	dir = clamp(dir * scale, -8.0, 8.0) * texel;

	const float3 rgbA = 0.5 * (source.SampleLevel(gLinearClamp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb
		+ source.SampleLevel(gLinearClamp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
	const float3 rgbB = rgbA * 0.5 + 0.25 * (source.SampleLevel(gLinearClamp, uv - dir * 0.5, 0).rgb
		+ source.SampleLevel(gLinearClamp, uv + dir * 0.5, 0).rgb);
	const float lumaB = Luma(rgbB);
	dest[id.xy] = float4(lumaB < lumaMin || lumaB > lumaMax ? rgbA : rgbB, 1.0);
}
