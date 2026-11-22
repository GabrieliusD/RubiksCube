// SkyPass.hlsl
// The sky (RenderSettings::sky): a full-screen triangle at the far plane, depth-tested so it
// only covers pixels no mesh or voxel was drawn on. A gradient from the ground colour through
// the horizon to the zenith, a sun disk with a soft glow, clouds, and stars at night.
#include "Common.hlsli"
#include "LightingUtil.hlsl"   // the clouds (CloudDensity)

struct VertexOut
{
	float4 position : SV_POSITION;
};

VertexOut VSMain(uint vertexId : SV_VertexID)
{
	VertexOut vout;
	const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	vout.position = float4(uv * float2(2, -2) + float2(-1, 1), 1, 1);   // z = 1: the far plane
	return vout;
}

float Hash13(float3 p)
{
	p = frac(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return frac((p.x + p.y) * p.z);
}

// Stars: one candidate per cell of a grid on the cube around the camera.
float Stars(float3 dir)
{
	const float3 a = abs(dir);
	float2 face;
	float faceId;
	if (a.x >= a.y && a.x >= a.z) { face = dir.yz / a.x; faceId = dir.x > 0 ? 0 : 1; }
	else if (a.y >= a.z)          { face = dir.xz / a.y; faceId = dir.y > 0 ? 2 : 3; }
	else                          { face = dir.xy / a.z; faceId = dir.z > 0 ? 4 : 5; }
	const float cells = 220.0;
	const float2 grid = face * cells;
	const float2 cell = floor(grid);
	const float3 seed = float3(cell, faceId);
	const float h = Hash13(seed);
	if (h < 0.965) return 0.0;   // most cells have no star
	const float2 at = float2(Hash13(seed + 17.0), Hash13(seed + 43.0)) * 0.7 + 0.15;
	const float d = length(frac(grid) - at);
	const float brightness = pow(Hash13(seed + 71.0), 3.0) * 3.0 + 0.2;
	const float twinkle = 0.75 + 0.25 * sin(gFrame.time * (1.5 + 3.0 * Hash13(seed + 5.0)) + h * 40.0);
	return brightness * twinkle * saturate(1.0 - d * 6.0);
}

float4 PSMain(VertexOut pin) : SV_Target
{
	float3 origin, dir;
	PixelRay(pin.position.xy, origin, dir);

	// Gradient: ground below the horizon, horizon to zenith above it.
	const float up = dir.y;
	float3 color;
	if (up >= 0.0)
		color = lerp(gFrame.skyHorizon, gFrame.skyZenith, pow(saturate(up), max(gFrame.skyGradientPower, 0.05)));
	else
		color = lerp(gFrame.skyHorizon, gFrame.skyGround, saturate(-up * 6.0));

	// Sun: lights[0].Direction is the way its light travels, so the sun is the other way.
	const float3 toSun = -normalize(gFrame.lights[0].Direction);
	const float cosAngle = dot(dir, toSun);
	if (gFrame.sunCosRadius < 1.0)
	{
		const float edge = (1.0 - gFrame.sunCosRadius) * 0.15;
		color += gFrame.sunDiskColor * smoothstep(gFrame.sunCosRadius - edge, gFrame.sunCosRadius + edge, cosAngle);
	}
	// A soft glow around the sun, from its light colour.
	color += gFrame.lights[0].Strength * (pow(saturate(cosAngle), 64.0) * 0.25 + pow(saturate(cosAngle), 8.0) * 0.05);

	// Clouds: the cloud layer (RenderSettings::clouds) where the view ray meets it, lit by
	// the sun's colour and the sky, fading towards the horizon.
	if (gFrame.cloudsEnabled != 0 && up > 0.02)
	{
		const float t = (gFrame.cloudHeight - origin.y) / up;
		if (t > 0.0)
		{
			const float density = CloudDensity(origin.xz + dir.xz * t);
			const float3 lit = gFrame.lights[0].Strength * 0.55 + gFrame.skyZenith * 0.6 + gFrame.skyHorizon * 0.5;
			const float fade = saturate(up * 5.0);
			color = lerp(color, lit, density * 0.85 * fade);
		}
	}

	// Stars, above the horizon only.
	if (gFrame.starIntensity > 0.0 && up > 0.0)
		color += gFrame.starIntensity * Stars(dir) * saturate(up * 8.0) * float3(0.9, 0.95, 1.0);

	return float4(color, 1.0);
}
