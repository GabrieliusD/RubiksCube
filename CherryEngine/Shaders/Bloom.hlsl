// Bloom.hlsl
// Glow around bright pixels (RenderSettings::bloom), as a chain of ever smaller textures:
// CSDownsample takes the HDR view to half size (keeping only what's brighter than the
// threshold), then halves it again a few times, blurring as it goes; CSUpsample walks back
// up, adding each smaller level, blurred, onto the next larger one. The tonemap adds the
// largest level to the image. (Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare", 2014.)
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<BloomConstants> gDraw : register(b0);

float Brightness(float3 c) { return max(c.r, max(c.g, c.b)); }

// Keeps what's brighter than the threshold, starting softly over the knee.
float3 Prefilter(float3 c)
{
	const float brightness = Brightness(c);
	float soft = clamp(brightness - gDraw.threshold + gDraw.knee, 0.0, 2.0 * gDraw.knee);
	soft = soft * soft / (4.0 * gDraw.knee + 1e-5);
	return c * max(soft, brightness - gDraw.threshold) / max(brightness, 1e-5);
}

// Weighted so a single very bright pixel can't flicker across frames ("fireflies").
float KarisWeight(float3 c) { return 1.0 / (1.0 + Brightness(c)); }

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.destWidth || id.y >= gDraw.destHeight)
		return;
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.sourceIndex];
	RWTexture2D<float4> dest = ResourceDescriptorHeap[gDraw.destIndex];
	const float2 destTexel = 1.0 / float2(gDraw.destWidth, gDraw.destHeight);
	const float2 uv = (id.xy + 0.5) * destTexel;
	const float2 s = destTexel * 0.5;   // one source texel
	#define TAP(x, y) source.SampleLevel(gLinearClamp, uv + s * float2(x, y), 0).rgb
	const float3 a = TAP(-2, -2), b = TAP(0, -2), c = TAP(2, -2);
	const float3 d = TAP(-2, 0), e = TAP(0, 0), f = TAP(2, 0);
	const float3 g = TAP(-2, 2), h = TAP(0, 2), i = TAP(2, 2);
	const float3 j = TAP(-1, -1), k = TAP(1, -1), l = TAP(-1, 1), m = TAP(1, 1);
	#undef TAP

	float3 result;
	if (gDraw.firstPass != 0)
	{
		// Five overlapping boxes, each weighted down by its brightness.
		const float3 boxes[5] = { (j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25, (d + e + g + h) * 0.25, (e + f + h + i) * 0.25 };
		const float weights[5] = { 0.5, 0.125, 0.125, 0.125, 0.125 };
		float3 sum = 0.0;
		float total = 0.0;
		[unroll] for (int n = 0; n < 5; ++n)
		{
			const float w = weights[n] * KarisWeight(boxes[n]);
			sum += boxes[n] * w;
			total += w;
		}
		result = Prefilter(sum / total);
	}
	else
	{
		result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	}
	dest[id.xy] = float4(result, 1.0);
}

[numthreads(8, 8, 1)]
void CSUpsample(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.destWidth || id.y >= gDraw.destHeight)
		return;
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.sourceIndex];   // the smaller level
	RWTexture2D<float4> dest = ResourceDescriptorHeap[gDraw.destIndex];     // the larger level, added to
	const float2 destTexel = 1.0 / float2(gDraw.destWidth, gDraw.destHeight);
	const float2 uv = (id.xy + 0.5) * destTexel;
	const float2 r = destTexel * 2.0 * gDraw.radius;
	#define TAP(x, y) source.SampleLevel(gLinearClamp, uv + r * float2(x, y), 0).rgb
	const float3 blurred = (TAP(-1, -1) + TAP(1, -1) + TAP(-1, 1) + TAP(1, 1)
		+ 2.0 * (TAP(0, -1) + TAP(-1, 0) + TAP(1, 0) + TAP(0, 1)) + 4.0 * TAP(0, 0)) / 16.0;
	#undef TAP
	dest[id.xy] = float4(dest[id.xy].rgb + blurred, 1.0);
}
