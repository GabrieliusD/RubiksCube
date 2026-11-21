#pragma once
#include "CherryAPI.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <array>
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
struct SurfaceShaderTag {};
using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;
using VoxelVolumeHandle = Handle<VoxelVolumeTag>;
using SurfaceShaderHandle = Handle<SurfaceShaderTag>;

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

// How a surface shader is drawn (Renderer::CreateSurfaceShader).
struct SurfaceShaderOptions
{
	bool translucent = false;      // blended by the surface's alpha, drawn after opaque surfaces, back to front
	float boundsPadding = 0.0f;    // metres added to mesh bounds for culling (vertex offsets move vertices)
};

// A material: PBR metallic/roughness. Without a surface shader, the built-in one shows
// baseColor x vertex colour x albedoTexture.
struct MaterialDesc
{
	DirectX::XMFLOAT4 baseColor{ 1, 1, 1, 1 };     // linear
	DirectX::XMFLOAT3 fresnelR0{ 0.04f, 0.04f, 0.04f };   // reflectance of the non-metal part (4% typical)
	float roughness = 0.5f;                         // perceptual, 0..1
	float metallic = 0.0f;                          // 0 = dielectric, 1 = metal tinted by baseColor
	TextureHandle albedoTexture;                    // optional

	// Optional surface shader (Renderer::CreateSurfaceShader) and the parameter block it
	// reads: 32-bit values (ParamFloat* in Shaders/Surface.hlsli) and texture slots
	// (SampleParamTexture). Materials that share a shader share one pipeline.
	SurfaceShaderHandle shader;
	std::vector<float> params;
	std::vector<TextureHandle> textures;
};

// A voxel volume: `size.x * size.y * size.z` palette indices, x fastest.
// Index 0 means empty; palette[0] is unused.
struct VoxelVolumeDesc
{
	DirectX::XMUINT3 size{ 0, 0, 0 };
	std::vector<uint8_t> voxels;
	std::vector<DirectX::XMFLOAT4> palette;         // sRGB colours (as picked in a colour picker), up to 256
	// Reuse another volume's palette instead of `palette` (e.g. many fragments of one world).
	VoxelVolumeHandle sharePaletteFrom;
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

// A point light (linear colour). Light falls off smoothly to zero at `range`.
struct PointLight
{
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	DirectX::XMFLOAT3 color{ 1, 1, 1 };
	float intensity = 1.0f;
	float range = 10.0f;
};

constexpr uint32_t kMaxPointLights = 8;   // matches CHERRY_MAX_POINT_LIGHTS
constexpr uint32_t kShaderGlobalCount = 4;   // matches CHERRY_SHADER_GLOBALS

// Picks which point lights to use when there are more than `maxLights`: the ones that matter
// most at `eye` (closest to reaching it: distance minus range). Returns indices into `lights`,
// in their original order.
CHERRY_API std::vector<uint32_t> SelectPointLights(const std::vector<PointLight>& lights, const DirectX::XMFLOAT3& eye, uint32_t maxLights);

// A gradient sky with a sun disk and stars, drawn wherever no mesh or voxel was drawn.
// Disabled, the background is RenderSettings::clearColor.
struct SkySettings
{
	bool enabled = false;
	DirectX::XMFLOAT3 zenith{ 0.16f, 0.34f, 0.78f };     // linear HDR, straight up
	DirectX::XMFLOAT3 horizon{ 0.60f, 0.74f, 0.90f };    // at the horizon
	DirectX::XMFLOAT3 ground{ 0.30f, 0.29f, 0.27f };     // below the horizon
	float gradientPower = 0.5f;     // < 1: the horizon colour stays low; > 1: it reaches higher
	float sunSize = 0.6f;           // angular radius of the sun disk, degrees (0 = no disk)
	float sunIntensity = 30.0f;     // disk brightness, times RenderSettings::sunColor
	float starIntensity = 0.0f;     // 0 by day; around 1 on a clear night
};

// Sun shadows: two shadow maps (cascades) around the camera, cast by opaque meshes and
// received by meshes and voxel volumes. Voxel volumes don't cast shadows yet.
struct ShadowSettings
{
	bool enabled = false;
	float nearDistance = 40.0f;     // metres from the camera covered by the sharp cascade
	float farDistance = 220.0f;     // ... and by the second one (shadows fade out towards it)
	float normalBias = 1.5f;        // shadow-map texels the lookup is pushed along the normal
};

// Glow around bright pixels (windows, fire, crystals, the sun).
struct BloomSettings
{
	bool enabled = false;
	float intensity = 0.06f;        // how much glow is added (it's the sum of several blur sizes)
	float threshold = 1.2f;         // HDR brightness (after exposure is not applied) where glow starts
	float knee = 0.6f;              // how softly it starts around the threshold
	float radius = 1.0f;            // blur spread per level
};

// Scene-wide lighting and presentation settings. Games may change any of these every frame.
struct RenderSettings
{
	DirectX::XMFLOAT4 clearColor{ 0.35f, 0.48f, 0.75f, 1.0f };   // linear HDR; this is also the sky colour
	DirectX::XMFLOAT3 ambient{ 0.25f, 0.25f, 0.32f };
	// Ambient from above is ambient x (1 + contrast), from below ambient x (1 - contrast);
	// a surface facing sideways gets `ambient`. Shiny surfaces reflect the same up/down split.
	float ambientContrast = 0.35f;
	DirectX::XMFLOAT3 sunDirection{ 0.57735f, -0.57735f, 0.57735f };
	DirectX::XMFLOAT3 sunColor{ 1.0f, 0.97f, 0.9f };              // set to 0 to switch the sun off
	float fillLightScale = 1.0f;                                  // the two fixed fill lights (0 = off)

	// With the scene's PointLight components, the kMaxPointLights that matter most at the
	// camera are used (SelectPointLights).
	std::vector<PointLight> pointLights;

	SkySettings sky;
	ShadowSettings shadows;
	BloomSettings bloom;

	// Values every surface shader can read (ShaderGlobal(i) in HLSL, the Global Input graph
	// node). Their meaning is up to the game, e.g. time of day, snow amount, wind.
	std::array<DirectX::XMFLOAT4, kShaderGlobalCount> shaderGlobals{};

	// Linear distance fog applied to meshes and voxels (not to the sky/clear colour).
	bool fogEnabled = false;
	DirectX::XMFLOAT3 fogColor{ 0.35f, 0.48f, 0.75f };            // linear
	float fogStart = 50.0f;
	float fogEnd = 200.0f;

	float exposure = 1.0f;
	bool vsync = true;
	bool cullingEnabled = true;
	bool drawBounds = false;
};
