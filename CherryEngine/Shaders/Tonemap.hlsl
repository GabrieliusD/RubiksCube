// HDR -> LDR tonemap as a compute pass (exercises the compute + UAV path).
// Writes gamma-encoded values into a UNORM UAV; the LDR texture is then shown
// by ImGui or blitted to the back buffer without further conversion.
#include "Common.hlsli"

ConstantBuffer<TonemapConstants> gDraw : register(b0);

// Krzysztof Narkowicz's ACES filmic fit.
float3 ACESFilm(float3 x)
{
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
	{
		return;
	}

	Texture2D<float4> hdr = ResourceDescriptorHeap[gDraw.hdrTextureIndex];
	RWTexture2D<float4> output = ResourceDescriptorHeap[gDraw.outputTextureIndex];

	float3 color = hdr[id.xy].rgb;
	if (gDraw.bloomTextureIndex != CHERRY_INVALID_INDEX)
	{
		// Bloom (Bloom.hlsl) at half size, stretched over the view.
		Texture2D<float4> bloom = ResourceDescriptorHeap[gDraw.bloomTextureIndex];
		const float2 uv = (id.xy + 0.5) / float2(gDraw.width, gDraw.height);
		color += bloom.SampleLevel(gLinearClamp, uv, 0).rgb * gDraw.bloomIntensity;
	}
	if (gDraw.shaftTextureIndex != CHERRY_INVALID_INDEX)
	{
		// Light shafts (LightShafts.hlsl) at half size, over the world only (as AO below).
		Texture2D<float> depth = ResourceDescriptorHeap[gDraw.depthTextureIndex];
		if (depth[id.xy] >= 1.0)
		{
			Texture2D<float4> shafts = ResourceDescriptorHeap[gDraw.shaftTextureIndex];
			const float2 uv = (id.xy + 0.5) / float2(gDraw.width, gDraw.height);
			color += shafts.SampleLevel(gLinearClamp, uv, 0).rgb;
		}
	}
	if (gDraw.aoTextureIndex != CHERRY_INVALID_INDEX)
	{
		// Ambient occlusion (Ssao.hlsl) darkens the world; view models (drawn after the
		// depth was cleared, so their depth isn't at the far plane) are left alone.
		Texture2D<float> depth = ResourceDescriptorHeap[gDraw.depthTextureIndex];
		if (depth[id.xy] >= 1.0)
		{
			Texture2D<float> ao = ResourceDescriptorHeap[gDraw.aoTextureIndex];
			color *= lerp(1.0, ao[id.xy], gDraw.aoStrength);
		}
	}
	color *= gView.exposure;
	if (gDraw.exposureTextureIndex != CHERRY_INVALID_INDEX)
	{
		// Eye adaptation (EyeAdaptation.hlsl).
		Texture2D<float> adapted = ResourceDescriptorHeap[gDraw.exposureTextureIndex];
		color *= adapted[uint2(0, 0)];
	}
	const bool grade = gFrame.gradeEnabled != 0;
	if (grade)
	{
		// White balance and saturation on the scene's light, before the tone curve.
		color *= gFrame.gradeTint;
		const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
		color = max(lerp(luma.xxx, color, gFrame.gradeSaturation), 0.0);
	}
	color = ACESFilm(color);
	float3 display = LinearToSrgb(color);
	if (grade)
	{
		// Contrast, lift and gain on what's shown, then the vignette.
		display = saturate((display - 0.5) * gFrame.gradeContrast + 0.5);
		display = saturate(display * gFrame.gradeGain + gFrame.gradeLift * (1.0 - display));
		const float2 centred = ((id.xy + 0.5) / float2(gDraw.width, gDraw.height) - 0.5) * float2(float(gDraw.width) / gDraw.height, 1.0);
		const float corner = smoothstep(0.35, 1.0, length(centred) * 1.25);
		display *= 1.0 - gFrame.gradeVignette * corner;
	}
	output[id.xy] = float4(display, 1.0);
}
