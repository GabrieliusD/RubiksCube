// MeshPass.hlsli
// Forward opaque mesh pass around a surface shader: included after the surface shader's
// EvaluateSurface (see Surface.hlsli). Instance and material data come from bindless
// structured buffers indexed through gFrame; the first instance index is a root constant
// and SV_InstanceID counts from it.
#ifndef CHERRY_MESH_PASS_HLSLI
#define CHERRY_MESH_PASS_HLSLI

#include "Surface.hlsli"

CHERRY_PUSH_CONSTANTS ConstantBuffer<MeshDrawConstants> gDraw : register(b0);

struct VertexIn
{
	CHERRY_LOCATION(0) float3 position : POSITION;
	CHERRY_LOCATION(1) float3 normal   : NORMAL;
	CHERRY_LOCATION(2) float2 uv       : TEXCOORD;
	CHERRY_LOCATION(3) float4 color    : COLOR;
};

struct VertexOut
{
	float4 positionH : SV_POSITION;
	float3 positionW : POSITION;
	float3 normalW   : NORMAL;
	float3 positionL : LOCAL;
	float2 uv        : TEXCOORD;
	float4 color     : COLOR;
	nointerpolation uint materialIndex : MATERIAL;
	nointerpolation float2 dither : DITHER;   // InstanceData::ditherCut, ditherInvert
};

// Screen-door fading (MeshRenderer::dither): a 4 x 4 ordered pattern over the pixels.
bool DitheredOut(float2 pixel, float2 dither)
{
	if (dither.x <= 0.0 && dither.y < 0.5) return false;
	static const float kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
	const uint2 p = uint2(pixel) & 3;
	const float n = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
	// Drawn: n below the share drawn (1 - cut); inverted, the rest (n at or above the cut).
	return dither.y > 0.5 ? n < dither.x : n >= 1.0 - dither.x;
}

// Instanced: a draw covers `instanceCount` consecutive entries starting at gDraw.instanceIndex.
VertexOut VSMain(VertexIn vin, uint instanceId : SV_InstanceID)
{
	StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[gFrame.instanceBufferIndex];
	InstanceData instance = instances[gDraw.instanceIndex + instanceId];

	VertexOut vout;
	float4 positionW = mul(float4(vin.position, 1.0), instance.world);
	vout.normalW = mul(vin.normal, (float3x3)instance.normalMatrix);
#ifdef CHERRY_SURFACE_VERTEX_OFFSET
	{
		StructuredBuffer<MaterialData> materials = ResourceDescriptorHeap[gFrame.materialBufferIndex];
		SurfaceInput input;
		input.positionW = positionW.xyz;
		input.normalW = normalize(vout.normalW);
		input.viewDirW = normalize(gView.eyePosW - positionW.xyz);
		input.uv = vin.uv;
		input.vertexColor = vin.color * instance.tint;
		input.screenUV = 0.0;
		input.time = gFrame.time;
		input.material = materials[instance.materialIndex];
		input.positionL = vin.position;
		input.blockId = 0.0;
		positionW.xyz += EvaluateVertexOffset(input);
	}
#endif
	vout.positionW = positionW.xyz;
	vout.positionH = mul(positionW, gView.viewProj);
	vout.positionL = vin.position;
	vout.uv = vin.uv;
	vout.color = vin.color * instance.tint;
	vout.materialIndex = instance.materialIndex;
	vout.dither = float2(instance.ditherCut, (float)instance.ditherInvert);
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	if (DitheredOut(pin.positionH.xy, pin.dither)) discard;
	StructuredBuffer<MaterialData> materials = ResourceDescriptorHeap[gFrame.materialBufferIndex];

	SurfaceInput input;
	input.positionW = pin.positionW;
	input.normalW = normalize(pin.normalW);
	input.viewDirW = normalize(gView.eyePosW - pin.positionW);
	input.uv = pin.uv;
	input.vertexColor = pin.color;
	input.screenUV = pin.positionH.xy * gView.invRenderTargetSize;
	input.time = gFrame.time;
	input.material = materials[pin.materialIndex];
	input.positionL = pin.positionL;
	input.blockId = 0.0;

	SurfaceOutput s = EvaluateSurface(input);
#ifdef CHERRY_SURFACE_MASKED
	clip(s.opacityMask - 1.0 / 3.0);
#endif

	float3 normal = normalize(s.normalW);
	PbrSurface surface = MakePbrSurface(s.baseColor, s.metallic, s.roughness, (0.08 * saturate(s.specular)).xxx);
	float3 color = ShadeSurface(surface, input.positionW, normal, input.viewDirW) + s.emissive;
	color = ApplySceneFog(color, gView.eyePosW, input.positionW);
	return float4(color, s.alpha);
}

#endif
