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
		using float4x4 = DirectX::XMFLOAT4X4;
	#define CHERRY_MATRIX float4x4
#else
	#define CHERRY_MATRIX row_major float4x4
#endif

#define CHERRY_MAX_LIGHTS 4
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

	float4 ambientLight;
	Light lights[CHERRY_MAX_LIGHTS];
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

struct MaterialData
{
	float4 baseColor;
	float3 fresnelR0;
	float  roughness;
	uint   albedoTextureIndex;     // CHERRY_INVALID_INDEX = untextured
	uint   flags;
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
	uint  pad0;
	uint  pad1;
	uint  pad2;
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
};

struct BlitConstants
{
	uint sourceTextureIndex;
};

#ifdef __cplusplus
	} // namespace gpu
#endif

#endif // CHERRY_SHADER_INTEROP_H
