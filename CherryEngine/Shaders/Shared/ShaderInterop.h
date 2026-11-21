// ShaderInterop.h
// Shared between C++ and HLSL. Any struct here is used on both sides with the
// same memory layout, so keep members 4-byte aligned and respect HLSL cbuffer
// packing (a float3 followed by a float fills one 16-byte register).
#ifndef CHERRY_SHADER_INTEROP_H
#define CHERRY_SHADER_INTEROP_H

#ifdef __cplusplus
	#include <DirectXMath.h>
	#include <cstdint>
	namespace gpu
	{
		using uint = uint32_t;
		using float2 = DirectX::XMFLOAT2;
		using float3 = DirectX::XMFLOAT3;
		using float4 = DirectX::XMFLOAT4;
		using uint3 = DirectX::XMUINT3;
		using int3 = DirectX::XMINT3;
		using float4x4 = DirectX::XMFLOAT4X4;
	#define CHERRY_MATRIX float4x4
#else
	#define CHERRY_MATRIX row_major float4x4
#endif

#define CHERRY_MAX_LIGHTS 4
#define CHERRY_MAX_POINT_LIGHTS 8
#define CHERRY_SHADER_GLOBALS 4
#define CHERRY_SHADOW_CASCADES 2
#define CHERRY_INVALID_INDEX 0xFFFFFFFF

struct Light
{
	float3 Strength;
	float  FalloffStart;
	float3 Direction;
	float  FalloffEnd;
	float3 Position;
	float  SpotPower;
};

// Root CBV b2: data shared by every view in a frame.
struct FrameConstants
{
	float time;
	float deltaTime;
	uint  instanceBufferIndex;     // StructuredBuffer<InstanceData>
	uint  materialBufferIndex;     // StructuredBuffer<MaterialData>

	uint  voxelVolumeBufferIndex;  // StructuredBuffer<VoxelVolumeData>
	uint  debugVertexBufferIndex;  // StructuredBuffer<DebugVertex>
	uint  frameNumber;
	uint  numDirLights;

	float4 ambientLight;           // from above
	float4 ambientGround;          // from below
	Light lights[CHERRY_MAX_LIGHTS];

	// Distance fog (linear, view distance). Disabled when fogEnd <= fogStart.
	float3 fogColor;
	float  fogStart;
	float  fogEnd;
	uint   numPointLights;
	uint   materialParamBufferIndex;  // StructuredBuffer<uint>: every material's parameter block
	float  pad1;
	// Point lights: Position, Strength (colour x intensity), FalloffEnd (range).
	Light pointLights[CHERRY_MAX_POINT_LIGHTS];

	// RenderSettings::shaderGlobals: values the game sets for every surface shader
	// (ShaderGlobal(i) in Surface.hlsli, the Global Input graph node).
	float4 globals[CHERRY_SHADER_GLOBALS];

	// The sky (RenderSettings::sky), drawn where nothing else is (SkyPass.hlsl).
	float3 skyZenith;              // linear HDR, straight up
	uint   skyEnabled;
	float3 skyHorizon;             // at the horizon
	float  sunCosRadius;           // cos of the sun disk's angular radius (1 = no disk)
	float3 skyGround;              // below the horizon
	float  starIntensity;
	float3 sunDiskColor;           // sun colour x disk brightness
	float  skyGradientPower;       // how fast the horizon colour gives way to the zenith colour

	// Sun shadows (RenderSettings::shadows): CHERRY_SHADOW_CASCADES shadow maps around the
	// camera, the nearest first. shadowMatrix maps world space to (u, v, depth) of its map.
	CHERRY_MATRIX shadowMatrix[CHERRY_SHADOW_CASCADES];
	float4 shadowSplits;           // view distance where each cascade ends (x, y); z: fade length of the last
	float4 shadowTexelWorld;       // world size of one shadow texel in each cascade (x, y)
	uint   shadowMap0;             // Texture2D<float> (depth) of cascade 0
	uint   shadowMap1;             // ... and cascade 1 (not an array: cbuffer arrays pad each element to 16 bytes)
	uint   shadowEnabled;
	float  shadowNormalBias;       // texels of push along the normal before the lookup
};

// Root CBV b1: one camera / view.
struct ViewConstants
{
	CHERRY_MATRIX view;
	CHERRY_MATRIX invView;
	CHERRY_MATRIX proj;
	CHERRY_MATRIX invProj;
	CHERRY_MATRIX viewProj;
	CHERRY_MATRIX invViewProj;

	float3 eyePosW;
	float  nearZ;

	float2 renderTargetSize;
	float2 invRenderTargetSize;

	float  farZ;
	float  exposure;
	float  pad0;
	float  pad1;
};

struct InstanceData
{
	CHERRY_MATRIX world;
	CHERRY_MATRIX normalMatrix;    // inverse-transpose of world (upper 3x3 used)
	uint materialIndex;
	uint flags;
	uint pad0;
	uint pad1;
};

// One material. Surface shaders read the fixed fields directly and their own parameters
// from the material's block in FrameConstants::materialParamBufferIndex: `paramCount`
// 32-bit values starting at `paramOffset`, then `textureCount` bindless texture indices
// starting at `textureOffset` (see Shaders/Surface.hlsli).
struct MaterialData
{
	float4 baseColor;              // linear
	float3 fresnelR0;              // reflectance of the non-metal part at normal incidence (0.04 typical)
	float  roughness;              // perceptual, 0..1
	uint   albedoTextureIndex;     // CHERRY_INVALID_INDEX = untextured
	float  metallic;               // 0 = dielectric, 1 = metal tinted by baseColor
	uint   paramOffset;
	uint   paramCount;
	uint   textureOffset;
	uint   textureCount;
	uint   pad0;
	uint   pad1;
};

// A ray-marched voxel volume: a dims.x*dims.y*dims.z grid of palette indices
// (0 = empty) stored in a Texture3D<uint>, positioned by `world`.
// Local space spans [0, dims] voxels; `world` already includes voxel size.
struct VoxelVolumeData
{
	CHERRY_MATRIX world;
	CHERRY_MATRIX invWorld;
	uint3 dims;
	uint  volumeTextureIndex;      // Texture3D<uint>
	uint  paletteBufferIndex;      // StructuredBuffer<float4>
	float colorJitter;             // +- relative brightness per voxel (0 = off)
	float aoStrength;              // corner ambient occlusion, 0..1 (0 = off)
	uint  materialIndex;           // into the material buffer (its surface shader is the pipeline)
	int3  jitterOffset;            // added to voxel coords for the jitter hash
	uint  pad1;
};

struct DebugVertex
{
	float3 position;
	uint   color;                  // RGBA8, R in the low byte
};

// Root constants (b0) used by each pass. Each must fit in 16 dwords.
struct MeshDrawConstants
{
	uint instanceIndex;
};

struct VoxelDrawConstants
{
	uint volumeIndex;
};

struct TonemapConstants
{
	uint hdrTextureIndex;          // Texture2D<float4>
	uint outputTextureIndex;       // RWTexture2D<float4> (UNORM, gamma encoded by the shader)
	uint width;
	uint height;
	uint bloomTextureIndex;        // Texture2D<float4> at half size, or CHERRY_INVALID_INDEX
	float bloomIntensity;
};

// One step of the bloom chain (Bloom.hlsl): read `source`, write `dest` (dest size given).
struct BloomConstants
{
	uint sourceIndex;              // Texture2D<float4>
	uint destIndex;                // RWTexture2D<float4>
	uint destWidth;
	uint destHeight;
	float threshold;               // first downsample only: brightness where bloom starts
	float knee;                    // ... and how softly it starts
	uint firstPass;                // 1: from the HDR view (threshold, firefly-proof averaging)
	float radius;                  // upsample: how far the blur spreads (in source texels)
};

struct BlitConstants
{
	uint sourceTextureIndex;
};

#ifdef __cplusplus
	} // namespace gpu
#endif

#endif // CHERRY_SHADER_INTEROP_H
