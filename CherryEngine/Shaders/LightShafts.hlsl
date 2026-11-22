// LightShafts.hlsl
// Sunlight scattered by the air (RenderSettings::lightShafts), at half the view's size.
//   CSMain: marches each pixel's view ray from the eye to what it hits (or maxDistance) and
//           adds the sun's light wherever it reaches the air: through the sun's shadow maps,
//           the clouds, and inside the interior only where the light volume lets the sun in
//           (as ShadeSurface does for surfaces). Trees and rims cut dark gaps into the beams;
//           a natural shaft in the cave shows as a column of light.
//   CSBlur: smooths it (depth-aware), hiding the per-pixel jitter of the march.
// The tonemap adds the result to the world (Tonemap.hlsl).
#include "LightingUtil.hlsl"

ConstantBuffer<LightShaftConstants> gDraw : register(b0);

// One bilinear shadow-map comparison (cheaper than SunShadow's 3x3: the march averages).
float ShaftShadow(float3 p, float t)
{
	if (gFrame.shadowEnabled == 0)
		return 1.0;
	uint map;
	float4x4 m;
	if (t < gFrame.shadowSplits.x) { map = gFrame.shadowMap0; m = gFrame.shadowMatrix[0]; }
	else if (t < gFrame.shadowSplits.y) { map = gFrame.shadowMap1; m = gFrame.shadowMatrix[1]; }
	else return 1.0;
	const float3 uvz = mul(float4(p, 1.0), m).xyz;
	if (any(uvz.xy <= 0.0) || any(uvz.xy >= 1.0) || uvz.z >= 1.0)
		return 1.0;
	Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(map)];
	return shadowMap.SampleCmpLevelZero(gShadowSampler, uvz.xy, uvz.z);
}

// Henyey-Greenstein, scaled so that light scattered evenly gives 1.
float Phase(float cosAngle, float g)
{
	const float g2 = g * g;
	return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosAngle, 1e-4), 1.5);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
		return;
	RWTexture2D<float4> dest = ResourceDescriptorHeap[gDraw.destIndex];
	Texture2D<float> depthMap = ResourceDescriptorHeap[gDraw.depthIndex];

	const float2 fullSize = gView.renderTargetSize;
	const uint2 fullPixel = min(id.xy * 2, uint2(fullSize) - 1);
	const float depth = depthMap[fullPixel];
	const float2 uv = (fullPixel + 0.5) / fullSize;
	const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, min(depth, 0.999999), 1.0);
	const float4 h = mul(ndc, gView.invViewProj);
	const float3 eye = gView.eyePosW;
	const float3 toPoint = h.xyz / h.w - eye;
	const float3 dir = normalize(toPoint);
	const float hitDistance = depth >= 1.0 ? gDraw.maxDistance : min(length(toPoint), gDraw.maxDistance);

	const float3 toSun = -normalize(gFrame.lights[0].Direction);
	const float3 sun = gFrame.lights[0].Strength;
	if (gFrame.numDirLights == 0 || dot(sun, sun) <= 0.0)
	{
		dest[id.xy] = 0.0;
		return;
	}

	// Steps grow with distance (more of them near the eye, where beams are sharp), jittered
	// per pixel (interleaved gradient noise) so the bands become noise the blur removes.
	const uint steps = max(gDraw.steps, 4u);
	const float jitter = frac(52.9829189 * frac(dot(float2(id.xy) + 0.5, float2(0.06711056, 0.00583715))));
	float scattered = 0.0;
	float opticalDepth = 0.0;
	float previous = 0.0;
	for (uint i = 0; i < steps; ++i)
	{
		const float s = (i + jitter) / steps;
		const float t = hitDistance * s * s;
		const float dt = t - previous;
		previous = t;
		const float3 p = eye + dir * t;

		const float interior = InteriorAmount(p);
		float reach = 1.0 - interior;
		if (interior > 0.0)
			reach += interior * saturate(InteriorLight(p, float3(0, 0, 0)) * gFrame.interiorSunFromLight);
		// Outside, the air thins with distance: beams near the eye, and the far valley is left
		// to the fog (it would only be an even veil out there, past the shadow maps).
		const float far = 1.0 - smoothstep(0.25, 1.0, t / gDraw.maxDistance);
		const float density = lerp(gDraw.density * far, gDraw.interiorDensity, interior);
		opticalDepth += density * dt;
		if (reach <= 0.0)
			continue;
		const float lit = ShaftShadow(p, t) * CloudShadow(p) * reach;
		scattered += lit * density * dt * exp(-opticalDepth);
	}

	const float phase = lerp(0.1, Phase(dot(dir, toSun), gDraw.anisotropy), 0.9);   // mostly towards the sun: a light haze elsewhere
	dest[id.xy] = float4(sun * scattered * phase * gDraw.intensity, 1.0);
}

float ViewZ(float depth)
{
	return gView.proj[3][2] / (depth - gView.proj[2][2]);
}

[numthreads(8, 8, 1)]
void CSBlur(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= gDraw.width || id.y >= gDraw.height)
		return;
	Texture2D<float4> source = ResourceDescriptorHeap[gDraw.sourceIndex];
	Texture2D<float> depthMap = ResourceDescriptorHeap[gDraw.depthIndex];
	RWTexture2D<float4> dest = ResourceDescriptorHeap[gDraw.destIndex];
	const uint2 fullMax = uint2(gView.renderTargetSize) - 1;
	const float z = ViewZ(min(depthMap[min(id.xy * 2, fullMax)], 0.999999));
	float3 sum = 0.0;
	float weight = 0.0;
	[unroll] for (int y = -2; y <= 2; ++y)
		[unroll] for (int x = -2; x <= 2; ++x)
		{
			const int2 at = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(gDraw.width - 1, gDraw.height - 1));
			const float d = ViewZ(min(depthMap[min(uint2(at) * 2, fullMax)], 0.999999));
			const float w = exp(-abs(d - z) / (0.1 * z + 0.1)) * (1.0 / (1.0 + x * x + y * y));
			sum += source[at].rgb * w;
			weight += w;
		}
	dest[id.xy] = float4(sum / max(weight, 1e-5), 1.0);
}
