// Surface.hlsli
// The contract between surface shaders and the mesh pass.
//
// A surface shader is HLSL that defines one function:
//
//     SurfaceOutput EvaluateSurface(SurfaceInput input)
//
// The engine compiles it between this file and MeshPass.hlsli (vertex shader, lighting,
// fog). Optional, defined by the surface shader before MeshPass.hlsli:
//   #define CHERRY_SURFACE_MASKED          discard pixels whose opacityMask is below 1/3
//   #define CHERRY_SURFACE_VERTEX_OFFSET   and define float3 EvaluateVertexOffset(SurfaceInput),
//                                          a world-space offset per vertex (fragment-only
//                                          fields of the input are 0 there) Hand-written surface shaders (.hlsl files in Content) and shader graphs both
// produce such a function. Mesh.hlsl is the built-in one used by plain materials.
#ifndef CHERRY_SURFACE_HLSLI
#define CHERRY_SURFACE_HLSLI

#include "Common.hlsli"
#include "LightingUtil.hlsl"

struct SurfaceInput
{
	float3 positionW;
	float3 normalW;        // interpolated vertex normal, normalized
	float3 viewDirW;       // from the surface towards the camera, normalized
	float2 uv;
	float4 vertexColor;
	float2 screenUV;       // 0..1 across the view, (0, 0) top left
	float  time;           // seconds
	MaterialData material; // the material's fixed fields (see Shared/ShaderInterop.h)
};

struct SurfaceOutput
{
	float3 baseColor;      // linear
	float  metallic;       // 0..1
	float  roughness;      // 0..1 (perceptual)
	float  specular;       // reflectance of the non-metal part: 0.5 = 4% (as in Unreal)
	float3 normalW;        // world-space shading normal (normalized by the pass)
	float3 emissive;       // linear HDR light added after lighting
	float  alpha;          // opacity of translucent surfaces (blended by it), else written to alpha
	float  opacityMask;    // masked surfaces (CHERRY_SURFACE_MASKED): pixels below 1/3 are discarded
};

// ---- Material parameters --------------------------------------------------------
// A material's parameter block: 32-bit values (floats) and texture slots, laid out by
// whoever made the material (MaterialDesc::params / textures). Out-of-range reads give 0
// (and an invalid texture).

uint MaterialParamRaw(MaterialData material, uint index)
{
	if (index >= material.paramCount)
		return 0;
	StructuredBuffer<uint> block = ResourceDescriptorHeap[gFrame.materialParamBufferIndex];
	return block[material.paramOffset + index];
}

float  ParamFloat (SurfaceInput input, uint index) { return asfloat(MaterialParamRaw(input.material, index)); }
float2 ParamFloat2(SurfaceInput input, uint index) { return float2(ParamFloat(input, index), ParamFloat(input, index + 1)); }
float3 ParamFloat3(SurfaceInput input, uint index) { return float3(ParamFloat2(input, index), ParamFloat(input, index + 2)); }
float4 ParamFloat4(SurfaceInput input, uint index) { return float4(ParamFloat3(input, index), ParamFloat(input, index + 3)); }

// Bindless index of texture slot `slot`, or CHERRY_INVALID_INDEX.
uint ParamTextureIndex(SurfaceInput input, uint slot)
{
	if (slot >= input.material.textureCount)
		return CHERRY_INVALID_INDEX;
	StructuredBuffer<uint> block = ResourceDescriptorHeap[gFrame.materialParamBufferIndex];
	return block[input.material.textureOffset + slot];
}

// Samples texture slot `slot`, or returns `fallback` when the material has no texture there.
float4 SampleParamTexture(SurfaceInput input, uint slot, SamplerState samplerState, float2 uv, float4 fallback = float4(1, 1, 1, 1))
{
	uint index = ParamTextureIndex(input, slot);
	if (index == CHERRY_INVALID_INDEX)
		return fallback;
	Texture2D<float4> map = ResourceDescriptorHeap[NonUniformResourceIndex(index)];
	return map.Sample(samplerState, uv);
}

// The same at a given mip level (for the vertex shader, which has no derivatives).
float4 SampleParamTextureLevel(SurfaceInput input, uint slot, SamplerState samplerState, float2 uv, float mip, float4 fallback = float4(1, 1, 1, 1))
{
	uint index = ParamTextureIndex(input, slot);
	if (index == CHERRY_INVALID_INDEX)
		return fallback;
	Texture2D<float4> map = ResourceDescriptorHeap[NonUniformResourceIndex(index)];
	return map.SampleLevel(samplerState, uv, mip);
}

// ---- Defaults -------------------------------------------------------------------

// What a plain material looks like: its base colour times the vertex colour and the
// albedo texture, with its roughness and metallic. Surface shaders can start from this.
SurfaceOutput DefaultSurfaceOutput(SurfaceInput input)
{
	MaterialData material = input.material;
	float4 albedo = material.baseColor * input.vertexColor;
	if (material.albedoTextureIndex != CHERRY_INVALID_INDEX)
	{
		Texture2D<float4> albedoMap = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoTextureIndex)];
		albedo *= albedoMap.Sample(gAnisoWrap, input.uv);
	}

	SurfaceOutput s;
	s.baseColor = albedo.rgb;
	s.metallic = material.metallic;
	s.roughness = material.roughness;
	s.specular = material.fresnelR0.x / 0.08;
	s.normalW = input.normalW;
	s.emissive = 0.0;
	s.alpha = albedo.a;
	s.opacityMask = 1.0;
	return s;
}

#endif
