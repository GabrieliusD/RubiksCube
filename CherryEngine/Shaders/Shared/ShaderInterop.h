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
#define CHERRY_MAX_ROOMS 16
#define CHERRY_INVALID_INDEX 0xFFFFFFFF

// A room (RenderSettings::rooms): an upright box turned about y.
struct RoomBox
{
	float3 center;
	float  fade;                   // metres in from its walls where the room's light is full
	float3 halfExtents;
	float  ambientScale;           // the sky's ambient inside
	float4 axes;                   // (axisX.x, axisX.z, axisZ.x, axisZ.z)
};

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
	uint   particleBufferIndex;       // StructuredBuffer<ParticleData> (Particles.hlsl)
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
	// The shadows before the sun's last step (ShadowSettings::fadeSeconds), fading out.
	CHERRY_MATRIX shadowMatrixPrevious[CHERRY_SHADOW_CASCADES];
	uint   shadowMapPrevious0;
	uint   shadowMapPrevious1;
	float  shadowFade;             // weight of the previous shadows, 1 just after a step .. 0
	float  pad8;

	// An enclosed space with its own lighting (RenderSettings::interior; InteriorAmount in
	// LightingUtil.hlsl). A world-axis box whose opening is its min-z face.
	float3 interiorMin;
	uint   interiorEnabled;
	float3 interiorMax;
	float  interiorFadeStart;      // metres in from the opening where interior lighting starts
	float3 interiorAmbientSky;     // replaces ambientLight inside
	float  interiorFadeLength;     // ... and the metres over which it takes over
	float3 interiorAmbientGround;  // replaces ambientGround inside
	float  interiorFogStart;
	float3 interiorFogColor;       // fog over the part of the view ray inside
	float  interiorFogEnd;
	uint   interiorLightVolume;    // Texture3D<float> of outside light over the box, or CHERRY_INVALID_INDEX
	float  interiorLightStrength;
	float  interiorSunFromLight;
	uint   cloudsEnabled;          // RenderSettings::clouds (CloudShadow in LightingUtil.hlsl)
	float2 cloudOffset;            // metres the pattern has drifted (wind x time)
	float  cloudScale;
	float  cloudCoverage;
	float  cloudSoftness;
	float  cloudDarkness;
	float  cloudHeight;
	uint   roomCount;
	float  cloudBlur;              // 0..1: clouds blur into their average (they pass too fast to see)
	float  pad2, pad3, pad4;
	// The colour grade (RenderSettings::colorGrade), applied by the tonemap.
	float3 gradeTint;              // white balance: multiplies the HDR colour
	float  gradeSaturation;
	float3 gradeLift;              // added to the shadows (display space)
	float  gradeContrast;          // around mid grey (display space)
	float3 gradeGain;              // multiplies the highlights (display space)
	float  gradeVignette;          // corner darkening, 0..1
	uint   gradeEnabled;
	float  pad5, pad6, pad7;
	// Height fog (RenderSettings::heightFog): replaces the linear fog outside the interior.
	float3 fogSunColor;            // the haze's colour looking towards the sun
	float  fogDensity;             // per metre at fogBaseHeight
	float  fogBaseHeight;
	float  fogFalloff;             // per metre of height: how quickly it thins upwards
	uint   fogHeightEnabled;
	float  fogSunPower;            // how tightly the sun's colour gathers around it
	RoomBox rooms[CHERRY_MAX_ROOMS];
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
	float ditherCut;     // screen-door fading: 1 - the share of pixels drawn (0 = all drawn)
	uint ditherInvert;   // 1: draw the other pixels (see MeshRenderer::dither)
	float4 tint;         // multiplies the vertex colour (MeshInstance::tint); 1 for entities
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

