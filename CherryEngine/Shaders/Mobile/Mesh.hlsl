// The mobile mesh pass: the engine's built-in surface (base colour x vertex colour x albedo
// texture), lit as Shaders/LightingUtil.hlsl lights it (GGX sun with its shadow, sky/ground
// ambient), into the HDR target. Point lights, fill lights, fog, interiors: not yet.
#include "Common.hlsli"

[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] Texture2D gAlbedo;
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] SamplerState gAlbedoSampler;

struct VertexIn
{
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(1)]] float3 normal : NORMAL;
	[[vk::location(2)]] float2 uv : TEXCOORD0;
	[[vk::location(3)]] float4 color : COLOR0;
};

struct VertexOut
{
	float4 positionH : SV_Position;
	[[vk::location(0)]] float3 positionW : POSITION;
	[[vk::location(1)]] float3 normalW : NORMAL;
	[[vk::location(2)]] float2 uv : TEXCOORD0;
	[[vk::location(3)]] float4 color : COLOR0;
};

VertexOut VSMain(VertexIn vin, uint instanceId : SV_InstanceID)
{
	const InstanceGpu instance = gInstances[gDraw.firstInstance + instanceId];
	VertexOut vout;
	const float4 positionW = mul(float4(vin.position, 1.0), instance.world);
	vout.positionW = positionW.xyz;
	vout.positionH = mul(positionW, gFrame.viewProj);
	vout.normalW = mul(vin.normal, (float3x3)instance.normalMatrix);
	vout.uv = vin.uv;
	vout.color = vin.color * instance.tint;
	return vout;
}

float D_GGX(float NoH, float alpha)
{
	const float a2 = alpha * alpha;
	const float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / (CHERRY_PI * d * d);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float alpha)
{
	const float a2 = alpha * alpha;
	const float ggxL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	const float ggxV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	return 0.5 / max(ggxV + ggxL, 1e-5);
}

float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	const float4 r = roughness * c0 + c1;
	const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

// 1 lit, 0 in shadow: 3x3 bilinear comparisons, the lookup pushed along the normal.
float SunShadow(float3 positionW, float3 N, float3 L)
{
	if (gFrame.eye.w == 0.0)
		return 1.0;
	const float push = gFrame.sunDirection.w * (1.5 - saturate(dot(N, L)));
	const float3 uvz = mul(float4(positionW + N * gFrame.sunStrength.w * push, 1.0), gFrame.shadowUv).xyz;
	if (any(uvz.xy <= 0.0) || any(uvz.xy >= 1.0) || uvz.z >= 1.0)
		return 1.0;
	const float texel = gFrame.cameraUp.w;
	float lit = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			lit += gShadowMap.SampleCmpLevelZero(gShadowSampler, uvz.xy + float2(x, y) * texel, uvz.z);
	return lit / 9.0;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	const float4 albedo = gDraw.baseColor * pin.color * gAlbedo.Sample(gAlbedoSampler, pin.uv);
	const float3 N = normalize(pin.normalW);
	const float3 V = normalize(gFrame.eye.xyz - pin.positionW);
	const float metallic = saturate(gDraw.metallic);
	const float roughness = clamp(gDraw.roughness, 0.02, 1.0);
	const float alpha = max(roughness * roughness, 0.002);
	const float3 diffuseColor = albedo.rgb * (1.0 - metallic);
	const float3 f0 = lerp(gDraw.f0.xxx, albedo.rgb, metallic);

	float3 color = 0.0;
	// The sun.
	const float3 L = -normalize(gFrame.sunDirection.xyz);
	const float NoL = saturate(dot(N, L));
	if (NoL > 0.0)
	{
		const float3 H = normalize(V + L);
		const float NoV = max(dot(N, V), 1e-4);
		const float NoH = saturate(dot(N, H));
		const float VoH = saturate(dot(V, H));
		const float3 F = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
		const float3 specular = D_GGX(NoH, alpha) * V_SmithGGXCorrelated(NoV, NoL, alpha) * F;
		const float3 diffuse = diffuseColor * (1.0 - F);
		color += (diffuse + specular * CHERRY_PI) * gFrame.sunStrength.rgb * SunShadow(pin.positionW, N, L) * NoL;
	}
	// Ambient: the sky/ground hemisphere.
	const float3 sky = gFrame.ambientSky.rgb, ground = gFrame.ambientGround.rgb;
	const float3 irradiance = lerp(ground, sky, saturate(N.y * 0.5 + 0.5));
	const float3 R = reflect(-V, N);
	const float horizon = 0.03 + roughness;
	float3 reflected = lerp(ground, sky, smoothstep(-horizon, horizon, R.y));
	reflected = lerp(reflected, irradiance, roughness * roughness);
	color += irradiance * diffuseColor + reflected * EnvBRDFApprox(f0, roughness, max(dot(N, V), 1e-4));
	return float4(color, albedo.a);
}
