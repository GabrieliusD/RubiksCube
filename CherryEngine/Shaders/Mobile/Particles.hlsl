// Billboard particles (Renderer::Particles): a quad per particle facing the camera, lit
// roughly like the scene at its centre (sun + sky ambient) plus its emissive. Covering ones
// blend over (sorted back to front on the CPU), additive ones add.
#include "Common.hlsli"

struct VertexOut
{
	float4 positionH : SV_Position;
	[[vk::location(0)]] float4 color : COLOR0;
	[[vk::location(1)]] float3 local : TEXCOORD0;   // xy: -1..1 across the quad; z: square
};

VertexOut VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	const ParticleGpu p = gParticles[gDraw.firstInstance + instanceId];
	const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	const float2 corner = kCorners[vertexId];
	const float s = sin(p.params.y), c = cos(p.params.y);
	const float2 turned = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
	const float halfSize = 0.5 * p.positionSize.w;
	const float3 positionW = p.positionSize.xyz + (gFrame.cameraRight.xyz * turned.x + gFrame.cameraUp.xyz * turned.y) * halfSize;
	VertexOut vout;
	vout.positionH = mul(float4(positionW, 1.0), gFrame.viewProj);
	const float3 light = gFrame.ambientSky.rgb + gFrame.sunStrength.rgb * saturate(-normalize(gFrame.sunDirection.xyz).y);
	vout.color = float4(p.color.rgb * (light + p.params.x), p.color.a);
	vout.local = float3(corner, p.params.z);
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	float coverage = 1.0;
	if (pin.local.z < 0.5)
		coverage = saturate(1.0 - dot(pin.local.xy, pin.local.xy));   // a soft round puff
	return float4(pin.color.rgb, pin.color.a * coverage);
}
