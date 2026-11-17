#pragma once
#include "CherryAPI.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <cstdint>
#include <vector>

// Public, D3D-free types that game code uses to talk to the renderer.

// Generational handle into a renderer-owned pool. A handle becomes stale (and
// IsValid() on the pool fails) once the resource it pointed to is destroyed.
template <typename Tag>
struct Handle
{
	uint32_t index = 0xFFFFFFFFu;
	uint32_t generation = 0;

	bool IsValid() const { return index != 0xFFFFFFFFu; }
	bool operator==(const Handle& other) const { return index == other.index && generation == other.generation; }
	bool operator!=(const Handle& other) const { return !(*this == other); }
};

struct MeshTag {};
struct MaterialTag {};
struct TextureTag {};
struct VoxelVolumeTag {};
using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;
using VoxelVolumeHandle = Handle<VoxelVolumeTag>;

// Engine vertex format (matches VertexLayout::Mesh in RHI/Pipeline.cpp).
struct Vertex
{
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	DirectX::XMFLOAT3 normal{ 0, 1, 0 };
	DirectX::XMFLOAT2 uv{ 0, 0 };
	DirectX::XMFLOAT4 color{ 1, 1, 1, 1 };
};

struct MeshData
{
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
};

struct MaterialDesc
{
	DirectX::XMFLOAT4 baseColor{ 1, 1, 1, 1 };     // linear
	DirectX::XMFLOAT3 fresnelR0{ 0.04f, 0.04f, 0.04f };
	float roughness = 0.5f;
	TextureHandle albedoTexture;                    // optional
};

// A voxel volume: `size.x * size.y * size.z` palette indices, x fastest.
// Index 0 means empty; palette[0] is unused.
struct VoxelVolumeDesc
{
	DirectX::XMUINT3 size{ 0, 0, 0 };
	std::vector<uint8_t> voxels;
	std::vector<DirectX::XMFLOAT4> palette;         // sRGB colours (as picked in a colour picker), up to 256
};

struct Ray
{
	DirectX::XMFLOAT3 origin{ 0, 0, 0 };
	DirectX::XMFLOAT3 direction{ 0, 0, 1 };         // normalized
};

struct RenderStats
{
	uint32_t drawCalls = 0;
	uint32_t instancesSubmitted = 0;
	uint32_t instancesCulled = 0;
	uint32_t voxelVolumes = 0;
	uint32_t debugLines = 0;
	uint32_t viewWidth = 0;
	uint32_t viewHeight = 0;
	uint32_t descriptorsInUse = 0;
	uint32_t d3dErrors = 0;
	uint32_t d3dWarnings = 0;
	uint32_t shaderReloads = 0;

	// GPU time in milliseconds (timestamp queries, a couple of frames old).
	float gpuFrameMs = 0.0f;
	float gpuMeshMs = 0.0f;
	float gpuVoxelMs = 0.0f;
	float gpuTonemapMs = 0.0f;
	float gpuDebugMs = 0.0f;
	float gpuUiMs = 0.0f;       // composite + ImGui
};

// Scene-wide lighting and presentation settings.
struct RenderSettings
{
	DirectX::XMFLOAT4 clearColor{ 0.35f, 0.48f, 0.75f, 1.0f };   // linear HDR
	DirectX::XMFLOAT3 ambient{ 0.25f, 0.25f, 0.32f };
	DirectX::XMFLOAT3 sunDirection{ 0.57735f, -0.57735f, 0.57735f };
	DirectX::XMFLOAT3 sunColor{ 1.0f, 0.97f, 0.9f };
	float exposure = 1.0f;
	bool vsync = true;
	bool cullingEnabled = true;
	bool drawBounds = false;
};
