// MeshPass.hlsli
// Forward opaque mesh pass around a surface shader: included after the surface shader's
// EvaluateSurface (see Surface.hlsli). Instance and material data come from bindless
// structured buffers indexed through gFrame; the instance index is a root constant.
#ifndef CHERRY_MESH_PASS_HLSLI
#define CHERRY_MESH_PASS_HLSLI

#include "Surface.hlsli"

ConstantBuffer<MeshDrawConstants> gDraw : register(b0);

struct VertexIn
{
	float3 position : POSITION;
	float3 normal   : NORMAL;
	float2 uv       : TEXCOORD;
	float4 color    : COLOR;
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
};

VertexOut VSMain(VertexIn vin)
{
	StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[gFrame.instanceBufferIndex];
	InstanceData instance = instances[gDraw.instanceIndex];

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
		input.vertexColor = vin.color;
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
	vout.color = vin.color;
	vout.materialIndex = instance.materialIndex;
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
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
	color = ApplyFog(color, distance(gView.eyePosW, input.positionW), gFrame.fogColor, gFrame.fogStart, gFrame.fogEnd);
	return float4(color, s.alpha);
}

#endif
