// LightingUtil.hlsl
// Physically based shading (metallic/roughness, GGX), used by the mesh and voxel passes.
//
// Light units: a light's Strength is the light arriving on a surface facing it, so a white
// matte surface under a light of Strength 1 shows its albedo (Lambert without the 1/pi).
// The specular term is scaled to match.
#ifndef CHERRY_LIGHTING_UTIL_HLSL
#define CHERRY_LIGHTING_UTIL_HLSL

#include "Common.hlsli"

static const float CHERRY_PI = 3.14159265;

// What the lighting needs to know about a surface point.
struct PbrSurface
{
	float3 diffuseColor;   // baseColor * (1 - metallic)
	float3 f0;             // specular reflectance at normal incidence
	float  alpha;          // GGX roughness (perceptual roughness squared)
	float  roughness;      // perceptual roughness
};

// baseColor linear, metallic 0..1, roughness 0..1 (perceptual),
// dielectricF0: the non-metal reflectance (0.04 for most materials).
PbrSurface MakePbrSurface(float3 baseColor, float metallic, float roughness, float3 dielectricF0)
{
	PbrSurface s;
	metallic = saturate(metallic);
	s.roughness = clamp(roughness, 0.02, 1.0);
	s.alpha = max(s.roughness * s.roughness, 0.002);
	s.diffuseColor = baseColor * (1.0 - metallic);
	s.f0 = lerp(dielectricF0, baseColor, metallic);
	return s;
}

float D_GGX(float NoH, float alpha)
{
	float a2 = alpha * alpha;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / (CHERRY_PI * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 NoL NoV) of the microfacet BRDF).
float V_SmithGGXCorrelated(float NoV, float NoL, float alpha)
{
	float a2 = alpha * alpha;
	float ggxL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
	float ggxV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
	return 0.5 / max(ggxV + ggxL, 1e-5);
}

float3 F_Schlick(float3 f0, float VoH)
{
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (1.0 - f0) * f;
}

// Light reflected towards V from one light arriving along L with strength E.
float3 ShadeLight(PbrSurface s, float3 N, float3 V, float3 L, float3 E)
{
	float NoL = saturate(dot(N, L));
	if (NoL <= 0.0)
		return 0.0;
	float3 H = normalize(V + L);
	float NoV = max(dot(N, V), 1e-4);
	float NoH = saturate(dot(N, H));
	float VoH = saturate(dot(V, H));

	float3 F = F_Schlick(s.f0, VoH);
	float3 specular = D_GGX(NoH, s.alpha) * V_SmithGGXCorrelated(NoV, NoL, s.alpha) * F;
	float3 diffuse = s.diffuseColor * (1.0 - F);
	// Strength already includes pi (see the header), so Lambert is diffuseColor * E * NoL.
	return (diffuse + specular * CHERRY_PI) * E * NoL;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

// ---- Sun shadows -----------------------------------------------------------------------
// 1 lit, 0 in shadow, from one cascade's shadow map: 3x3 bilinear comparisons (soft edges).
float SampleShadowMap(uint mapIndex, float4x4 shadowMatrix, float3 positionW)
{
	const float3 uvz = mul(float4(positionW, 1.0), shadowMatrix).xyz;
	if (any(uvz.xy <= 0.0) || any(uvz.xy >= 1.0) || uvz.z >= 1.0)
		return 1.0;
	Texture2D<float> map = ResourceDescriptorHeap[NonUniformResourceIndex(mapIndex)];
	float2 size;
	map.GetDimensions(size.x, size.y);
	const float2 texel = 1.0 / size;
	float lit = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			lit += map.SampleCmpLevelZero(gShadowSampler, uvz.xy + float2(x, y) * texel, uvz.z);
	return lit / 9.0;
}

// How much sun reaches a point (1 = all). The nearest cascade that covers it is used, pushed
// a little along the normal (less acne on surfaces facing away from the sun), and the last
// cascade fades out towards its end.
float SunShadow(float3 positionW, float3 N)
{
	if (gFrame.shadowEnabled == 0)
		return 1.0;
	const float d = distance(positionW, gView.eyePosW);
	const float3 L = -normalize(gFrame.lights[0].Direction);
	const float push = gFrame.shadowNormalBias * (1.5 - saturate(dot(N, L)));
	if (d < gFrame.shadowSplits.x)
		return SampleShadowMap(gFrame.shadowMap0, gFrame.shadowMatrix[0], positionW + N * gFrame.shadowTexelWorld.x * push);
	if (d < gFrame.shadowSplits.y)
	{
		const float lit = SampleShadowMap(gFrame.shadowMap1, gFrame.shadowMatrix[1], positionW + N * gFrame.shadowTexelWorld.y * push);
		return lerp(lit, 1.0, smoothstep(gFrame.shadowSplits.y - gFrame.shadowSplits.z, gFrame.shadowSplits.y, d));
	}
	return 1.0;
}

// Every light of the frame (directional + point) plus the flat ambient, without fog. The
// sun (light 0) is shadowed when RenderSettings::shadows is on.
float3 ShadeSurface(PbrSurface s, float3 positionW, float3 N, float3 V)
{
	float3 color = 0.0;
	for (uint i = 0; i < min(gFrame.numDirLights, (uint)CHERRY_MAX_LIGHTS); ++i)
	{
		Light light = gFrame.lights[i];
		const float shadow = i == 0 ? SunShadow(positionW, N) : 1.0;
		color += ShadeLight(s, N, V, -light.Direction, light.Strength * shadow);
	}
	for (uint j = 0; j < min(gFrame.numPointLights, (uint)CHERRY_MAX_POINT_LIGHTS); ++j)
	{
		// Smooth falloff that reaches exactly zero at FalloffEnd (the range).
		Light light = gFrame.pointLights[j];
		float3 toLight = light.Position - positionW;
		float d = length(toLight);
		if (d >= light.FalloffEnd)
			continue;
		float falloff = saturate(1.0 - d / light.FalloffEnd);
		color += ShadeLight(s, N, V, toLight / max(d, 1e-4), light.Strength * falloff * falloff);
	}

	// Ambient: a sky/ground hemisphere (brighter from above). Diffuse sees it through the
	// normal; specular reflects it, with a horizon that blurs as the surface gets rougher.
	float3 sky = gFrame.ambientLight.rgb;
	float3 ground = gFrame.ambientGround.rgb;
	float3 irradiance = lerp(ground, sky, saturate(N.y * 0.5 + 0.5));
	float3 R = reflect(-V, N);
	float horizon = 0.03 + s.roughness;
	float3 reflected = lerp(ground, sky, smoothstep(-horizon, horizon, R.y));
	reflected = lerp(reflected, irradiance, s.roughness * s.roughness);
	float NoV = max(dot(N, V), 1e-4);
	color += irradiance * s.diffuseColor + reflected * EnvBRDFApprox(s.f0, s.roughness, NoV);
	return color;
}

// Linear distance fog; returns the colour blended towards the fog colour.
float3 ApplyFog(float3 color, float distance, float3 fogColor, float fogStart, float fogEnd)
{
	if (fogEnd <= fogStart)
		return color;
	float f = saturate((distance - fogStart) / (fogEnd - fogStart));
	return lerp(color, fogColor, f);
}

#endif
