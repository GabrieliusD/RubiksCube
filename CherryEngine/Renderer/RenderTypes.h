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
struct LightVolumeTag {};
using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;
using VoxelVolumeHandle = Handle<VoxelVolumeTag>;
using SurfaceShaderHandle = Handle<SurfaceShaderTag>;
using LightVolumeHandle = Handle<LightVolumeTag>;

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
	bool depthWrite = false;       // translucent only: still writes depth, so only its nearest layer shows (water: no flicker where faces overlap)
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
	// The sunlight direction the shadow maps are drawn along; (0, 0, 0) = RenderSettings::sunDirection.
	// A game whose sun moves can step this (each change turns the maps' texel grid, which makes
	// shadow edges shimmer) while the sun itself moves smoothly.
	DirectX::XMFLOAT3 direction{ 0, 0, 0 };
	// When `direction` changes, the old shadows fade into the new over this many seconds
	// instead of jumping (0 = jump). Keeps a second set of shadow maps.
	float fadeSeconds = 0.0f;
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

// An enclosed space (a cave, a mine) lit differently from the world outside. Inside it the
// directional lights (sun and fills) fade out and its own ambient and fog take over, decided
// per pixel: from inside, the outside world seen through the opening stays lit, and from
// outside, the inside stays dark. The box is aligned with the world axes and its opening is
// the face at boxMin.z; interior lighting fades in from fadeStart metres in from that face
// over fadeLength metres. Point lights work the same everywhere.
struct InteriorSettings
{
	bool enabled = false;
	DirectX::XMFLOAT3 boxMin{ 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 boxMax{ 0.0f, 0.0f, 0.0f };
	float fadeStart = 0.0f;
	float fadeLength = 4.0f;
	DirectX::XMFLOAT3 ambient{ 0.03f, 0.03f, 0.035f };       // like RenderSettings::ambient (same contrast)
	// Fog over the length of the view ray that's inside (after fadeStart); fogEnd <= fogStart: none.
	DirectX::XMFLOAT3 fogColor{ 0.0f, 0.0f, 0.0f };
	float fogStart = 0.0f;
	float fogEnd = 0.0f;
	// Optional: how much outside light reaches each part of the interior, 0..1 per cell of
	// a light volume (Renderer::CreateLightVolume) stretched over the box. Inside, the
	// world's ambient x level x lightStrength is added to the interior's, and the directional
	// lights reach in where level x sunFromLight >= 1 (their shadows decide exactly where:
	// a patch of sun under a shaft). Sampled smoothly, half a cell out from each surface.
	LightVolumeHandle lightVolume;
	float lightStrength = 1.0f;
	float sunFromLight = 0.0f;   // 0: the directional lights never reach in
};

// A room (inside a building): an upright box turned about y. Inside it the sky's ambient
// drops to ambientScale and the two fill lights fade out, over `fade` metres in from its
// walls; the sun still comes in where its shadows let it (through windows and doors).
// Up to kMaxRooms per frame (more are ignored).
constexpr uint32_t kMaxRooms = 16;
struct RoomSettings
{
	DirectX::XMFLOAT3 center{ 0, 0, 0 };
	DirectX::XMFLOAT3 halfExtents{ 1, 1, 1 };   // along the room's own axes
	DirectX::XMFLOAT3 axisX{ 1, 0, 0 };         // the room's x axis in the world (unit, horizontal)
	DirectX::XMFLOAT3 axisZ{ 0, 0, 1 };         // ... and its z axis
	float ambientScale = 0.35f;
	float fade = 0.75f;
};

// Shadows of drifting clouds (they take part of the sun): a noise pattern on a layer at
// `height`, carried by the wind and cast along the sunlight. The sky shows the clouds too.
struct CloudSettings
{
	bool enabled = false;
	float coverage = 0.4f;          // 0 clear .. 1 overcast
	float scale = 220.0f;           // metres across a typical cloud
	float softness = 0.12f;         // how soft their edges are
	float darkness = 0.65f;         // how much of the sun a cloud takes
	float height = 320.0f;          // metres above y = 0
	DirectX::XMFLOAT2 wind{ 5.0f, 2.0f };   // metres per second the clouds drift (x, z)
	DirectX::XMFLOAT2 drift{ 0.0f, 0.0f };  // metres the game has moved them itself (added to wind x time)
	float blur = 0.0f;                      // 0..1: they pass too fast to see; shadows and sky even out
};

// Screen-space ambient occlusion: darkens creases, corners and the ground under things,
// from the depth buffer. Applied to the world (not view models) before the tonemap.
struct SsaoSettings
{
	bool enabled = false;
	float radius = 0.7f;            // metres around each pixel that can occlude it
	float strength = 0.8f;          // 0 off .. 1 full
	float bias = 0.03f;             // metres: ignores occluders this close (flat surfaces)
};

// Height fog: haze that's thickest low down and thins with height (mist in a valley, clear
// peaks), replacing the linear distance fog when enabled. It uses RenderSettings::fogColor,
// warming towards `sunColor` looking at the sun, and starts after fogStart metres.
struct HeightFogSettings
{
	bool enabled = false;
	float density = 0.004f;         // per metre at baseHeight (so about 1 - e^-density*d of the view is hidden after d metres)
	float baseHeight = 0.0f;        // world height where it's that thick
	float falloff = 0.03f;          // per metre: it halves every ln(2)/falloff metres up
	DirectX::XMFLOAT3 sunColor{ 1.0f, 0.9f, 0.7f };   // linear
	float sunPower = 8.0f;
};

// Light shafts: sunlight scattered by the air, so it shows as beams where it passes between
// shadows (through trees, past a rim, down a shaft into the interior). Marched through the
// sun's shadow maps, so it needs `shadows` on to have gaps; clouds dim it.
struct LightShaftSettings
{
	bool enabled = false;
	float density = 0.003f;         // scattering per metre of air outside
	float interiorDensity = 0.03f;  // ... inside the interior box (dusty cave air)
	float anisotropy = 0.6f;        // how much more it scatters towards the sun (0..0.95)
	float intensity = 1.0f;
	float maxDistance = 160.0f;     // metres along the view ray
	int steps = 32;
};

// Eye adaptation: an exposure that follows how bright the view is (multiplied with
// RenderSettings::exposure), clamped to [minExposure, maxExposure]. It adjusts quickly to
// brightness (dazzled for a moment coming out of the dark) and slowly to darkness.
struct EyeAdaptationSettings
{
	bool enabled = false;
	float key = 0.18f;              // the average brightness the view is shown at
	float minExposure = 0.5f;
	float maxExposure = 4.0f;
	float speedBrighter = 3.0f;     // per second (higher is faster)
	float speedDarker = 0.6f;
	bool reset = false;             // jump straight to the view's exposure this frame (cuts, teleports)
};

// Edge highlights: a thin lighter line along convex edges (block corners), found from the
// view depth, fading out with distance. Applied to the world, not view models.
struct EdgeHighlightSettings
{
	bool enabled = false;
	float strength = 0.3f;          // how much brighter an edge gets
	float distance = 60.0f;         // metres where they've faded out
	float sharpness = 0.004f;       // how far (relative to distance) an edge must stand out; smaller finds softer edges
};

// A colour grade over the whole view, applied by the tonemap: white balance and saturation
// on the HDR colour, then the tone curve, then contrast, lift and gain on the displayed
// colour, then a vignette. The defaults change nothing.
struct ColorGradeSettings
{
	bool enabled = false;
	DirectX::XMFLOAT3 tint{ 1, 1, 1 };     // white balance (e.g. warmer: more red, less blue)
	float saturation = 1.0f;                // 0 grey .. 1 as is .. more is richer
	float contrast = 1.0f;                  // around mid grey
	DirectX::XMFLOAT3 lift{ 0, 0, 0 };     // raises the shadows (a faded print: lift them)
	DirectX::XMFLOAT3 gain{ 1, 1, 1 };     // scales the highlights
	float vignette = 0.0f;                  // how much the corners darken, 0..1
};

// One particle to draw this frame (Renderer::Particles): a billboard facing the camera, lit
// like the scene at its position.
struct ParticleInstance
{
	DirectX::XMFLOAT3 position{ 0, 0, 0 };
	float size = 0.1f;                         // metres across
	DirectX::XMFLOAT4 color{ 1, 1, 1, 1 };     // linear, alpha = opacity
	float emissive = 0.0f;                     // glow (x colour)
	float rotation = 0.0f;                     // radians in the view plane
	bool additive = false;                     // adds light instead of covering
	bool square = false;                       // a square block, else a soft round puff
	DirectX::XMFLOAT3 velocity{ 0, 0, 0 };     // m/s (for stretch)
	float stretch = 0.0f;                      // seconds of motion drawn as a streak along the velocity
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
	InteriorSettings interior;
	CloudSettings clouds;
	SsaoSettings ssao;
	LightShaftSettings lightShafts;
	HeightFogSettings heightFog;
	ColorGradeSettings colorGrade;
	EyeAdaptationSettings eyeAdaptation;
	EdgeHighlightSettings edgeHighlights;
	std::vector<RoomSettings> rooms;
	bool antiAliasing = true;   // FXAA after the tonemap: smooths stair-stepped edges

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
