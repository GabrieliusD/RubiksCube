// Particles.hlsl
// Particle billboards (Renderer::Particles): six vertices per particle from SV_VertexID,
// facing the camera. Lit in the vertex shader like a small cloud of dust at its centre:
// the sun (with its shadows and the clouds, so dust only shines where sunlight falls), the
// fill lights and the ambient (with the cave interior and rooms), and the point lights.
// Round puffs fade softly to their edge; square ones are blocks.
#include "Common.hlsli"
#include "LightingUtil.hlsl"

ConstantBuffer<MeshDrawConstants> gDraw : register(b0);   // instanceIndex: the first particle

struct VertexOut
{
	float4 position : SV_POSITION;
	float2 corner : TEXCOORD0;      // -1..1 across the quad
	float4 color : COLOR;           // lit colour, alpha = opacity
	nointerpolation uint square : SQUARE;
};

float3 LightAt(float3 p)
{
	// Like ShadeSurface for a surface that faces every way at once.
	const float interior = InteriorAmount(p);
	const float inLight = interior > 0.0 ? InteriorLight(p, float3(0, 0, 0)) : 0.0;
	const float reach = (1.0 - interior) + interior * saturate(inLight * gFrame.interiorSunFromLight);
	float roomAmbient = 1.0;
	const float room = gFrame.roomCount > 0 ? RoomAmount(p, roomAmbient) : 0.0;

	float3 light = 0.0;
	for (uint i = 0; i < min(gFrame.numDirLights, (uint)CHERRY_MAX_LIGHTS); ++i)
	{
		const float shadow = i == 0 ? SunShadow(p, float3(0, 0, 0)) * CloudShadow(p) : (1.0 - room) * 0.5;
		light += gFrame.lights[i].Strength * shadow * reach * 0.6;
	}
	for (uint j = 0; j < min(gFrame.numPointLights, (uint)CHERRY_MAX_POINT_LIGHTS); ++j)
	{
		const Light pl = gFrame.pointLights[j];
		const float d = distance(pl.Position, p);
		if (d >= pl.FalloffEnd) continue;
		const float falloff = saturate(1.0 - d / pl.FalloffEnd);
		light += pl.Strength * falloff * falloff * 0.6;
	}
	const float roomScale = lerp(1.0, roomAmbient, room);
	const float3 outsideAmbient = (gFrame.ambientLight.rgb + gFrame.ambientGround.rgb) * 0.5;
	const float3 insideAmbient = (gFrame.interiorAmbientSky + gFrame.interiorAmbientGround) * 0.5 + outsideAmbient * inLight * gFrame.interiorLightStrength;
	light += lerp(outsideAmbient * roomScale, insideAmbient, interior);
	return light;
}

VertexOut VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	StructuredBuffer<ParticleData> particles = ResourceDescriptorHeap[gFrame.particleBufferIndex];
	const ParticleData particle = particles[gDraw.instanceIndex + instanceId];

	static const float2 kCorners[6] = { float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1) };
	const float2 corner = kCorners[vertexId];
	const float c = cos(particle.rotation), s = sin(particle.rotation);
	const float2 turned = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
	const float3 right = gView.invView[0].xyz;
	const float3 up = gView.invView[1].xyz;
	const float3 positionW = particle.position + (right * turned.x + up * turned.y) * (particle.size * 0.5);

	VertexOut vout;
	vout.position = mul(float4(positionW, 1.0), gView.viewProj);
	vout.corner = corner;
	const float3 lit = particle.color.rgb * (LightAt(particle.position) + particle.emissive);
	vout.color = float4(ApplySceneFog(lit, gView.eyePosW, particle.position), particle.color.a);
	vout.square = particle.flags & 1u;
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	float alpha = pin.color.a;
	if (pin.square == 0)
	{
		const float r = length(pin.corner);
		if (r >= 1.0) discard;
		alpha *= (1.0 - r) * (1.0 - r) * 2.0;   // a soft puff
	}
	return float4(pin.color.rgb, saturate(alpha));
}
