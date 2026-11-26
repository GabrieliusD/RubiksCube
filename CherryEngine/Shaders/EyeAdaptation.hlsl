// EyeAdaptation.hlsl
// Exposure that follows how bright the view is (RenderSettings::eyeAdaptation), like the eye.
//   CSReduce: each 16x16 block of the HDR view sums its log brightness (centre-weighted,
//             clamped so the sun or a black corner can't swing it) into one texel.
//   CSAdapt:  one group sums those, works out the exposure that would show the view at the
//             key brightness, and eases the stored exposure towards it: quickly when the view
//             got brighter (being dazzled), slowly when it got darker (eyes opening up).
// The tonemap multiplies by the stored exposure (Tonemap.hlsl). Nothing is read back.
#include "Common.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<EyeAdaptationConstants> gDraw : register(b0);

groupshared float2 gShared[256];

[numthreads(16, 16, 1)]
void CSReduce(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
	float2 sum = 0.0;   // (weighted log2 brightness, weight)
	if (id.x < gDraw.width && id.y < gDraw.height)
	{
		Texture2D<float4> hdr = ResourceDescriptorHeap[gDraw.sourceIndex];
		const float luma = dot(hdr[id.xy].rgb, float3(0.2126, 0.7152, 0.0722));
		const float logLuma = clamp(log2(max(luma, 1e-6)), gDraw.minLog, gDraw.maxLog);
		const float2 uv = (id.xy + 0.5) / float2(gDraw.width, gDraw.height) - 0.5;
		const float weight = 1.0 - 0.75 * saturate(length(uv) * 1.6);   // the middle counts most
		sum = float2(logLuma * weight, weight);
	}
	gShared[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (index < stride)
			gShared[index] += gShared[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0)
	{
		RWTexture2D<float2> blocks = ResourceDescriptorHeap[gDraw.blocksIndex];
		blocks[group.xy] = gShared[0];
	}
}

[numthreads(256, 1, 1)]
void CSAdapt(uint index : SV_GroupIndex)
{
	Texture2D<float2> blocks = ResourceDescriptorHeap[gDraw.blocksReadIndex];
	float2 sum = 0.0;
	const uint count = gDraw.blocksX * gDraw.blocksY;
	for (uint i = index; i < count; i += 256)
		sum += blocks[uint2(i % gDraw.blocksX, i / gDraw.blocksX)];
	gShared[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (index < stride)
			gShared[index] += gShared[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index != 0)
		return;

	const float average = exp2(gShared[0].x / max(gShared[0].y, 1e-6));
	const float target = clamp(gDraw.key / max(average, 1e-6), gDraw.minExposure, gDraw.maxExposure);
	RWTexture2D<float> exposure = ResourceDescriptorHeap[gDraw.exposureIndex];
	const float previous = exposure[uint2(0, 0)];
	float now = target;
	if (gDraw.reset == 0 && previous > 0.0)
	{
		// In stops (log2), so a doubling takes as long either way it's going.
		const float speed = target < previous ? gDraw.speedBrighter : gDraw.speedDarker;
		now = exp2(lerp(log2(previous), log2(target), 1.0 - exp(-gFrame.deltaTime * speed)));
	}
	exposure[uint2(0, 0)] = now;
}