// A particle billboard (Renderer::Particles, Particles.hlsl).
struct ParticleData
{
	float3 position;
	float  size;
	float4 color;                  // linear, alpha = opacity
	float  emissive;
	float  rotation;
	uint   flags;                  // bit 0: square
	float  stretch;                // seconds of motion drawn as a streak
	float3 velocity;               // m/s
	float  pad;
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
	uint aoTextureIndex;           // Texture2D<float> ambient occlusion, or CHERRY_INVALID_INDEX
	uint depthTextureIndex;        // the view depth: pixels not at the far plane are view models (no AO)
	float aoStrength;
	uint shaftTextureIndex;        // Texture2D<float4> light shafts at half size, or CHERRY_INVALID_INDEX
	uint exposureTextureIndex;     // Texture2D<float> 1x1 eye adaptation exposure, or CHERRY_INVALID_INDEX
	uint edgeTextureIndex;         // Texture2D<float> edge highlights (Edges.hlsl), or CHERRY_INVALID_INDEX
	float edgeStrength;
	uint toneCurve;                // 1: the ACES curve; 0: none (RenderSettings::toneCurve)
};

// Eye adaptation (EyeAdaptation.hlsl): CSReduce sums the view per 16x16 block, CSAdapt eases
// the stored exposure towards the view's.
struct EyeAdaptationConstants
{
	uint sourceIndex;              // CSReduce: the HDR view (Texture2D<float4>)
	uint blocksIndex;              // CSReduce: RWTexture2D<float2> per block (log2 sum, weight)
	uint blocksReadIndex;          // CSAdapt: the same, as Texture2D<float2>
	uint width;
	uint height;
	uint blocksX;
	uint blocksY;
	uint exposureIndex;            // RWTexture2D<float> 1x1: the exposure, kept from frame to frame
	float minLog;                  // log2 brightness range that counts
	float maxLog;
	float key;                     // the brightness the view is shown at
	float minExposure;
	float maxExposure;
	float speedBrighter;           // per second, when the view got brighter
	float speedDarker;             // ... darker
	uint reset;                    // 1: jump straight to the view's exposure
};

// Light shafts (LightShafts.hlsl): CSMain marches the view rays into a half-size texture,
// CSBlur smooths it. Sizes are the half-size texture's.
struct LightShaftConstants
{
	uint sourceIndex;              // CSBlur: the raw shafts (Texture2D<float4>)
	uint destIndex;                // RWTexture2D<float4>
	uint width;
	uint height;
	uint depthIndex;               // the full-size view depth
	float density;                 // scattering per metre outside the interior
	float interiorDensity;         // ... and inside it
	float anisotropy;              // 0 even .. towards 1: mostly towards the sun
	float intensity;
	float maxDistance;             // metres marched at most
	uint steps;
	uint pad;
};

// Screen-space ambient occlusion (Ssao.hlsl): CSMain reads the depth and writes raw AO;
// CSBlur smooths it (depth-aware) into the final texture.
struct SsaoConstants
{
	uint sourceIndex;              // CSMain: unused; CSBlur: the raw AO (Texture2D<float>)
	uint destIndex;                // RWTexture2D<float>
	uint width;
	uint height;
	uint depthIndex;               // Texture2D<float>
	float radius;                  // metres
	float bias;                    // metres
	uint pad;
};

// Anti-aliasing (Fxaa.hlsl): read the tonemapped image, write the view.
struct FxaaConstants
{
	uint sourceIndex;              // Texture2D<float4>
	uint destIndex;                // RWTexture2D<float4>
	uint width;
	uint height;
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

// RmlUi geometry (Shaders/Ui.hlsl, Renderer/UiRenderer.cpp).
struct UiConstants
{
	float2 translation;            // pixels, added before the transform
	uint   textureIndex;           // Texture2D<float4> (premultiplied), or CHERRY_INVALID_INDEX = untextured
	uint   transformIndex;         // into transformBufferIndex: projection x the element's transform
	uint   transformBufferIndex;   // StructuredBuffer<float4x4>, this frame's transforms
};

#ifdef __cplusplus
	} // namespace gpu
#endif

#endif // CHERRY_SHADER_INTEROP_H
