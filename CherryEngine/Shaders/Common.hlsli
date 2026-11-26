#ifndef CHERRY_COMMON_HLSLI
#define CHERRY_COMMON_HLSLI

#include "Shared/ShaderInterop.h"

// The global binding model (see RHI/RHI.h). On Vulkan (SPIR-V), b0 is a push constant block.
#if defined(__spirv__)
#define CHERRY_PUSH_CONSTANTS [[vk::push_constant]]
#define CHERRY_LOCATION(n) [[vk::location(n)]]
#else
#define CHERRY_PUSH_CONSTANTS
#define CHERRY_LOCATION(n)
#endif
ConstantBuffer<ViewConstants>  gView  : register(b1);
ConstantBuffer<FrameConstants> gFrame : register(b2);

SamplerState gPointWrap   : register(s0);
SamplerState gPointClamp  : register(s1);
SamplerState gLinearWrap  : register(s2);
SamplerState gLinearClamp : register(s3);
SamplerState gAnisoWrap   : register(s4);
SamplerState gAnisoClamp  : register(s5);
SamplerComparisonState gShadowSampler : register(s6);   // shadow maps: LESS_EQUAL, bilinear, border = lit

float3 SrgbToLinear(float3 c)
{
	return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 LinearToSrgb(float3 c)
{
	c = saturate(c);
	return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float4 UnpackRGBA8(uint packed)
{
	return float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
}

// World-space ray through a pixel centre (pixel coords in render-target space).
void PixelRay(float2 pixel, out float3 origin, out float3 direction)
{
	float2 ndc = (pixel * gView.invRenderTargetSize) * float2(2, -2) + float2(-1, 1);
	float4 farPoint = mul(float4(ndc, 1, 1), gView.invViewProj);
	origin = gView.eyePosW;
	direction = normalize(farPoint.xyz / farPoint.w - origin);
}

#endif
