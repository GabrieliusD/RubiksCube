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

// The sun reaching a point through one set of cascade maps.
float SunShadowFrom(uint map0, uint map1, float4x4 matrix0, float4x4 matrix1, float3 positionW, float3 N)
{
	const float d = distance(positionW, gView.eyePosW);
	const float3 L = -normalize(gFrame.lights[0].Direction);
	const float push = gFrame.shadowNormalBias * (1.5 - saturate(dot(N, L)));
	if (d < gFrame.shadowSplits.x)
		return SampleShadowMap(map0, matrix0, positionW + N * gFrame.shadowTexelWorld.x * push);
	if (d < gFrame.shadowSplits.y)
	{
		const float lit = SampleShadowMap(map1, matrix1, positionW + N * gFrame.shadowTexelWorld.y * push);
		return lerp(lit, 1.0, smoothstep(gFrame.shadowSplits.y - gFrame.shadowSplits.z, gFrame.shadowSplits.y, d));
	}
	return 1.0;
}

// How much sun reaches a point (1 = all). The nearest cascade that covers it is used, pushed
// a little along the normal (less acne on surfaces facing away from the sun), and the last
// cascade fades out towards its end. Just after the shadows step, the previous ones fade out.
float SunShadow(float3 positionW, float3 N)
{
	if (gFrame.shadowEnabled == 0)
		return 1.0;
	float lit = SunShadowFrom(gFrame.shadowMap0, gFrame.shadowMap1, gFrame.shadowMatrix[0], gFrame.shadowMatrix[1], positionW, N);
	if (gFrame.shadowFade > 0.0)
	{
		const float before = SunShadowFrom(gFrame.shadowMapPrevious0, gFrame.shadowMapPrevious1, gFrame.shadowMatrixPrevious[0], gFrame.shadowMatrixPrevious[1], positionW, N);
		lit = lerp(lit, before, gFrame.shadowFade);
	}
	return lit;
}

// ---- Interior (RenderSettings::interior) ------------------------------------------------
// 0 outside the interior box, rising to 1 past its fade-in from the opening (the min-z face).
float InteriorAmount(float3 p)
{
	if (gFrame.interiorEnabled == 0)
		return 0.0;
	if (any(p < gFrame.interiorMin) || any(p > gFrame.interiorMax))
		return 0.0;
	return saturate((p.z - gFrame.interiorMin.z - gFrame.interiorFadeStart) / gFrame.interiorFadeLength);
}

// Length of the segment a..b inside the interior box (from its fade start on).
float InteriorRayLength(float3 a, float3 b)
{
	if (gFrame.interiorEnabled == 0)
		return 0.0;
	const float3 boxMin = gFrame.interiorMin + float3(0.0, 0.0, gFrame.interiorFadeStart);
	const float3 d = b - a;
	const float3 invD = 1.0 / select(abs(d) < 1e-6, 1e-6, d);
	const float3 t0 = (boxMin - a) * invD;
	const float3 t1 = (gFrame.interiorMax - a) * invD;
	const float3 tNear = min(t0, t1), tFar = max(t0, t1);
	const float enter = max(max(max(tNear.x, tNear.y), tNear.z), 0.0);
	const float exit = min(min(min(tFar.x, tFar.y), tFar.z), 1.0);
	return max(exit - enter, 0.0) * length(d);
}

// How much outside light reaches this point of the interior (its light volume), 0..1:
// sampled half a cell out from the surface, so walls take the level of the air in front.
float InteriorLight(float3 positionW, float3 N)
{
	if (gFrame.interiorLightVolume == CHERRY_INVALID_INDEX)
		return 0.0;
	Texture3D<float> volume = ResourceDescriptorHeap[gFrame.interiorLightVolume];
	float3 dims;
	volume.GetDimensions(dims.x, dims.y, dims.z);
	const float3 extent = gFrame.interiorMax - gFrame.interiorMin;
	const float3 uvw = (positionW + N * (0.5 * extent.x / dims.x) - gFrame.interiorMin) / extent;
	return volume.SampleLevel(gLinearClamp, uvw, 0);
}

// ---- Rooms (RenderSettings::rooms) --------------------------------------------------------
// 0 outside every room, 1 well inside one; `ambientScale` gets that room's sky ambient.
float RoomAmount(float3 positionW, out float ambientScale)
{
	float best = 0.0;
	ambientScale = 1.0;
	for (uint i = 0; i < min(gFrame.roomCount, (uint)CHERRY_MAX_ROOMS); ++i)
	{
		const RoomBox room = gFrame.rooms[i];
		const float3 d = positionW - room.center;
		const float3 local = float3(d.x * room.axes.x + d.z * room.axes.y, d.y, d.x * room.axes.z + d.z * room.axes.w);
		const float3 inside = room.halfExtents - abs(local);
		const float amount = saturate(min(min(inside.x, inside.y), inside.z) / room.fade);
		if (amount > best)
		{
			best = amount;
			ambientScale = room.ambientScale;
		}
	}
	return best;
}

