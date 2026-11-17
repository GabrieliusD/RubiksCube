// Forward opaque mesh pass. Instance and material data come from bindless
// structured buffers indexed through gFrame; the instance index is a root constant.
#include "Common.hlsli"
#include "LightingUtil.hlsl"

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
	vout.positionW = positionW.xyz;
	vout.positionH = mul(positionW, gView.viewProj);
	vout.normalW = mul(vin.normal, (float3x3)instance.normalMatrix);
	vout.uv = vin.uv;
	vout.color = vin.color;
	vout.materialIndex = instance.materialIndex;
	return vout;
}

float4 PSMain(VertexOut pin) : SV_Target
{
	StructuredBuffer<MaterialData> materials = ResourceDescriptorHeap[gFrame.materialBufferIndex];
	MaterialData material = materials[pin.materialIndex];

	// Vertex colour tints the material (white by default), so chunked meshes can carry per-cell colour.
	float4 albedo = material.baseColor * pin.color;
	if (material.albedoTextureIndex != CHERRY_INVALID_INDEX)
	{
		Texture2D<float4> albedoMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoTextureIndex)];
		albedo *= albedoMap.Sample(gAnisoWrap, pin.uv);
	}

	float3 normal = normalize(pin.normalW);
	float3 toEye = normalize(gView.eyePosW - pin.positionW);

	Material mat;
	mat.DiffuseAlbedo = albedo;
	mat.FresnelR0 = material.fresnelR0;
	mat.Shininess = 1.0 - material.roughness;

	float3 color = gFrame.ambientLight.rgb * albedo.rgb
		+ ComputeLighting(gFrame.lights, gFrame.numDirLights, mat, normal, toEye)
		+ ComputePointLights(gFrame.pointLights, gFrame.numPointLights, mat, pin.positionW, normal, toEye);
	color = ApplyFog(color, distance(gView.eyePosW, pin.positionW), gFrame.fogColor, gFrame.fogStart, gFrame.fogEnd);
	return float4(color, albedo.a);
}