// ---- Cloud shadows (RenderSettings::clouds) ----------------------------------------------
// A value per grid corner, from its integer coordinates: integer maths gives every cell the
// same value for a shared corner (a float hash can differ by rounding between the cells,
// which shows as straight seams).
float CloudHash(int2 c)
{
	uint h = asuint(c.x) * 0x8DA6B343u ^ asuint(c.y) * 0xD8163841u;
	h ^= h >> 13;
	h *= 0x5BD1E995u;
	h ^= h >> 15;
	return (h & 0xFFFFFFu) / 16777216.0;
}

float CloudValueNoise(float2 p)
{
	const float2 cell = floor(p);
	const int2 i = int2(cell);
	const float2 f = p - cell;
	const float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(CloudHash(i), CloudHash(i + int2(1, 0)), u.x), lerp(CloudHash(i + int2(0, 1)), CloudHash(i + int2(1, 1)), u.x), u.y);
}

// Cloud density 0..1 at a point of the cloud layer (world x, z).
float CloudDensity(float2 xz)
{
	float2 p = (xz + gFrame.cloudOffset) / gFrame.cloudScale;
	const float n = CloudValueNoise(p) * 0.55 + CloudValueNoise(p * 2.03 + 17.1) * 0.3 + CloudValueNoise(p * 4.11 + 41.7) * 0.15;
	const float edge = 1.0 - gFrame.cloudCoverage;
	const float density = smoothstep(edge - gFrame.cloudSoftness, edge + gFrame.cloudSoftness, n);
	// Passing too fast to see: their average (about the coverage) everywhere.
	return lerp(density, gFrame.cloudCoverage, gFrame.cloudBlur);
}

// How much sun gets past the clouds to a point (1 = all).
float CloudShadow(float3 positionW)
{
	if (gFrame.cloudsEnabled == 0)
		return 1.0;
	const float3 toSun = -normalize(gFrame.lights[0].Direction);
	const float2 at = positionW.xz + toSun.xz * ((gFrame.cloudHeight - positionW.y) / max(toSun.y, 0.08));
	return 1.0 - gFrame.cloudDarkness * CloudDensity(at);
}

// Every light of the frame (directional + point) plus the flat ambient, without fog. The
// sun (light 0) is shadowed when RenderSettings::shadows is on, and by clouds. Inside the
// interior the directional lights fade out, except where its light volume lets them in,
// and its ambient (plus the outside light that reaches in) takes over.
float3 ShadeSurface(PbrSurface s, float3 positionW, float3 N, float3 V)
{
	float3 color = 0.0;
	const float interior = InteriorAmount(positionW);
	const float inLight = interior > 0.0 ? InteriorLight(positionW, N) : 0.0;
	const float reach = (1.0 - interior) + interior * saturate(inLight * gFrame.interiorSunFromLight);
	float roomAmbient = 1.0;
	const float room = gFrame.roomCount > 0 ? RoomAmount(positionW, roomAmbient) : 0.0;
	for (uint i = 0; i < min(gFrame.numDirLights, (uint)CHERRY_MAX_LIGHTS); ++i)
	{
		if (reach <= 0.0)
			break;
		Light light = gFrame.lights[i];
		// The sun enters rooms where its shadows let it; the fill lights don't.
		const float shadow = i == 0 ? SunShadow(positionW, N) * CloudShadow(positionW) : 1.0 - room;
		color += ShadeLight(s, N, V, -light.Direction, light.Strength * shadow * reach);
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
	const float outsideShare = inLight * gFrame.interiorLightStrength;
	const float roomScale = lerp(1.0, roomAmbient, room);
	float3 sky = lerp(gFrame.ambientLight.rgb * roomScale, gFrame.interiorAmbientSky + gFrame.ambientLight.rgb * outsideShare, interior);
	float3 ground = lerp(gFrame.ambientGround.rgb * roomScale, gFrame.interiorAmbientGround + gFrame.ambientGround.rgb * outsideShare, interior);
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

// The frame's fog between the eye and a surface: the world's fog over the part of the view
// ray outside the interior, then the interior's over the part inside.
float3 ApplySceneFog(float3 color, float3 eye, float3 positionW)
{
	const float total = distance(eye, positionW);
	const float inside = InteriorRayLength(eye, positionW);
	color = ApplyFog(color, total - inside, gFrame.fogColor, gFrame.fogStart, gFrame.fogEnd);
	return ApplyFog(color, inside, gFrame.interiorFogColor, gFrame.interiorFogStart, gFrame.interiorFogEnd);
}

#endif
