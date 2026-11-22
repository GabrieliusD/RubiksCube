#include "Renderer/Renderer.h"
#include "Renderer/ResourcePool.h"
#include "Scene/Scene.h"
#include "RHI/Device.h"
#include "RHI/SwapChain.h"
#include "RHI/Resources.h"
#include "RHI/UploadRing.h"
#include "RHI/Pipeline.h"
#include "Shaders/Shared/ShaderInterop.h"
#include "DDSTextureLoader.h"
#include "Core/Log.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
	constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;   // gamma-encoded by the tonemap shader
	constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
	constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;

	std::filesystem::path ExecutableDir()
	{
		wchar_t buffer[MAX_PATH];
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return std::filesystem::path(buffer).parent_path();
	}

	std::filesystem::path FindShaderRoot()
	{
#if defined(_DEBUG) && defined(CHERRY_SHADER_SOURCE_DIR)
		// Development builds compile straight from the source tree so edits hot-reload.
		std::filesystem::path source = CHERRY_SHADER_SOURCE_DIR;
		if (std::filesystem::exists(source / "Mesh.hlsl"))
		{
			return source;
		}
#endif
		return ExecutableDir() / "Shaders";
	}

	DXGI_FORMAT ToSrgb(DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
		case DXGI_FORMAT_BC1_UNORM:      return DXGI_FORMAT_BC1_UNORM_SRGB;
		case DXGI_FORMAT_BC2_UNORM:      return DXGI_FORMAT_BC2_UNORM_SRGB;
		case DXGI_FORMAT_BC3_UNORM:      return DXGI_FORMAT_BC3_UNORM_SRGB;
		case DXGI_FORMAT_BC7_UNORM:      return DXGI_FORMAT_BC7_UNORM_SRGB;
		default:                         return format;
		}
	}

	struct MeshResource
	{
		std::unique_ptr<rhi::Buffer> vertexBuffer;
		std::unique_ptr<rhi::Buffer> indexBuffer;
		uint32_t indexCount = 0;
		BoundingBox bounds;
	};

	struct TextureResource
	{
		std::unique_ptr<rhi::Texture> texture;
	};

	struct LightVolumeResource
	{
		std::unique_ptr<rhi::Texture> texture;
		XMUINT3 size{};
	};

	struct VoxelResource
	{
		std::unique_ptr<rhi::Texture> volume;
		std::shared_ptr<rhi::Buffer> palette;   // may be shared between volumes
		XMUINT3 size{};
	};

	// One upload-heap structured buffer per frame in flight, grown on demand.
	struct DynamicStructuredBuffer
	{
		const char* name = "DynamicBuffer";
		uint32_t stride = 0;
		uint32_t capacity = 0;
		std::array<std::unique_ptr<rhi::Buffer>, rhi::kFramesInFlight> buffers;

		// Copies `count` elements into this frame's buffer; returns its bindless SRV index.
		uint32_t Write(rhi::Device& device, uint32_t frameIndex, const void* data, uint32_t count)
		{
			if (count > capacity || !buffers[frameIndex])
			{
				capacity = std::max({ count, capacity * 2, 64u });
				for (auto& buffer : buffers)
				{
					rhi::BufferDesc desc;
					desc.size = static_cast<uint64_t>(capacity) * stride;
					desc.stride = stride;
					desc.heap = rhi::HeapKind::Upload;
					desc.createSrv = true;
					desc.name = name;
					buffer = rhi::Buffer::Create(device, desc);   // old buffers are released deferred
				}
			}
			if (count > 0)
			{
				std::memcpy(buffers[frameIndex]->Mapped(), data, static_cast<size_t>(count) * stride);
			}
			return buffers[frameIndex]->SrvIndex();
		}

		void Reset()
		{
			for (auto& buffer : buffers) buffer.reset();
			capacity = 0;
		}
	};

	struct DrawItem
	{
		uint32_t layer;          // RenderLayer
		uint32_t translucent;    // 1: drawn after opaque surfaces, back to front
		uint32_t pipeline;       // 0 = built-in surface, otherwise surface shader slot + 1
		uint32_t meshSlot;
		uint32_t instanceIndex;
		float distanceSq;        // from the camera (translucent sorting)
	};

	// A surface shader: its pipeline in the PipelineLibrary, built from generated source.
	struct SurfaceShaderResource
	{
		std::string name;        // for diagnostics (e.g. the asset path)
		std::string pipeline;    // PipelineLibrary name
		SurfaceShaderOptions options;
		std::string source;      // the latest source (for the voxel pipeline)
		std::string voxelPipeline;   // PipelineLibrary name of the voxel variant ("" until a volume uses it)
	};

	// Wraps a surface shader's EvaluateSurface into a whole mesh-pass shader. #line keeps
	// compiler errors pointing at the surface shader's own lines.
	std::string WrapSurfaceSource(const std::string& name, const std::string& source, bool voxel = false)
	{
		std::string escaped;
		for (char c : name) escaped += (c == '"' || c == '\\') ? '/' : c;
		return "#include \"Surface.hlsli\"\n#line 1 \"" + escaped + "\"\n" + source
			+ (voxel ? "\n#include \"VoxelPass.hlsli\"\n" : "\n#include \"MeshPass.hlsli\"\n");
	}

	// GPU timestamp markers written each frame (see GpuTimer).
	// Order matches the frame: world meshes, voxels, debug lines + view models, tonemap, UI.
	enum GpuMark : uint32_t
	{
		Mark_FrameBegin,
		Mark_MeshEnd,
		Mark_VoxelEnd,
		Mark_DebugEnd,
		Mark_TonemapEnd,
		Mark_FrameEnd,
		Mark_Count
	};

	// Per-pass GPU timings from timestamp queries. Each frame slot resolves into
	// its own region of a readback buffer, which is read when the slot comes round
	// again (after Device::BeginFrame has waited for it).
	struct GpuTimer
	{
		rhi::ComPtr<ID3D12QueryHeap> heap;
		rhi::ComPtr<ID3D12Resource> readback;
		const uint64_t* mapped = nullptr;
		double ticksToMs = 0.0;
		std::array<uint32_t, rhi::kFramesInFlight> writtenMask{};

		void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
		{
			D3D12_QUERY_HEAP_DESC desc{};
			desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			desc.Count = Mark_Count * rhi::kFramesInFlight;
			DX_CHECK(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)));

			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
			CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * desc.Count);
			DX_CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
			rhi::SetDebugName(readback.Get(), "TimestampReadback");
			DX_CHECK(readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint64_t**>(&mapped))));

			uint64_t frequency = 1;
			queue->GetTimestampFrequency(&frequency);
			ticksToMs = 1000.0 / static_cast<double>(frequency);
		}

		void Shutdown()
		{
			if (readback && mapped) readback->Unmap(0, nullptr);
			mapped = nullptr;
			readback.Reset();
			heap.Reset();
		}

		void Mark(ID3D12GraphicsCommandList* cmd, uint32_t frame, GpuMark mark)
		{
			cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * Mark_Count + mark);
			writtenMask[frame] |= 1u << mark;
		}

		void Resolve(ID3D12GraphicsCommandList* cmd, uint32_t frame)
		{
			cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * Mark_Count, Mark_Count,
				readback.Get(), sizeof(uint64_t) * frame * Mark_Count);
		}

		// Reads the slot's previous results; call after the GPU finished that slot.
		void Collect(uint32_t frame, RenderStats& stats)
		{
			const uint32_t mask = writtenMask[frame];
			writtenMask[frame] = 0;
			if (!mapped || (mask & (1u << Mark_FrameBegin)) == 0 || (mask & (1u << Mark_FrameEnd)) == 0) return;

			const uint64_t* t = mapped + frame * Mark_Count;
			auto span = [&](GpuMark from, GpuMark to) -> float
			{
				if ((mask & (1u << from)) == 0 || (mask & (1u << to)) == 0 || t[to] < t[from]) return 0.0f;
				return static_cast<float>((t[to] - t[from]) * ticksToMs);
			};
			stats.gpuFrameMs = span(Mark_FrameBegin, Mark_FrameEnd);
			stats.gpuMeshMs = span(Mark_FrameBegin, Mark_MeshEnd);
			stats.gpuVoxelMs = span(Mark_MeshEnd, Mark_VoxelEnd);
			stats.gpuDebugMs = span(Mark_VoxelEnd, Mark_DebugEnd);        // debug lines + view models
			stats.gpuTonemapMs = span(Mark_DebugEnd, Mark_TonemapEnd);
			stats.gpuUiMs = span(Mark_TonemapEnd, Mark_FrameEnd);
		}
	};
}

struct Renderer::Impl
{
	rhi::Device device;
	rhi::SwapChain swapChain;
	rhi::UploadRing uploadRing;
	rhi::PipelineLibrary pipelines;
	rhi::ComPtr<ID3D12RootSignature> rootSignature;
	HWND hwnd = nullptr;
	bool initialized = false;

	ResourcePool<MeshResource, MeshHandle> meshes;
	ResourcePool<TextureResource, TextureHandle> textures;
	ResourcePool<MaterialDesc, MaterialHandle> materials;
	ResourcePool<VoxelResource, VoxelVolumeHandle> voxelVolumes;
	ResourcePool<LightVolumeResource, LightVolumeHandle> lightVolumes;
	ResourcePool<SurfaceShaderResource, SurfaceShaderHandle> surfaceShaders;
	uint32_t nextSurfacePipeline = 0;
	rhi::GraphicsPipelineDesc meshPipelineDesc;   // what surface shader pipelines are built from
	rhi::GraphicsPipelineDesc voxelPipelineDesc;  // ... and their voxel variants
	MaterialHandle defaultMaterial;
	GpuTimer gpuTimer;

	DynamicStructuredBuffer instanceBuffer{ "InstanceBuffer", sizeof(gpu::InstanceData) };
	DynamicStructuredBuffer shadowInstanceBuffer{ "ShadowInstanceBuffer", sizeof(gpu::InstanceData) };
	DynamicStructuredBuffer shadowVoxelBuffer{ "ShadowVoxelBuffer", sizeof(gpu::VoxelVolumeData) };
	DynamicStructuredBuffer materialBuffer{ "MaterialBuffer", sizeof(gpu::MaterialData) };
	DynamicStructuredBuffer materialParamBuffer{ "MaterialParamBuffer", sizeof(uint32_t) };
	DynamicStructuredBuffer voxelBuffer{ "VoxelVolumeBuffer", sizeof(gpu::VoxelVolumeData) };
	DynamicStructuredBuffer debugVertexBuffer{ "DebugVertexBuffer", sizeof(gpu::DebugVertex) };
	DynamicStructuredBuffer particleBuffer{ "ParticleBuffer", sizeof(gpu::ParticleData) };
	std::vector<ParticleInstance> particles;           // this frame's, from Renderer::Particles
	std::vector<gpu::ParticleData> particleScratch;

	// Offscreen view targets.
	std::unique_ptr<rhi::Texture> hdr;
	std::unique_ptr<rhi::Texture> ldrBeforeAA;   // the tonemap's output when anti-aliasing is on
	std::unique_ptr<rhi::Texture> depth;
	std::unique_ptr<rhi::Texture> aoRaw, ao;   // screen-space ambient occlusion
	std::unique_ptr<rhi::Texture> edges;       // edge highlights
	std::unique_ptr<rhi::Texture> shaftRaw, shaft;   // light shafts, half size
	std::unique_ptr<rhi::Texture> luminanceBlocks;   // eye adaptation: per 16x16 block
	std::unique_ptr<rhi::Texture> adaptedExposure;   // eye adaptation: 1x1, kept across frames
	bool exposureValid = false;
	std::unique_ptr<rhi::Texture> ldr;
	uint32_t viewWidth = 0, viewHeight = 0;
	uint32_t requestedViewWidth = 1280, requestedViewHeight = 720;

	uint32_t pendingWindowWidth = 0, pendingWindowHeight = 0;
	D3D12_RESOURCE_STATES backBufferState = D3D12_RESOURCE_STATE_PRESENT;

	RenderSettings settings;
	RenderStats stats;
	DebugDraw debugDraw;
	std::string imguiIniPath;
	std::string emptyString;
	float totalTime = 0.0f, deltaTime = 0.0f;
	bool viewRendered = false;
	bool frameActive = false;

	std::vector<gpu::InstanceData> instanceScratch;
	std::vector<gpu::InstanceData> sortedInstanceScratch;

	// Sun shadows: one depth map per cascade, and this frame's cascades.
	static constexpr uint32_t kShadowMapSize = 2048;
	std::unique_ptr<rhi::Texture> shadowMaps[CHERRY_SHADOW_CASCADES];
	// Shadow fades (ShadowSettings::fadeSeconds): the maps from before the last step.
	std::unique_ptr<rhi::Texture> previousShadowMaps[CHERRY_SHADOW_CASCADES];
	XMFLOAT3 shadowDirectionDrawn{ 0, 0, 0 };   // the direction the current maps were drawn along
	float shadowFade = 0.0f;                    // weight of the previous maps
	struct ShadowCascade
	{
		BoundingOrientedBox box;    // world-space volume whose casters it draws
		XMFLOAT4X4 viewProj;        // light view x orthographic projection
		XMFLOAT4X4 uv;              // world -> (u, v, depth)
		float texelWorld = 0.0f;    // world size of one texel
		float split = 0.0f;         // view distance it covers
	};
	ShadowCascade cascades[CHERRY_SHADOW_CASCADES];
	ShadowCascade previousCascades[CHERRY_SHADOW_CASCADES];
	struct ShadowItem { uint32_t meshSlot; uint32_t instance; };
	std::vector<ShadowItem> shadowScratch[CHERRY_SHADOW_CASCADES];
	std::vector<gpu::InstanceData> shadowCasterScratch;     // instances, as the casters were found
	std::vector<gpu::InstanceData> shadowInstanceScratch;   // ... in draw order, cascade after cascade
	std::vector<gpu::VoxelVolumeData> shadowVoxelScratch;   // voxel volumes that reach a cascade
	void CreateShadowMaps();
	std::unique_ptr<rhi::Texture> CreateShadowMap();
	void SetupShadowCascades(const XMMATRIX& view, const XMMATRIX& proj, const RenderView& renderView);

	// Bloom: half the view's size, then halving down the chain.
	std::vector<std::unique_ptr<rhi::Texture>> bloomChain;
	std::vector<gpu::MaterialData> materialScratch;
	std::vector<uint32_t> materialParamScratch;
	std::vector<gpu::VoxelVolumeData> voxelScratch;
	std::vector<DrawItem> drawScratch;
	std::vector<uint32_t> voxelDrawScratch;

	void CreatePipelines();
	// The PSO for DrawItem::pipeline (the error stand-in for surface shaders that never compiled).
	ID3D12PipelineState* SurfacePipeline(uint32_t pipeline);
	// The same for the voxel pass (0 = built-in); asks for the voxel variant the first time.
	ID3D12PipelineState* VoxelPipeline(uint32_t pipeline);
	void BuildVoxelVariant(SurfaceShaderResource& shader);
	void CreateViewTargets(uint32_t width, uint32_t height);
	void InitImGui();
	void ShutdownImGui();
	void SetViewport(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height);
	void BindGlobals(ID3D12GraphicsCommandList4* cmd);
	gpu::FrameConstants BuildFrameConstants(const XMFLOAT3* eye = nullptr);   // eye: picks the point lights that matter there
	std::vector<PointLight> sceneLights;   // PointLight components of the scene being rendered
	std::vector<PointLight> pointLightScratch;
	void ClearView(ID3D12GraphicsCommandList4* cmd);
	// Fills materialScratch / materialParamScratch from the material pool: one GPU entry per
	// pool slot, plus its parameter block.
	void BuildMaterialScratch();

	// Material preview (RenderPreview): its own targets and per-frame buffers, so it doesn't
	// disturb the scene's (RenderScene writes those later in the frame).
	std::unique_ptr<rhi::Texture> previewHdr;
	std::unique_ptr<rhi::Texture> previewDepth;
	std::unique_ptr<rhi::Texture> previewLdr;
	uint32_t previewSize = 0;
	DynamicStructuredBuffer previewInstanceBuffer{ "PreviewInstanceBuffer", sizeof(gpu::InstanceData) };
	DynamicStructuredBuffer previewMaterialBuffer{ "PreviewMaterialBuffer", sizeof(gpu::MaterialData) };
	DynamicStructuredBuffer previewParamBuffer{ "PreviewMaterialParamBuffer", sizeof(uint32_t) };
	void CreatePreviewTargets(uint32_t size);
};

// =============================================================================
// Setup
// =============================================================================

Renderer::Renderer() : mImpl(std::make_unique<Impl>()) {}

Renderer::~Renderer()
{
	Shutdown();
}

bool Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height, std::string& error)
{
	Impl& r = *mImpl;
	r.hwnd = hwnd;

	rhi::DeviceOptions options;
#if defined(_DEBUG)
	options.debugLayer = true;
	char gbv[8] = {};
	options.gpuBasedValidation = GetEnvironmentVariableA("CHERRY_GPU_VALIDATION", gbv, sizeof(gbv)) > 0 && gbv[0] == '1';
#endif
	if (!r.device.Initialize(options, error))
	{
		return false;
	}

	r.swapChain.Initialize(r.device, hwnd, width, height);
	r.uploadRing.Initialize(r.device, 4 * 1024 * 1024);
	r.rootSignature = rhi::CreateBindlessRootSignature(r.device.D3D());
	r.pipelines.Initialize(r.device, r.rootSignature.Get(), FindShaderRoot());

	try
	{
		r.CreatePipelines();
	}
	catch (const std::exception& e)
	{
		error = e.what();
		return false;
	}

	MaterialDesc defaultDesc;
	defaultDesc.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
	r.defaultMaterial = r.materials.Add(MaterialDesc(defaultDesc));

	r.CreateViewTargets(r.requestedViewWidth, r.requestedViewHeight);
	r.CreateShadowMaps();
	r.gpuTimer.Initialize(r.device.D3D(), r.device.Queue());
	r.InitImGui();
	r.initialized = true;
	return true;
}

void Renderer::Impl::CreatePipelines()
{
	rhi::GraphicsPipelineDesc mesh;
	mesh.shaderFile = "Mesh.hlsl";
	mesh.rtvFormats = { kHdrFormat };
	mesh.dsvFormat = kDepthFormat;
	mesh.cullMode = D3D12_CULL_MODE_NONE;   // game meshes aren't guaranteed to have consistent winding yet
	pipelines.AddGraphics("mesh", mesh);
	meshPipelineDesc = mesh;

	rhi::GraphicsPipelineDesc meshError = mesh;
	meshError.defines = { "CHERRY_ERROR_SURFACE" };
	pipelines.AddGraphics("mesh_error", meshError);

	rhi::GraphicsPipelineDesc voxel;
	voxel.shaderFile = "VoxelRaymarch.hlsl";
	voxel.vertexLayout = rhi::VertexLayout::None;
	voxel.rtvFormats = { kHdrFormat };
	voxel.dsvFormat = kDepthFormat;
	// The box's outward faces wind counter-clockwise (D3D back faces), so culling back
	// faces leaves only the far side: one ray per pixel, and it still works with the
	// camera inside the volume.
	voxel.cullMode = D3D12_CULL_MODE_BACK;
	pipelines.AddGraphics("voxel", voxel);
	voxelPipelineDesc = voxel;

	rhi::GraphicsPipelineDesc voxelError = voxel;
	voxelError.defines = { "CHERRY_ERROR_SURFACE" };
	pipelines.AddGraphics("voxel_error", voxelError);

	rhi::ComputePipelineDesc tonemap;
	tonemap.shaderFile = "Tonemap.hlsl";
	pipelines.AddCompute("tonemap", tonemap);

	rhi::GraphicsPipelineDesc debug;
	debug.shaderFile = "DebugDraw.hlsl";
	debug.vertexLayout = rhi::VertexLayout::None;
	debug.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	debug.rtvFormats = { kHdrFormat };
	debug.dsvFormat = kDepthFormat;
	debug.depthWrite = false;
	debug.cullMode = D3D12_CULL_MODE_NONE;
	pipelines.AddGraphics("debug", debug);

	// Particles: camera-facing quads pulled from a buffer, depth-tested but not written.
	rhi::GraphicsPipelineDesc particles;
	particles.shaderFile = "Particles.hlsl";
	particles.vertexLayout = rhi::VertexLayout::None;
	particles.rtvFormats = { kHdrFormat };
	particles.dsvFormat = kDepthFormat;
	particles.depthWrite = false;
	particles.cullMode = D3D12_CULL_MODE_NONE;
	particles.alphaBlend = true;
	pipelines.AddGraphics("particles", particles);
	particles.alphaBlend = false;
	particles.additiveBlend = true;
	pipelines.AddGraphics("particles_add", particles);

	rhi::GraphicsPipelineDesc shadow;
	shadow.shaderFile = "ShadowPass.hlsl";
	shadow.psEntry = "";                     // depth only
	shadow.dsvFormat = kDepthFormat;
	shadow.cullMode = D3D12_CULL_MODE_NONE;  // both sides cast (game meshes' winding isn't guaranteed)
	shadow.slopeScaledDepthBias = 2.5f;
	shadow.depthBiasClamp = 0.01f;
	pipelines.AddGraphics("shadow", shadow);

	rhi::GraphicsPipelineDesc voxelShadow;
	voxelShadow.shaderFile = "VoxelShadow.hlsl";
	voxelShadow.vertexLayout = rhi::VertexLayout::None;
	voxelShadow.dsvFormat = kDepthFormat;
	voxelShadow.cullMode = D3D12_CULL_MODE_BACK;   // the box's far side: one ray per pixel (as in the voxel pass)
	voxelShadow.slopeScaledDepthBias = 2.5f;
	voxelShadow.depthBiasClamp = 0.01f;
	pipelines.AddGraphics("voxel_shadow", voxelShadow);

	rhi::ComputePipelineDesc fxaa;
	fxaa.shaderFile = "Fxaa.hlsl";
	pipelines.AddCompute("fxaa", fxaa);

	rhi::ComputePipelineDesc ssao;
	ssao.shaderFile = "Ssao.hlsl";
	ssao.entry = "CSMain";
	pipelines.AddCompute("ssao", ssao);
	rhi::ComputePipelineDesc ssaoBlur;
	ssaoBlur.shaderFile = "Ssao.hlsl";
	ssaoBlur.entry = "CSBlur";
	pipelines.AddCompute("ssao_blur", ssaoBlur);
	rhi::ComputePipelineDesc shafts;
	shafts.shaderFile = "LightShafts.hlsl";
	shafts.entry = "CSMain";
	pipelines.AddCompute("shafts", shafts);
	rhi::ComputePipelineDesc edgePass;
	edgePass.shaderFile = "Edges.hlsl";
	edgePass.entry = "CSMain";
	pipelines.AddCompute("edges", edgePass);
	rhi::ComputePipelineDesc shaftsBlur;
	shaftsBlur.shaderFile = "LightShafts.hlsl";
	shaftsBlur.entry = "CSBlur";
	pipelines.AddCompute("shafts_blur", shaftsBlur);
	rhi::ComputePipelineDesc eyeReduce;
	eyeReduce.shaderFile = "EyeAdaptation.hlsl";
	eyeReduce.entry = "CSReduce";
	pipelines.AddCompute("eye_reduce", eyeReduce);
	rhi::ComputePipelineDesc eyeAdapt;
	eyeAdapt.shaderFile = "EyeAdaptation.hlsl";
	eyeAdapt.entry = "CSAdapt";
	pipelines.AddCompute("eye_adapt", eyeAdapt);

	rhi::ComputePipelineDesc bloomDown;
	bloomDown.shaderFile = "Bloom.hlsl";
	bloomDown.entry = "CSDownsample";
	pipelines.AddCompute("bloom_down", bloomDown);
	rhi::ComputePipelineDesc bloomUp;
	bloomUp.shaderFile = "Bloom.hlsl";
	bloomUp.entry = "CSUpsample";
	pipelines.AddCompute("bloom_up", bloomUp);

	rhi::GraphicsPipelineDesc sky;
	sky.shaderFile = "SkyPass.hlsl";
	sky.vertexLayout = rhi::VertexLayout::None;
	sky.rtvFormats = { kHdrFormat };
	sky.dsvFormat = kDepthFormat;
	sky.depthWrite = false;   // z = 1 passes LESS_EQUAL only where the depth is still clear
	sky.cullMode = D3D12_CULL_MODE_NONE;
	pipelines.AddGraphics("sky", sky);

	rhi::GraphicsPipelineDesc blit;
	blit.shaderFile = "Blit.hlsl";
	blit.vertexLayout = rhi::VertexLayout::None;
	blit.rtvFormats = { rhi::SwapChain::kFormat };
	blit.depthTest = false;
	blit.depthWrite = false;
	blit.cullMode = D3D12_CULL_MODE_NONE;
	pipelines.AddGraphics("blit", blit);
}

ID3D12PipelineState* Renderer::Impl::SurfacePipeline(uint32_t pipeline)
{
	if (pipeline > 0)
	{
		const SurfaceShaderResource* shader = surfaceShaders.AtSlot(pipeline - 1);
		ID3D12PipelineState* pso = shader ? pipelines.Get(shader->pipeline) : nullptr;
		if (pso) return pso;
		// Never built: the built-in surface while the first build runs, the checker if it failed.
		return shader && pipelines.IsPending(shader->pipeline) ? pipelines.Get("mesh") : pipelines.Get("mesh_error");
	}
	return pipelines.Get("mesh");
}

ID3D12PipelineState* Renderer::Impl::VoxelPipeline(uint32_t pipeline)
{
	SurfaceShaderResource* shader = pipeline > 0 ? surfaceShaders.AtSlot(pipeline - 1) : nullptr;
	if (!shader) return pipelines.Get("voxel");
	if (shader->voxelPipeline.empty()) BuildVoxelVariant(*shader);
	if (ID3D12PipelineState* pso = pipelines.Get(shader->voxelPipeline)) return pso;
	// Never built: the built-in look while the first build runs, the checker if it failed.
	return pipelines.IsPending(shader->voxelPipeline) ? pipelines.Get("voxel") : pipelines.Get("voxel_error");
}

void Renderer::Impl::BuildVoxelVariant(SurfaceShaderResource& shader)
{
	if (shader.voxelPipeline.empty()) shader.voxelPipeline = shader.pipeline + ":voxel";
	rhi::GraphicsPipelineDesc desc = voxelPipelineDesc;
	desc.shaderFile = shader.name;
	desc.source = WrapSurfaceSource(shader.name, shader.source, true);
	pipelines.SetGraphicsAsync(shader.voxelPipeline, desc, {});
}

void Renderer::Impl::CreateViewTargets(uint32_t width, uint32_t height)
{
	width = std::max(width, 1u);
	height = std::max(height, 1u);

	rhi::TextureDesc hdrDesc;
	hdrDesc.width = width;
	hdrDesc.height = height;
	hdrDesc.format = kHdrFormat;
	hdrDesc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
	hdrDesc.clearColor[0] = settings.clearColor.x;
	hdrDesc.clearColor[1] = settings.clearColor.y;
	hdrDesc.clearColor[2] = settings.clearColor.z;
	hdrDesc.clearColor[3] = settings.clearColor.w;
	hdrDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	hdrDesc.name = "ViewHDR";
	hdr = rhi::Texture::Create(device, hdrDesc);

	rhi::TextureDesc depthDesc;
	depthDesc.width = width;
	depthDesc.height = height;
	depthDesc.format = kDepthResourceFormat;
	depthDesc.dsvFormat = kDepthFormat;
	depthDesc.srvFormat = kDepthSrvFormat;
	depthDesc.usage = rhi::TextureUsage_DepthStencil | rhi::TextureUsage_ShaderResource;
	depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depthDesc.name = "ViewDepth";
	depth = rhi::Texture::Create(device, depthDesc);

	rhi::TextureDesc ldrDesc;
	ldrDesc.width = width;
	ldrDesc.height = height;
	ldrDesc.format = kLdrFormat;
	ldrDesc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	ldrDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ldrDesc.name = "ViewLDR";
	ldr = rhi::Texture::Create(device, ldrDesc);
	ldrDesc.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	ldrDesc.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	ldrDesc.name = "ViewLDRBeforeAA";
	ldrBeforeAA = rhi::Texture::Create(device, ldrDesc);

	// Ambient occlusion: raw, then blurred.
	rhi::TextureDesc aoDesc;
	aoDesc.width = width;
	aoDesc.height = height;
	aoDesc.format = DXGI_FORMAT_R8_UNORM;
	aoDesc.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	aoDesc.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	aoDesc.name = "AmbientOcclusionRaw";
	aoRaw = rhi::Texture::Create(device, aoDesc);
	aoDesc.name = "AmbientOcclusion";
	ao = rhi::Texture::Create(device, aoDesc);
	aoDesc.name = "EdgeHighlights";
	edges = rhi::Texture::Create(device, aoDesc);

	// Light shafts: half size, raw then blurred.
	rhi::TextureDesc shaftDesc;
	shaftDesc.width = std::max((width + 1) / 2, 1u);
	shaftDesc.height = std::max((height + 1) / 2, 1u);
	shaftDesc.format = kHdrFormat;
	shaftDesc.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	shaftDesc.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	shaftDesc.name = "LightShaftsRaw";
	shaftRaw = rhi::Texture::Create(device, shaftDesc);
	shaftDesc.name = "LightShafts";
	shaft = rhi::Texture::Create(device, shaftDesc);

	// Eye adaptation: brightness per 16x16 block, and the exposure (kept across resizes).
	rhi::TextureDesc blocksDesc;
	blocksDesc.width = (width + 15) / 16;
	blocksDesc.height = (height + 15) / 16;
	blocksDesc.format = DXGI_FORMAT_R32G32_FLOAT;
	blocksDesc.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	blocksDesc.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	blocksDesc.name = "LuminanceBlocks";
	luminanceBlocks = rhi::Texture::Create(device, blocksDesc);
	if (!adaptedExposure)
	{
		rhi::TextureDesc exposureDesc;
		exposureDesc.width = exposureDesc.height = 1;
		exposureDesc.format = DXGI_FORMAT_R32_FLOAT;
		exposureDesc.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
		exposureDesc.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		exposureDesc.name = "AdaptedExposure";
		adaptedExposure = rhi::Texture::Create(device, exposureDesc);
		exposureValid = false;
	}

	// Bloom chain: half size, then halving while it's at least 8 texels, up to 6 levels.
	bloomChain.clear();
	for (uint32_t w = std::max(width / 2, 1u), h = std::max(height / 2, 1u); bloomChain.size() < 6; w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
	{
		rhi::TextureDesc level;
		level.width = w;
		level.height = h;
		level.format = kHdrFormat;
		level.usage = rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
		level.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		level.name = "Bloom";
		bloomChain.push_back(rhi::Texture::Create(device, level));
		if (w < 16 || h < 16) break;
	}

	viewWidth = width;
	viewHeight = height;
}

std::unique_ptr<rhi::Texture> Renderer::Impl::CreateShadowMap()
{
	rhi::TextureDesc desc;
	desc.width = desc.height = kShadowMapSize;
	desc.format = kDepthResourceFormat;
	desc.dsvFormat = kDepthFormat;
	desc.srvFormat = kDepthSrvFormat;
	desc.usage = rhi::TextureUsage_DepthStencil | rhi::TextureUsage_ShaderResource;
	desc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	desc.name = "ShadowMap";
	return rhi::Texture::Create(device, desc);
}

void Renderer::Impl::CreateShadowMaps()
{
	for (auto& map : shadowMaps) map = CreateShadowMap();
}

// Fits each cascade around its slice of the view frustum: a sphere around the slice (so the
// map doesn't change size as the camera turns), in the sun's space, moved in whole texels
// (so shadow edges don't shimmer as the camera moves). It reaches far towards the sun so
// tall casters outside the view still cast into it.
void Renderer::Impl::SetupShadowCascades(const XMMATRIX& view, const XMMATRIX& proj, const RenderView& renderView)
{
	const XMMATRIX invView = XMMatrixInverse(nullptr, view);
	const XMVECTOR eye = XMLoadFloat3(&renderView.position);
	const XMVECTOR forward = XMVector3Normalize(invView.r[2]);
	XMFLOAT4X4 p;
	XMStoreFloat4x4(&p, proj);
	const float tanX = 1.0f / p._11, tanY = 1.0f / p._22;
	const float k2 = tanX * tanX + tanY * tanY;

	const XMFLOAT3& shadowSun = settings.shadows.direction;
	const bool ownDirection = shadowSun.x != 0.0f || shadowSun.y != 0.0f || shadowSun.z != 0.0f;
	const XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(ownDirection ? &shadowSun : &settings.sunDirection));
	const XMVECTOR up = std::fabs(XMVectorGetY(sun)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
	const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), sun, up);
	const XMMATRIX lightToWorld = XMMatrixInverse(nullptr, lightView);
	const XMMATRIX toUv = XMMatrixScaling(0.5f, -0.5f, 1.0f) * XMMatrixTranslation(0.5f, 0.5f, 0.0f);

	const float splits[CHERRY_SHADOW_CASCADES + 1] = { std::max(renderView.nearZ, 0.01f),
		std::max(settings.shadows.nearDistance, 1.0f), std::max(settings.shadows.farDistance, settings.shadows.nearDistance + 1.0f) };
	for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c)
	{
		const float n = splits[c], f = splits[c + 1];
		const float zc = std::min(f, 0.5f * (n + f) * (1.0f + k2));
		const float radius = std::max(std::sqrt((f - zc) * (f - zc) + f * f * k2), std::sqrt((zc - n) * (zc - n) + n * n * k2));
		const XMVECTOR center = XMVectorAdd(eye, XMVectorScale(forward, zc));

		XMFLOAT3 cl;
		XMStoreFloat3(&cl, XMVector3TransformCoord(center, lightView));
		const float texel = 2.0f * radius / kShadowMapSize;
		cl.x = std::floor(cl.x / texel) * texel;
		cl.y = std::floor(cl.y / texel) * texel;
		const float reach = 400.0f;   // towards the sun, for casters outside the slice
		const float zNear = cl.z - radius - reach, zFar = cl.z + radius;
		const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(cl.x - radius, cl.x + radius, cl.y - radius, cl.y + radius, zNear, zFar);

		ShadowCascade& cascade = cascades[c];
		XMStoreFloat4x4(&cascade.viewProj, lightView * lightProj);
		XMStoreFloat4x4(&cascade.uv, lightView * lightProj * toUv);
		cascade.texelWorld = texel;
		cascade.split = f;
		BoundingOrientedBox::CreateFromBoundingBox(cascade.box,
			BoundingBox(XMFLOAT3(cl.x, cl.y, 0.5f * (zNear + zFar)), XMFLOAT3(radius, radius, 0.5f * (zFar - zNear))));
		cascade.box.Transform(cascade.box, lightToWorld);
	}
}

void Renderer::Impl::CreatePreviewTargets(uint32_t size)
{
	rhi::TextureDesc hdrDesc;
	hdrDesc.width = hdrDesc.height = size;
	hdrDesc.format = kHdrFormat;
	hdrDesc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource;
	hdrDesc.clearColor[0] = 0.08f;
	hdrDesc.clearColor[1] = 0.08f;
	hdrDesc.clearColor[2] = 0.09f;
	hdrDesc.clearColor[3] = 1.0f;
	hdrDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	hdrDesc.name = "PreviewHDR";
	previewHdr = rhi::Texture::Create(device, hdrDesc);

	rhi::TextureDesc depthDesc;
	depthDesc.width = depthDesc.height = size;
	depthDesc.format = kDepthResourceFormat;
	depthDesc.dsvFormat = kDepthFormat;
	depthDesc.srvFormat = kDepthSrvFormat;
	depthDesc.usage = rhi::TextureUsage_DepthStencil | rhi::TextureUsage_ShaderResource;
	depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depthDesc.name = "PreviewDepth";
	previewDepth = rhi::Texture::Create(device, depthDesc);

	rhi::TextureDesc ldrDesc;
	ldrDesc.width = ldrDesc.height = size;
	ldrDesc.format = kLdrFormat;
	ldrDesc.usage = rhi::TextureUsage_RenderTarget | rhi::TextureUsage_ShaderResource | rhi::TextureUsage_UnorderedAccess;
	ldrDesc.initialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ldrDesc.name = "PreviewLDR";
	previewLdr = rhi::Texture::Create(device, ldrDesc);
	previewSize = size;
}

void Renderer::Impl::BuildMaterialScratch()
{
	materialScratch.resize(materials.SlotCount());
	materialParamScratch.clear();
	for (uint32_t slot = 0; slot < materials.SlotCount(); ++slot)
	{
		gpu::MaterialData& gpuMaterial = materialScratch[slot];
		gpuMaterial = {};
		gpuMaterial.albedoTextureIndex = CHERRY_INVALID_INDEX;
		if (const MaterialDesc* desc = materials.AtSlot(slot))
		{
			gpuMaterial.baseColor = desc->baseColor;
			gpuMaterial.fresnelR0 = desc->fresnelR0;
			gpuMaterial.roughness = desc->roughness;
			gpuMaterial.metallic = desc->metallic;
			if (const TextureResource* texture = textures.Get(desc->albedoTexture))
			{
				gpuMaterial.albedoTextureIndex = texture->texture->SrvIndex();
			}

			gpuMaterial.paramOffset = static_cast<uint32_t>(materialParamScratch.size());
			gpuMaterial.paramCount = static_cast<uint32_t>(desc->params.size());
			for (float value : desc->params)
			{
				uint32_t bits;
				std::memcpy(&bits, &value, sizeof(bits));
				materialParamScratch.push_back(bits);
			}
			gpuMaterial.textureOffset = static_cast<uint32_t>(materialParamScratch.size());
			gpuMaterial.textureCount = static_cast<uint32_t>(desc->textures.size());
			for (TextureHandle handle : desc->textures)
			{
				const TextureResource* texture = textures.Get(handle);
				materialParamScratch.push_back(texture ? texture->texture->SrvIndex() : CHERRY_INVALID_INDEX);
			}
		}
	}
}

void Renderer::Impl::InitImGui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	imguiIniPath = (ExecutableDir() / "imgui.ini").string();
	io.IniFilename = imguiIniPath.c_str();
	ImGui::StyleColorsDark();

	// Match the UI to the monitor's DPI (the process is per-monitor DPI aware).
	const float dpiScale = GetDpiForWindow(hwnd) / 96.0f;
	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(dpiScale);
	style.FontScaleDpi = dpiScale;

	ImGui_ImplWin32_Init(hwnd);

	ImGui_ImplDX12_InitInfo info;
	info.Device = device.D3D();
	info.CommandQueue = device.Queue();
	info.NumFramesInFlight = static_cast<int>(rhi::kFramesInFlight);
	info.RTVFormat = rhi::SwapChain::kFormat;
	info.DSVFormat = DXGI_FORMAT_UNKNOWN;
	info.UserData = this;
	info.SrvDescriptorHeap = device.SrvHeap().Heap();
	info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* init, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
	{
		auto* self = static_cast<Impl*>(init->UserData);
		rhi::DescriptorHandle handle = self->device.SrvHeap().Allocate();
		*cpu = handle.cpu;
		*gpu = handle.gpu;
	};
	info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* init, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
	{
		auto* self = static_cast<Impl*>(init->UserData);
		rhi::DescriptorHeap& heap = self->device.SrvHeap();
		const auto start = heap.Heap()->GetCPUDescriptorHandleForHeapStart().ptr;
		rhi::DescriptorHandle handle = heap.HandleAt(static_cast<uint32_t>((cpu.ptr - start) / heap.DescriptorSize()));
		self->device.FreeDescriptor(heap, handle);
	};
	ImGui_ImplDX12_Init(&info);
}

void Renderer::Impl::ShutdownImGui()
{
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void Renderer::Shutdown()
{
	Impl& r = *mImpl;
	if (!r.initialized) return;
	r.device.WaitIdle();

	r.ShutdownImGui();
	r.gpuTimer.Shutdown();
	r.hdr.reset();
	r.depth.reset();
	r.ldr.reset();
	r.ldrBeforeAA.reset();
	r.instanceBuffer.Reset();
	r.shadowInstanceBuffer.Reset();
	r.shadowVoxelBuffer.Reset();
	for (auto& map : r.shadowMaps) map.reset();
	for (auto& map : r.previousShadowMaps) map.reset();
	r.bloomChain.clear();
	r.materialBuffer.Reset();
	r.materialParamBuffer.Reset();
	r.voxelBuffer.Reset();
	r.debugVertexBuffer.Reset();
	r.particleBuffer.Reset();
	r.previewHdr.reset();
	r.previewDepth.reset();
	r.previewLdr.reset();
	r.previewInstanceBuffer.Reset();
	r.previewMaterialBuffer.Reset();
	r.previewParamBuffer.Reset();
	r.meshes.Clear();
	r.textures.Clear();
	r.materials.Clear();
	r.voxelVolumes.Clear();
	r.surfaceShaders.Clear();
	r.pipelines.Shutdown();
	r.rootSignature.Reset();
	r.uploadRing.Shutdown();
	r.swapChain.Shutdown();
	r.device.Shutdown();
	r.initialized = false;
}

// =============================================================================
// Resources
// =============================================================================

MeshHandle Renderer::CreateMesh(const MeshData& data)
{
	Impl& r = *mImpl;
	if (data.vertices.empty() || data.indices.empty())
	{
		CHERRY_WARN("CreateMesh called with empty data");
		return {};
	}

	MeshResource mesh;
	rhi::BufferDesc vbDesc;
	vbDesc.size = data.vertices.size() * sizeof(Vertex);
	vbDesc.name = "MeshVertices";
	mesh.vertexBuffer = rhi::Buffer::Create(r.device, vbDesc);
	mesh.vertexBuffer->Upload(data.vertices.data(), vbDesc.size);

	rhi::BufferDesc ibDesc;
	ibDesc.size = data.indices.size() * sizeof(uint32_t);
	ibDesc.name = "MeshIndices";
	mesh.indexBuffer = rhi::Buffer::Create(r.device, ibDesc);
	mesh.indexBuffer->Upload(data.indices.data(), ibDesc.size);

	mesh.indexCount = static_cast<uint32_t>(data.indices.size());
	BoundingBox::CreateFromPoints(mesh.bounds, data.vertices.size(), &data.vertices[0].position, sizeof(Vertex));
	return r.meshes.Add(std::move(mesh));
}

void Renderer::DestroyMesh(MeshHandle mesh)
{
	mImpl->meshes.Remove(mesh);
}

const BoundingBox* Renderer::GetMeshBounds(MeshHandle mesh) const
{
	const MeshResource* resource = mImpl->meshes.Get(mesh);
	return resource ? &resource->bounds : nullptr;
}

TextureHandle Renderer::LoadTexture(const std::wstring& path, bool srgb)
{
	Impl& r = *mImpl;
	std::filesystem::path fullPath = path;
	if (fullPath.is_relative())
	{
		fullPath = ExecutableDir() / fullPath;
	}

	rhi::ComPtr<ID3D12Resource> resource;
	rhi::ComPtr<ID3D12Resource> uploadHeap;
	HRESULT hr = S_OK;
	r.device.ExecuteImmediate([&](ID3D12GraphicsCommandList4* cmd)
	{
		hr = DirectX::CreateDDSTextureFromFile12(r.device.D3D(), cmd, fullPath.c_str(), resource, uploadHeap);
	});
	if (FAILED(hr) || !resource)
	{
		CHERRY_ERROR("Failed to load texture %s (%s)", fullPath.string().c_str(), rhi::HResultToString(hr).c_str());
		return {};
	}

	const DXGI_FORMAT format = resource->GetDesc().Format;
	TextureResource texture;
	texture.texture = rhi::Texture::Wrap(r.device, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		srgb ? ToSrgb(format) : format, "LoadedTexture");
	return r.textures.Add(std::move(texture));
}

TextureHandle Renderer::CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba, bool srgb)
{
	Impl& r = *mImpl;
	if (width == 0 || height == 0 || !rgba) return {};

	// Mip chain by 2x2 box filtering (in the stored space; good enough for albedo maps).
	std::vector<std::vector<uint8_t>> levels;
	levels.emplace_back(rgba, rgba + static_cast<size_t>(width) * height * 4);
	uint32_t w = width, h = height;
	while (w > 1 || h > 1)
	{
		const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
		const std::vector<uint8_t>& src = levels.back();
		std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
		for (uint32_t y = 0; y < nh; ++y)
		for (uint32_t x = 0; x < nw; ++x)
		for (uint32_t c = 0; c < 4; ++c)
		{
			const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
			const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
			const uint32_t sum = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c] + src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
			dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
		}
		levels.push_back(std::move(dst));
		w = nw;
		h = nh;
	}

	rhi::TextureDesc desc;
	desc.width = width;
	desc.height = height;
	desc.mipLevels = static_cast<uint32_t>(levels.size());
	desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.srvFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.usage = rhi::TextureUsage_ShaderResource;
	desc.name = "CreatedTexture";

	std::vector<D3D12_SUBRESOURCE_DATA> data(levels.size());
	w = width;
	h = height;
	for (size_t i = 0; i < levels.size(); ++i)
	{
		data[i].pData = levels[i].data();
		data[i].RowPitch = static_cast<LONG_PTR>(w) * 4;
		data[i].SlicePitch = static_cast<LONG_PTR>(w) * 4 * h;
		w = std::max(w / 2, 1u);
		h = std::max(h / 2, 1u);
	}

	TextureResource texture;
	texture.texture = rhi::Texture::Create(r.device, desc);
	texture.texture->Upload(data.data(), static_cast<uint32_t>(data.size()), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return r.textures.Add(std::move(texture));
}

void Renderer::DestroyTexture(TextureHandle texture)
{
	mImpl->textures.Remove(texture);
}

MaterialHandle Renderer::CreateMaterial(const MaterialDesc& desc)
{
	return mImpl->materials.Add(MaterialDesc(desc));
}

void Renderer::SetMaterial(MaterialHandle material, const MaterialDesc& desc)
{
	if (MaterialDesc* existing = mImpl->materials.Get(material))
	{
		*existing = desc;
	}
}

const MaterialDesc* Renderer::GetMaterial(MaterialHandle material) const
{
	return mImpl->materials.Get(material);
}

void Renderer::DestroyMaterial(MaterialHandle material)
{
	if (material != mImpl->defaultMaterial)
	{
		mImpl->materials.Remove(material);
	}
}

SurfaceShaderHandle Renderer::CreateSurfaceShader(const std::string& name, const std::string& source, const SurfaceShaderOptions& options,
	std::function<void(bool)> done)
{
	Impl& r = *mImpl;
	SurfaceShaderResource shader;
	shader.name = name;
	shader.pipeline = "surface:" + std::to_string(r.nextSurfacePipeline++) + ":" + name;
	const SurfaceShaderHandle handle = r.surfaceShaders.Add(std::move(shader));
	SetSurfaceShaderSource(handle, source, options, std::move(done));
	return handle;
}

void Renderer::SetSurfaceShaderSource(SurfaceShaderHandle shader, const std::string& source, const SurfaceShaderOptions& options,
	std::function<void(bool)> done)
{
	Impl& r = *mImpl;
	SurfaceShaderResource* resource = r.surfaceShaders.Get(shader);
	if (!resource) return;
	resource->source = source;
	if (!resource->voxelPipeline.empty()) r.BuildVoxelVariant(*resource);   // a volume uses it
	rhi::GraphicsPipelineDesc desc = r.meshPipelineDesc;
	desc.shaderFile = resource->name;
	desc.source = WrapSurfaceSource(resource->name, source);
	if (options.translucent)
	{
		desc.alphaBlend = true;
		desc.depthWrite = options.depthWrite;
	}
	r.pipelines.SetGraphicsAsync(resource->pipeline, desc, [&r, shader, options, done = std::move(done)](bool ok)
	{
		// Options follow the source that's running: a failed build keeps the old pipeline.
		SurfaceShaderResource* built = r.surfaceShaders.Get(shader);
		if (built && (ok || !r.pipelines.Get(built->pipeline))) built->options = options;
		if (done) done(ok);
	});
}

bool Renderer::IsSurfaceShaderCompiling(SurfaceShaderHandle shader) const
{
	const SurfaceShaderResource* resource = mImpl->surfaceShaders.Get(shader);
	return resource && mImpl->pipelines.IsPending(resource->pipeline);
}

bool CompileSurfaceShaderOffline(const std::string& name, const std::string& source, const std::filesystem::path& shaderRoot, std::string& errors, bool voxel)
{
	ShaderCompiler compiler;
	compiler.Initialize();
	ShaderDesc desc;
	desc.filePath = shaderRoot / name;
	desc.source = WrapSurfaceSource(name, source, voxel);
	desc.includeDirs.push_back(shaderRoot);
	errors.clear();
	for (ShaderType type : { ShaderType::Vertex, ShaderType::Pixel })
	{
		desc.type = type;
		desc.entryPoint = type == ShaderType::Vertex ? "VSMain" : "PSMain";
		const CompiledShader result = compiler.Compile(desc);
		if (!result.success)
		{
			errors = std::string(type == ShaderType::Vertex ? "VSMain: " : "PSMain: ") + result.errors;
			return false;
		}
	}
	return true;
}

bool CompileShaderFileOffline(const std::string& file, const std::filesystem::path& shaderRoot, std::string& errors)
{
	ShaderCompiler compiler;
	compiler.Initialize();
	ShaderDesc desc;
	desc.filePath = shaderRoot / file;
	desc.includeDirs.push_back(shaderRoot);
	errors.clear();
	for (ShaderType type : { ShaderType::Vertex, ShaderType::Pixel })
	{
		desc.type = type;
		desc.entryPoint = type == ShaderType::Vertex ? "VSMain" : "PSMain";
		const CompiledShader result = compiler.Compile(desc);
		if (!result.success)
		{
			errors = std::string(type == ShaderType::Vertex ? "VSMain: " : "PSMain: ") + result.errors;
			return false;
		}
	}
	return true;
}

bool CompileComputeShaderOffline(const std::string& file, const std::string& entry, const std::filesystem::path& shaderRoot, std::string& errors)
{
	ShaderCompiler compiler;
	compiler.Initialize();
	ShaderDesc desc;
	desc.filePath = shaderRoot / file;
	desc.includeDirs.push_back(shaderRoot);
	desc.type = ShaderType::Compute;
	desc.entryPoint = entry;
	const CompiledShader result = compiler.Compile(desc);
	errors = result.success ? std::string() : entry + ": " + result.errors;
	return result.success;
}

void Renderer::DestroySurfaceShader(SurfaceShaderHandle shader)
{
	Impl& r = *mImpl;
	if (const SurfaceShaderResource* resource = r.surfaceShaders.Get(shader))
	{
		r.pipelines.Remove(resource->pipeline);
		if (!resource->voxelPipeline.empty()) r.pipelines.Remove(resource->voxelPipeline);
		r.surfaceShaders.Remove(shader);
	}
}

const std::string& Renderer::SurfaceShaderError(SurfaceShaderHandle shader) const
{
	const SurfaceShaderResource* resource = mImpl->surfaceShaders.Get(shader);
	if (!resource) return mImpl->emptyString;
	const std::string& error = mImpl->pipelines.Error(resource->pipeline);
	return error.empty() && !resource->voxelPipeline.empty() ? mImpl->pipelines.Error(resource->voxelPipeline) : error;
}

VoxelVolumeHandle Renderer::CreateVoxelVolume(const VoxelVolumeDesc& desc)
{
	Impl& r = *mImpl;
	const size_t voxelCount = static_cast<size_t>(desc.size.x) * desc.size.y * desc.size.z;
	if (voxelCount == 0 || desc.voxels.size() != voxelCount)
	{
		CHERRY_ERROR("CreateVoxelVolume: voxel data size does not match dimensions");
		return {};
	}

	VoxelResource volume;
	volume.size = desc.size;

	rhi::TextureDesc texDesc;
	texDesc.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	texDesc.width = desc.size.x;
	texDesc.height = desc.size.y;
	texDesc.depthOrArraySize = desc.size.z;
	texDesc.format = DXGI_FORMAT_R8_UINT;
	texDesc.usage = rhi::TextureUsage_ShaderResource;
	texDesc.name = "VoxelVolume";
	volume.volume = rhi::Texture::Create(r.device, texDesc);

	D3D12_SUBRESOURCE_DATA data{};
	data.pData = desc.voxels.data();
	data.RowPitch = desc.size.x;
	data.SlicePitch = static_cast<LONG_PTR>(desc.size.x) * desc.size.y;
	volume.volume->Upload(&data, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	if (const VoxelResource* source = r.voxelVolumes.Get(desc.sharePaletteFrom))
	{
		volume.palette = source->palette;
		return r.voxelVolumes.Add(std::move(volume));
	}

	// Palette is authored in sRGB; shading happens in linear space.
	auto toLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
	std::vector<XMFLOAT4> palette(256, XMFLOAT4(1, 0, 1, 1));
	for (size_t i = 0; i < std::min<size_t>(desc.palette.size(), 256); ++i)
	{
		const XMFLOAT4& c = desc.palette[i];
		palette[i] = { toLinear(c.x), toLinear(c.y), toLinear(c.z), c.w };
	}

	rhi::BufferDesc paletteDesc;
	paletteDesc.size = palette.size() * sizeof(XMFLOAT4);
	paletteDesc.stride = sizeof(XMFLOAT4);
	paletteDesc.createSrv = true;
	paletteDesc.name = "VoxelPalette";
	volume.palette = std::shared_ptr<rhi::Buffer>(rhi::Buffer::Create(r.device, paletteDesc));
	volume.palette->Upload(palette.data(), paletteDesc.size);

	return r.voxelVolumes.Add(std::move(volume));
}

bool Renderer::UpdateVoxelVolume(VoxelVolumeHandle volume, const XMUINT3& offset, const XMUINT3& size, const uint8_t* voxels)
{
	VoxelResource* resource = mImpl->voxelVolumes.Get(volume);
	if (!resource || !voxels) return false;
	const XMUINT3& dims = resource->size;
	if (offset.x + size.x > dims.x || offset.y + size.y > dims.y || offset.z + size.z > dims.z)
	{
		CHERRY_ERROR("UpdateVoxelVolume: region is outside the volume");
		return false;
	}
	resource->volume->UploadRegion(offset.x, offset.y, offset.z, size.x, size.y, size.z, voxels, 1);
	return true;
}

void Renderer::DestroyVoxelVolume(VoxelVolumeHandle volume)
{
	mImpl->voxelVolumes.Remove(volume);
}

LightVolumeHandle Renderer::CreateLightVolume(const XMUINT3& size, const uint8_t* levels)
{
	Impl& r = *mImpl;
	if (size.x == 0 || size.y == 0 || size.z == 0 || !levels)
	{
		CHERRY_ERROR("CreateLightVolume: empty volume");
		return {};
	}
	LightVolumeResource volume;
	volume.size = size;
	rhi::TextureDesc desc;
	desc.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	desc.width = size.x;
	desc.height = size.y;
	desc.depthOrArraySize = size.z;
	desc.format = DXGI_FORMAT_R8_UNORM;
	desc.usage = rhi::TextureUsage_ShaderResource;
	desc.name = "LightVolume";
	volume.texture = rhi::Texture::Create(r.device, desc);
	D3D12_SUBRESOURCE_DATA data{};
	data.pData = levels;
	data.RowPitch = size.x;
	data.SlicePitch = static_cast<LONG_PTR>(size.x) * size.y;
	volume.texture->Upload(&data, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	return r.lightVolumes.Add(std::move(volume));
}

bool Renderer::UpdateLightVolume(LightVolumeHandle volume, const XMUINT3& offset, const XMUINT3& size, const uint8_t* levels)
{
	LightVolumeResource* resource = mImpl->lightVolumes.Get(volume);
	if (!resource || !levels) return false;
	const XMUINT3& dims = resource->size;
	if (offset.x + size.x > dims.x || offset.y + size.y > dims.y || offset.z + size.z > dims.z)
	{
		CHERRY_ERROR("UpdateLightVolume: region is outside the volume");
		return false;
	}
	resource->texture->UploadRegion(offset.x, offset.y, offset.z, size.x, size.y, size.z, levels, 1);
	return true;
}

void Renderer::DestroyLightVolume(LightVolumeHandle volume)
{
	mImpl->lightVolumes.Remove(volume);
}

XMUINT3 Renderer::GetVoxelVolumeSize(VoxelVolumeHandle volume) const
{
	const VoxelResource* resource = mImpl->voxelVolumes.Get(volume);
	return resource ? resource->size : XMUINT3(0, 0, 0);
}

RenderSettings& Renderer::Settings() { return mImpl->settings; }
std::vector<ParticleInstance>& Renderer::Particles() { return mImpl->particles; }
DebugDraw& Renderer::Debug() { return mImpl->debugDraw; }
const RenderStats& Renderer::Stats() const { return mImpl->stats; }
const std::string& Renderer::AdapterName() const { return mImpl->device.Caps().adapterName; }
void Renderer::ReloadShaders() { mImpl->pipelines.ReloadAll(); }
const std::string& Renderer::ShaderError() const { return mImpl->pipelines.LastError(); }

// =============================================================================
// Frame
// =============================================================================

void Renderer::OnWindowResized(uint32_t width, uint32_t height)
{
	mImpl->pendingWindowWidth = width;
	mImpl->pendingWindowHeight = height;
}

void Renderer::SetViewSize(uint32_t width, uint32_t height)
{
	mImpl->requestedViewWidth = std::max(width, 1u);
	mImpl->requestedViewHeight = std::max(height, 1u);
}

uint32_t Renderer::ViewWidth() const { return mImpl->viewWidth; }
uint32_t Renderer::ViewHeight() const { return mImpl->viewHeight; }

unsigned long long Renderer::ViewTextureId() const
{
	return mImpl->ldr ? static_cast<unsigned long long>(mImpl->ldr->SrvGpu().ptr) : 0;
}

unsigned long long Renderer::RenderPreview(MeshHandle meshHandle, MaterialHandle materialHandle, uint32_t size, float yaw, float pitch)
{
	Impl& r = *mImpl;
	const MeshResource* mesh = r.meshes.Get(meshHandle);
	if (!r.frameActive || !mesh) return 0;
	size = std::clamp(size, 16u, 1024u);
	if (r.previewSize != size) r.CreatePreviewTargets(size);   // old targets are released once unused
	ID3D12GraphicsCommandList4* cmd = r.device.CmdList();
	const uint32_t frameIndex = r.device.FrameIndex();

	// Camera: orbits the mesh's bounds, framing them.
	const float fovY = XMConvertToRadians(35.0f);
	const XMVECTOR center = XMLoadFloat3(&mesh->bounds.Center);
	const float radius = std::max(XMVectorGetX(XMVector3Length(XMLoadFloat3(&mesh->bounds.Extents))), 0.01f);
	const float distance = radius / std::sin(fovY * 0.5f) * 1.05f;
	const XMVECTOR offset = XMVectorSet(std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw), 0.0f);
	const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(offset, distance));
	const XMMATRIX view = XMMatrixLookAtLH(eye, center, XMVectorSet(0, 1, 0, 0));
	const XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, 1.0f, std::max(distance - radius * 2.0f, distance * 0.01f), distance + radius * 2.0f);
	const XMMATRIX viewProj = view * proj;

	// Per-frame data in the preview's own buffers.
	r.BuildMaterialScratch();
	gpu::FrameConstants frame = r.BuildFrameConstants();
	frame.gradeEnabled = 0;   // previews show the material as it is
	frame.numPointLights = 0;
	frame.fogStart = frame.fogEnd = 0.0f;
	frame.shadowEnabled = 0;
	frame.materialBufferIndex = r.previewMaterialBuffer.Write(r.device, frameIndex, r.materialScratch.data(), static_cast<uint32_t>(r.materialScratch.size()));
	frame.materialParamBufferIndex = r.previewParamBuffer.Write(r.device, frameIndex, r.materialParamScratch.data(), static_cast<uint32_t>(r.materialParamScratch.size()));
	const MaterialHandle material = r.materials.IsValid(materialHandle) ? materialHandle : r.defaultMaterial;
	gpu::InstanceData instance{};
	XMStoreFloat4x4(&instance.world, XMMatrixIdentity());
	XMStoreFloat4x4(&instance.normalMatrix, XMMatrixIdentity());
	instance.materialIndex = material.index;
	frame.instanceBufferIndex = r.previewInstanceBuffer.Write(r.device, frameIndex, &instance, 1);

	gpu::ViewConstants viewConstants{};
	XMStoreFloat4x4(&viewConstants.view, view);
	XMStoreFloat4x4(&viewConstants.invView, XMMatrixInverse(nullptr, view));
	XMStoreFloat4x4(&viewConstants.proj, proj);
	XMStoreFloat4x4(&viewConstants.invProj, XMMatrixInverse(nullptr, proj));
	XMStoreFloat4x4(&viewConstants.viewProj, viewProj);
	XMStoreFloat4x4(&viewConstants.invViewProj, XMMatrixInverse(nullptr, viewProj));
	XMStoreFloat3(&viewConstants.eyePosW, eye);
	viewConstants.renderTargetSize = { static_cast<float>(size), static_cast<float>(size) };
	viewConstants.invRenderTargetSize = { 1.0f / size, 1.0f / size };
	viewConstants.exposure = r.settings.exposure;
	const D3D12_GPU_VIRTUAL_ADDRESS frameCb = r.uploadRing.PushConstants(frame);
	const D3D12_GPU_VIRTUAL_ADDRESS viewCb = r.uploadRing.PushConstants(viewConstants);
	cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_ViewConstants, viewCb);
	cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_FrameConstants, frameCb);
	cmd->SetComputeRootConstantBufferView(rhi::RootSlot_ViewConstants, viewCb);
	cmd->SetComputeRootConstantBufferView(rhi::RootSlot_FrameConstants, frameCb);

	// Draw.
	r.previewHdr->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
	r.previewDepth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const float background[4] = { 0.08f, 0.08f, 0.09f, 1.0f };
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = r.previewHdr->Rtv();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = r.previewDepth->Dsv();
	cmd->ClearRenderTargetView(rtv, background, 0, nullptr);
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	r.SetViewport(cmd, size, size);

	const MaterialDesc* desc = r.materials.Get(material);
	const uint32_t pipeline = desc && r.surfaceShaders.IsValid(desc->shader) ? desc->shader.index + 1 : 0;
	cmd->SetPipelineState(r.SurfacePipeline(pipeline));
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const D3D12_VERTEX_BUFFER_VIEW vbv = mesh->vertexBuffer->VertexView(sizeof(Vertex));
	const D3D12_INDEX_BUFFER_VIEW ibv = mesh->indexBuffer->IndexView(DXGI_FORMAT_R32_UINT);
	cmd->IASetVertexBuffers(0, 1, &vbv);
	cmd->IASetIndexBuffer(&ibv);
	gpu::MeshDrawConstants constants{ 0 };
	cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
	cmd->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);

	// Tonemap.
	r.previewHdr->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	r.previewLdr->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetPipelineState(r.pipelines.Get("tonemap"));
	gpu::TonemapConstants tonemap{ r.previewHdr->SrvIndex(), r.previewLdr->UavIndex(), size, size, CHERRY_INVALID_INDEX, 0.0f, CHERRY_INVALID_INDEX, CHERRY_INVALID_INDEX, 0.0f, CHERRY_INVALID_INDEX, CHERRY_INVALID_INDEX, CHERRY_INVALID_INDEX, 0.0f };
	cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(tonemap) / 4, &tonemap, 0);
	cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
	r.previewLdr->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return static_cast<unsigned long long>(r.previewLdr->SrvGpu().ptr);
}

void Renderer::Impl::SetViewport(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height)
{
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	cmd->RSSetViewports(1, &viewport);
	cmd->RSSetScissorRects(1, &scissor);
}

void Renderer::Impl::BindGlobals(ID3D12GraphicsCommandList4* cmd)
{
	// Directly-indexed heaps must be bound before the root signature.
	ID3D12DescriptorHeap* heaps[] = { device.SrvHeap().Heap() };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetGraphicsRootSignature(rootSignature.Get());
	cmd->SetComputeRootSignature(rootSignature.Get());
}

void Renderer::BeginFrame(float totalTime, float deltaTime)
{
	Impl& r = *mImpl;
	r.totalTime = totalTime;
	r.deltaTime = deltaTime;

	if (r.pendingWindowWidth > 0 && r.pendingWindowHeight > 0)
	{
		r.swapChain.Resize(r.pendingWindowWidth, r.pendingWindowHeight);
		r.pendingWindowWidth = r.pendingWindowHeight = 0;
	}

	if (r.pipelines.PollHotReload())
	{
		r.stats.shaderReloads = r.pipelines.ReloadCount();
	}
	r.pipelines.PumpAsync();   // background pipeline builds that finished

	r.device.BeginFrame();
	r.gpuTimer.Collect(r.device.FrameIndex(), r.stats);   // this slot's previous frame is complete now
	r.uploadRing.BeginFrame(r.device.FrameIndex());

	if (r.requestedViewWidth != r.viewWidth || r.requestedViewHeight != r.viewHeight)
	{
		// Old targets are released once in-flight frames no longer use them.
		r.CreateViewTargets(r.requestedViewWidth, r.requestedViewHeight);
	}

	r.BindGlobals(r.device.CmdList());
	r.gpuTimer.Mark(r.device.CmdList(), r.device.FrameIndex(), Mark_FrameBegin);
	r.viewRendered = false;
	r.frameActive = true;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

gpu::FrameConstants Renderer::Impl::BuildFrameConstants(const XMFLOAT3* eye)
{
	gpu::FrameConstants frame{};
	frame.time = totalTime;
	frame.deltaTime = deltaTime;
	frame.frameNumber = static_cast<uint32_t>(device.FrameNumber());
	const float up = 1.0f + std::clamp(settings.ambientContrast, 0.0f, 1.0f);
	const float down = 1.0f - std::clamp(settings.ambientContrast, 0.0f, 1.0f);
	frame.ambientLight = { settings.ambient.x * up, settings.ambient.y * up, settings.ambient.z * up, 1.0f };
	frame.ambientGround = { settings.ambient.x * down, settings.ambient.y * down, settings.ambient.z * down, 1.0f };

	XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(&settings.sunDirection));
	XMStoreFloat3(&frame.lights[0].Direction, sun);
	frame.lights[0].Strength = settings.sunColor;
	// Two fixed fill lights keep unlit sides readable (scaled by fillLightScale).
	const float fill = std::max(settings.fillLightScale, 0.0f);
	XMStoreFloat3(&frame.lights[1].Direction, XMVector3Normalize(XMVectorSet(-0.57735f, -0.57735f, 0.57735f, 0)));
	frame.lights[1].Strength = { 0.35f * fill, 0.35f * fill, 0.4f * fill };
	XMStoreFloat3(&frame.lights[2].Direction, XMVector3Normalize(XMVectorSet(0.0f, 0.707f, -0.707f, 0)));
	frame.lights[2].Strength = { 0.2f * fill, 0.2f * fill, 0.22f * fill };
	frame.numDirLights = 3;

	// The game's lights and the scene's PointLight components; past CHERRY_MAX_POINT_LIGHTS,
	// the ones that matter most at the camera.
	std::vector<PointLight>& all = pointLightScratch;
	all.assign(settings.pointLights.begin(), settings.pointLights.end());
	all.insert(all.end(), sceneLights.begin(), sceneLights.end());
	std::vector<uint32_t> chosen;
	if (all.size() > CHERRY_MAX_POINT_LIGHTS && eye) chosen = SelectPointLights(all, *eye, CHERRY_MAX_POINT_LIGHTS);
	else for (uint32_t i = 0; i < std::min<uint32_t>(static_cast<uint32_t>(all.size()), CHERRY_MAX_POINT_LIGHTS); ++i) chosen.push_back(i);
	const uint32_t pointCount = static_cast<uint32_t>(chosen.size());
	for (uint32_t i = 0; i < pointCount; ++i)
	{
		const PointLight& light = all[chosen[i]];
		gpu::Light& out = frame.pointLights[i];
		out.Position = light.position;
		out.Strength = { light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity };
		out.FalloffStart = 0.0f;
		out.FalloffEnd = std::max(light.range, 0.01f);
	}
	frame.numPointLights = pointCount;

	if (settings.fogEnabled)
	{
		frame.fogColor = settings.fogColor;
		frame.fogStart = settings.fogStart;
		frame.fogEnd = settings.fogEnd;
	}
	else
	{
		frame.fogStart = frame.fogEnd = 0.0f;   // disabled
	}

	for (uint32_t i = 0; i < CHERRY_SHADER_GLOBALS; ++i) frame.globals[i] = settings.shaderGlobals[i];

	const InteriorSettings& interior = settings.interior;
	frame.interiorEnabled = interior.enabled ? 1u : 0u;
	frame.interiorMin = interior.boxMin;
	frame.interiorMax = interior.boxMax;
	frame.interiorFadeStart = std::max(interior.fadeStart, 0.0f);
	frame.interiorFadeLength = std::max(interior.fadeLength, 1e-3f);
	frame.interiorAmbientSky = { interior.ambient.x * up, interior.ambient.y * up, interior.ambient.z * up };
	frame.interiorAmbientGround = { interior.ambient.x * down, interior.ambient.y * down, interior.ambient.z * down };
	frame.interiorFogColor = interior.fogColor;
	frame.interiorFogStart = interior.fogStart;
	frame.interiorFogEnd = interior.fogEnd;
	const LightVolumeResource* lightVolume = lightVolumes.Get(interior.lightVolume);
	frame.interiorLightVolume = lightVolume ? lightVolume->texture->SrvIndex() : CHERRY_INVALID_INDEX;
	frame.interiorLightStrength = interior.lightStrength;
	frame.interiorSunFromLight = interior.sunFromLight;

	const CloudSettings& clouds = settings.clouds;
	frame.cloudsEnabled = clouds.enabled ? 1u : 0u;
	frame.cloudOffset = { -(clouds.wind.x * totalTime + clouds.drift.x), -(clouds.wind.y * totalTime + clouds.drift.y) };
	frame.cloudBlur = std::clamp(clouds.blur, 0.0f, 1.0f);

	const HeightFogSettings& heightFog = settings.heightFog;
	frame.fogHeightEnabled = heightFog.enabled && settings.fogEnabled ? 1u : 0u;
	frame.fogDensity = std::max(heightFog.density, 0.0f);
	frame.fogBaseHeight = heightFog.baseHeight;
	frame.fogFalloff = std::max(heightFog.falloff, 1e-4f);
	frame.fogSunColor = heightFog.sunColor;
	frame.fogSunPower = std::max(heightFog.sunPower, 1.0f);

	const ColorGradeSettings& grade = settings.colorGrade;
	frame.gradeEnabled = grade.enabled ? 1u : 0u;
	frame.gradeTint = grade.tint;
	frame.gradeSaturation = std::max(grade.saturation, 0.0f);
	frame.gradeLift = grade.lift;
	frame.gradeContrast = std::max(grade.contrast, 0.0f);
	frame.gradeGain = grade.gain;
	frame.gradeVignette = std::clamp(grade.vignette, 0.0f, 1.0f);
	frame.cloudScale = std::max(clouds.scale, 1.0f);
	frame.cloudCoverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
	frame.cloudSoftness = std::max(clouds.softness, 0.001f);
	frame.cloudDarkness = std::clamp(clouds.darkness, 0.0f, 1.0f);
	frame.cloudHeight = clouds.height;

	frame.roomCount = static_cast<uint32_t>(std::min<size_t>(settings.rooms.size(), CHERRY_MAX_ROOMS));
	for (uint32_t i = 0; i < frame.roomCount; ++i)
	{
		const RoomSettings& room = settings.rooms[i];
		gpu::RoomBox& box = frame.rooms[i];
		box.center = room.center;
		box.fade = std::max(room.fade, 0.01f);
		box.halfExtents = room.halfExtents;
		box.ambientScale = room.ambientScale;
		box.axes = { room.axisX.x, room.axisX.z, room.axisZ.x, room.axisZ.z };
	}

	const SkySettings& sky = settings.sky;
	frame.skyEnabled = sky.enabled ? 1u : 0u;
	frame.skyZenith = sky.zenith;
	frame.skyHorizon = sky.horizon;
	frame.skyGround = sky.ground;
	frame.skyGradientPower = sky.gradientPower;
	frame.sunCosRadius = sky.sunSize > 0.0f ? std::cos(XMConvertToRadians(std::min(sky.sunSize, 20.0f))) : 1.0f;
	frame.sunDiskColor = { settings.sunColor.x * sky.sunIntensity, settings.sunColor.y * sky.sunIntensity, settings.sunColor.z * sky.sunIntensity };
	frame.starIntensity = std::max(sky.starIntensity, 0.0f);
	return frame;
}

std::vector<uint32_t> SelectPointLights(const std::vector<PointLight>& lights, const XMFLOAT3& eye, uint32_t maxLights)
{
	std::vector<uint32_t> order(lights.size());
	for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
	if (order.size() <= maxLights) return order;
	// How far the camera is outside each light's reach (negative: inside it).
	std::vector<float> score(lights.size());
	for (size_t i = 0; i < lights.size(); ++i)
	{
		const XMFLOAT3& p = lights[i].position;
		const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
		score[i] = std::sqrt(dx * dx + dy * dy + dz * dz) - std::max(lights[i].range, 0.0f);
	}
	std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return score[a] < score[b]; });
	order.resize(maxLights);
	std::sort(order.begin(), order.end());
	return order;
}

void Renderer::RenderScene(Scene& scene, const RenderView& renderView)
{
	Impl& r = *mImpl;
	ID3D12GraphicsCommandList4* cmd = r.device.CmdList();
	const uint32_t frameIndex = r.device.FrameIndex();
	RenderStats& stats = r.stats;
	stats.drawCalls = stats.instancesSubmitted = stats.instancesCulled = stats.voxelVolumes = 0;

	const XMMATRIX view = XMLoadFloat4x4(&renderView.view);
	const XMMATRIX proj = XMLoadFloat4x4(&renderView.proj);
	const XMMATRIX viewProj = view * proj;
	const XMMATRIX invView = XMMatrixInverse(nullptr, view);

	BoundingFrustum frustum(proj);
	frustum.Transform(frustum, invView);

	r.BuildMaterialScratch();

	// ---- Sun shadow cascades ------------------------------------------------
	const XMFLOAT3& sunColor = r.settings.sunColor;
	const bool shadows = r.settings.shadows.enabled && sunColor.x + sunColor.y + sunColor.z > 0.0f;
	if (shadows)
	{
		// A fade between shadow steps: when the shadows' direction changes, last frame's maps
		// become the previous ones (fading out) and the other set is drawn along the new one.
		const XMFLOAT3& own = r.settings.shadows.direction;
		const XMFLOAT3 direction = (own.x != 0.0f || own.y != 0.0f || own.z != 0.0f) ? own : r.settings.sunDirection;
		const float fadeSeconds = r.settings.shadows.fadeSeconds;
		const bool moved = direction.x != r.shadowDirectionDrawn.x || direction.y != r.shadowDirectionDrawn.y || direction.z != r.shadowDirectionDrawn.z;
		const bool drawnBefore = r.shadowDirectionDrawn.x != 0.0f || r.shadowDirectionDrawn.y != 0.0f || r.shadowDirectionDrawn.z != 0.0f;
		if (fadeSeconds <= 0.0f)
			r.shadowFade = 0.0f;
		else if (moved && drawnBefore)
		{
			if (!r.previousShadowMaps[0])
				for (auto& map : r.previousShadowMaps) map = r.CreateShadowMap();
			for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c)
			{
				std::swap(r.shadowMaps[c], r.previousShadowMaps[c]);
				r.previousCascades[c] = r.cascades[c];
			}
			r.shadowFade = 1.0f;
		}
		else
			r.shadowFade = std::max(0.0f, r.shadowFade - r.deltaTime / fadeSeconds);
		r.shadowDirectionDrawn = direction;
		r.SetupShadowCascades(view, proj, renderView);
	}
	else
	{
		r.shadowDirectionDrawn = { 0, 0, 0 };
		r.shadowFade = 0.0f;
	}
	for (auto& list : r.shadowScratch) list.clear();
	r.shadowCasterScratch.clear();

	// ---- Mesh instances (CPU frustum culling) ------------------------------
	r.instanceScratch.clear();
	r.drawScratch.clear();
	const uint32_t boundsColor = DebugDraw::Color(0.2f, 1.0f, 0.3f);
	// One part of a mesh entity (its mesh, or a voxel model's glass part): a shadow caster,
	// and a draw unless culled.
	auto submit = [&](const MeshRenderer& renderer, const XMMATRIX& world, MeshHandle meshHandle, MaterialHandle requestedMaterial)
	{
		const MeshResource* mesh = r.meshes.Get(meshHandle);
		if (!mesh) return;

		const bool viewModel = renderer.layer == RenderLayer::ViewModel;
		const MaterialHandle materialHandle = r.materials.IsValid(requestedMaterial) ? requestedMaterial : r.defaultMaterial;
		const MaterialDesc* material = r.materials.Get(materialHandle);
		const SurfaceShaderResource* surface = material ? r.surfaceShaders.Get(material->shader) : nullptr;
		BoundingBox worldBounds;
		mesh->bounds.Transform(worldBounds, world);
		if (surface && surface->options.boundsPadding > 0.0f)
		{
			const float pad = surface->options.boundsPadding;
			worldBounds.Extents = { worldBounds.Extents.x + pad, worldBounds.Extents.y + pad, worldBounds.Extents.z + pad };
		}
		const bool castsShadow = shadows && !viewModel && !(surface && surface->options.translucent);
		if (castsShadow)
		{
			// Shadow casters are found whether or not the camera sees them.
			uint32_t caster = UINT32_MAX;
			for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c)
			{
				if (!r.cascades[c].box.Intersects(worldBounds)) continue;
				if (caster == UINT32_MAX)
				{
					caster = static_cast<uint32_t>(r.shadowCasterScratch.size());
					gpu::InstanceData shadowInstance{};
					XMStoreFloat4x4(&shadowInstance.world, world);
					r.shadowCasterScratch.push_back(shadowInstance);
				}
				r.shadowScratch[c].push_back({ meshHandle.index, caster });
			}
		}
		if (!viewModel && r.settings.cullingEnabled && !frustum.Intersects(worldBounds))
		{
			++stats.instancesCulled;
			return;
		}
		if (!viewModel && r.settings.drawBounds)
		{
			r.debugDraw.Box(worldBounds, boundsColor);
		}

		gpu::InstanceData instance{};
		XMStoreFloat4x4(&instance.world, world);
		XMStoreFloat4x4(&instance.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
		instance.materialIndex = materialHandle.index;
		const uint32_t pipeline = surface ? material->shader.index + 1 : 0;
		const uint32_t translucent = surface && surface->options.translucent ? 1u : 0u;
		const XMVECTOR toCamera = XMVectorSubtract(XMLoadFloat3(&worldBounds.Center), XMLoadFloat3(&renderView.position));

		r.drawScratch.push_back({ static_cast<uint32_t>(renderer.layer), translucent, pipeline, meshHandle.index,
			static_cast<uint32_t>(r.instanceScratch.size()), XMVectorGetX(XMVector3LengthSq(toCamera)) });
		r.instanceScratch.push_back(instance);
	};
	for (Entity entity : scene.MeshEntities())
	{
		const MeshRenderer& renderer = scene.Get<MeshRenderer>(entity);
		if (!renderer.visible) continue;
		const XMMATRIX world = scene.WorldMatrix(entity);
		submit(renderer, world, renderer.mesh, renderer.material);
		if (renderer.glassMesh.IsValid()) submit(renderer, world, renderer.glassMesh, renderer.glassMaterial);
	}
	// Layer first (world before view models), opaque before translucent; opaque by pipeline
	// (surface shader), then by mesh so pipelines and vertex/index buffers change rarely;
	// translucent back to front so blending comes out right.
	std::sort(r.drawScratch.begin(), r.drawScratch.end(), [](const DrawItem& a, const DrawItem& b)
	{
		if (a.layer != b.layer) return a.layer < b.layer;
		if (a.translucent != b.translucent) return a.translucent < b.translucent;
		if (a.translucent) return a.distanceSq > b.distanceSq;
		if (a.pipeline != b.pipeline) return a.pipeline < b.pipeline;
		return a.meshSlot < b.meshSlot;
	});
	// Instances in draw order, so a run of draws with the same pipeline and mesh becomes one
	// instanced draw over consecutive entries.
	r.sortedInstanceScratch.clear();
	for (DrawItem& item : r.drawScratch)
	{
		const uint32_t sorted = static_cast<uint32_t>(r.sortedInstanceScratch.size());
		r.sortedInstanceScratch.push_back(r.instanceScratch[item.instanceIndex]);
		item.instanceIndex = sorted;
	}
	std::swap(r.instanceScratch, r.sortedInstanceScratch);
	// Shadow casters by mesh (instanced), cascade after cascade in one buffer.
	r.shadowInstanceScratch.clear();
	for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c)
	{
		std::sort(r.shadowScratch[c].begin(), r.shadowScratch[c].end(), [](const auto& a, const auto& b) { return a.meshSlot < b.meshSlot; });
		for (auto& item : r.shadowScratch[c])
		{
			const uint32_t sorted = static_cast<uint32_t>(r.shadowInstanceScratch.size());
			r.shadowInstanceScratch.push_back(r.shadowCasterScratch[item.instance]);
			item.instance = sorted;
		}
	}
	const auto firstViewModel = std::find_if(r.drawScratch.begin(), r.drawScratch.end(),
		[](const DrawItem& item) { return item.layer == static_cast<uint32_t>(RenderLayer::ViewModel); });
	const auto firstWorldTranslucent = std::find_if(r.drawScratch.begin(), firstViewModel,
		[](const DrawItem& item) { return item.translucent != 0; });

	// ---- Voxel volumes ------------------------------------------------------
	r.voxelScratch.clear();
	r.voxelDrawScratch.clear();
	r.shadowVoxelScratch.clear();
	for (Entity entity : scene.VoxelEntities())
	{
		const VoxelVolumeRenderer& renderer = scene.Get<VoxelVolumeRenderer>(entity);
		if (!renderer.visible) continue;
		const VoxelResource* volume = r.voxelVolumes.Get(renderer.volume);
		if (!volume) continue;

		const XMMATRIX world = XMMatrixScaling(renderer.voxelSize, renderer.voxelSize, renderer.voxelSize) * scene.WorldMatrix(entity);
		BoundingOrientedBox box;
		BoundingOrientedBox::CreateFromBoundingBox(box, BoundingBox(
			XMFLOAT3(volume->size.x * 0.5f, volume->size.y * 0.5f, volume->size.z * 0.5f),
			XMFLOAT3(volume->size.x * 0.5f, volume->size.y * 0.5f, volume->size.z * 0.5f)));
		box.Transform(box, world);
		const bool seen = !r.settings.cullingEnabled || frustum.Intersects(box);
		bool castsShadow = false;
		for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES && shadows; ++c) castsShadow |= r.cascades[c].box.Intersects(box);
		if (!seen && !castsShadow) continue;
		if (seen && r.settings.drawBounds)
		{
			r.debugDraw.Box(box, DebugDraw::Color(1.0f, 0.85f, 0.2f));
		}

		gpu::VoxelVolumeData data{};
		XMStoreFloat4x4(&data.world, world);
		XMStoreFloat4x4(&data.invWorld, XMMatrixInverse(nullptr, world));
		data.dims = volume->size;
		data.volumeTextureIndex = volume->volume->SrvIndex();
		data.paletteBufferIndex = volume->palette->SrvIndex();
		data.colorJitter = renderer.colorJitter;
		data.aoStrength = renderer.aoStrength;
		data.jitterOffset = renderer.jitterOffset;
		const MaterialHandle material = r.materials.IsValid(renderer.material) ? renderer.material : r.defaultMaterial;
		data.materialIndex = material.index;
		const MaterialDesc* materialDesc = r.materials.Get(material);
		if (castsShadow) r.shadowVoxelScratch.push_back(data);
		if (!seen) continue;
		r.voxelDrawScratch.push_back(materialDesc && r.surfaceShaders.IsValid(materialDesc->shader) ? materialDesc->shader.index + 1 : 0);
		r.voxelScratch.push_back(data);
	}

	// ---- Scene lights ---------------------------------------------------------
	r.sceneLights.clear();
	for (Entity entity : scene.PointLightEntities())
	{
		const PointLightComponent& component = scene.Get<PointLightComponent>(entity);
		PointLight light;
		light.position = scene.WorldPosition(entity);
		light.color = component.color;
		light.intensity = component.intensity;
		light.range = component.range;
		r.sceneLights.push_back(light);
	}

	// ---- Upload per-frame data ---------------------------------------------
	gpu::FrameConstants frame = r.BuildFrameConstants(&renderView.position);
	frame.materialBufferIndex = r.materialBuffer.Write(r.device, frameIndex, r.materialScratch.data(), static_cast<uint32_t>(r.materialScratch.size()));
	frame.materialParamBufferIndex = r.materialParamBuffer.Write(r.device, frameIndex, r.materialParamScratch.data(), static_cast<uint32_t>(r.materialParamScratch.size()));
	frame.instanceBufferIndex = r.instanceBuffer.Write(r.device, frameIndex, r.instanceScratch.data(), static_cast<uint32_t>(r.instanceScratch.size()));
	frame.voxelVolumeBufferIndex = r.voxelBuffer.Write(r.device, frameIndex, r.voxelScratch.data(), static_cast<uint32_t>(r.voxelScratch.size()));
	const auto& lines = r.debugDraw.Vertices();
	frame.debugVertexBufferIndex = r.debugVertexBuffer.Write(r.device, frameIndex, lines.data(), static_cast<uint32_t>(lines.size()));

	// Particles: covering ones back to front, then the additive ones (order doesn't matter).
	r.particleScratch.clear();
	uint32_t coveringParticles = 0;
	{
		std::vector<ParticleInstance>& list = r.particles;
		const XMVECTOR eye = XMLoadFloat3(&renderView.position);
		auto distance = [&](const ParticleInstance& p) { return XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(XMLoadFloat3(&p.position), eye))); };
		auto firstAdditive = std::partition(list.begin(), list.end(), [](const ParticleInstance& p) { return !p.additive; });
		std::sort(list.begin(), firstAdditive, [&](const ParticleInstance& a, const ParticleInstance& b) { return distance(a) > distance(b); });
		coveringParticles = static_cast<uint32_t>(firstAdditive - list.begin());
		for (const ParticleInstance& p : list)
		{
			gpu::ParticleData data{};
			data.position = p.position;
			data.size = p.size;
			data.color = p.color;
			data.emissive = p.emissive;
			data.rotation = p.rotation;
			data.flags = p.square ? 1u : 0u;
			data.stretch = p.stretch;
			data.velocity = p.velocity;
			r.particleScratch.push_back(data);
		}
		list.clear();
	}
	const uint32_t particleCount = static_cast<uint32_t>(r.particleScratch.size());
	frame.particleBufferIndex = r.particleBuffer.Write(r.device, frameIndex, r.particleScratch.data(), particleCount);

	// ---- Shadow maps (before the mesh pass, which reads them) -------------------
	if (shadows)
	{
		gpu::FrameConstants shadowFrame = frame;
		shadowFrame.instanceBufferIndex = r.shadowInstanceBuffer.Write(r.device, frameIndex, r.shadowInstanceScratch.data(), static_cast<uint32_t>(r.shadowInstanceScratch.size()));
		shadowFrame.voxelVolumeBufferIndex = r.shadowVoxelBuffer.Write(r.device, frameIndex, r.shadowVoxelScratch.data(), static_cast<uint32_t>(r.shadowVoxelScratch.size()));
		cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_FrameConstants, r.uploadRing.PushConstants(shadowFrame));
		cmd->SetPipelineState(r.pipelines.Get("shadow"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c)
		{
			gpu::ViewConstants lightView{};
			lightView.viewProj = r.cascades[c].viewProj;
			lightView.eyePosW = renderView.position;
			cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_ViewConstants, r.uploadRing.PushConstants(lightView));
			rhi::Texture& map = *r.shadowMaps[c];
			map.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			const D3D12_CPU_DESCRIPTOR_HANDLE mapDsv = map.Dsv();
			cmd->ClearDepthStencilView(mapDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			cmd->OMSetRenderTargets(0, nullptr, FALSE, &mapDsv);
			r.SetViewport(cmd, Impl::kShadowMapSize, Impl::kShadowMapSize);
			const auto& items = r.shadowScratch[c];
			for (size_t i = 0; i < items.size(); )
			{
				size_t end = i + 1;
				while (end < items.size() && items[end].meshSlot == items[i].meshSlot) ++end;
				const MeshResource* mesh = r.meshes.AtSlot(items[i].meshSlot);
				const D3D12_VERTEX_BUFFER_VIEW vbv = mesh->vertexBuffer->VertexView(sizeof(Vertex));
				const D3D12_INDEX_BUFFER_VIEW ibv = mesh->indexBuffer->IndexView(DXGI_FORMAT_R32_UINT);
				cmd->IASetVertexBuffers(0, 1, &vbv);
				cmd->IASetIndexBuffer(&ibv);
				gpu::MeshDrawConstants constants{ items[i].instance };
				cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
				cmd->DrawIndexedInstanced(mesh->indexCount, static_cast<UINT>(end - i), 0, 0, 0);
				++stats.drawCalls;
				i = end;
			}
			// Voxel volumes, ray-marched along the sunlight.
			if (!r.shadowVoxelScratch.empty())
			{
				cmd->SetPipelineState(r.pipelines.Get("voxel_shadow"));
				for (uint32_t v = 0; v < r.shadowVoxelScratch.size(); ++v)
				{
					gpu::VoxelDrawConstants constants{ v };
					cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
					cmd->DrawInstanced(36, 1, 0, 0);
					++stats.drawCalls;
				}
				cmd->SetPipelineState(r.pipelines.Get("shadow"));
			}
			map.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		frame.shadowEnabled = 1;
		frame.shadowMap0 = r.shadowMaps[0]->SrvIndex();
		frame.shadowMap1 = r.shadowMaps[1]->SrvIndex();
		for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c) frame.shadowMatrix[c] = r.cascades[c].uv;
		frame.shadowSplits = { r.cascades[0].split, r.cascades[1].split, r.cascades[1].split * 0.15f, 0.0f };
		frame.shadowTexelWorld = { r.cascades[0].texelWorld, r.cascades[1].texelWorld, 0.0f, 0.0f };
		frame.shadowNormalBias = r.settings.shadows.normalBias;
		frame.shadowFade = r.shadowFade > 0.0f && r.previousShadowMaps[0] ? r.shadowFade : 0.0f;
		if (frame.shadowFade > 0.0f)
		{
			for (auto& map : r.previousShadowMaps) map->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			frame.shadowMapPrevious0 = r.previousShadowMaps[0]->SrvIndex();
			frame.shadowMapPrevious1 = r.previousShadowMaps[1]->SrvIndex();
			for (uint32_t c = 0; c < CHERRY_SHADOW_CASCADES; ++c) frame.shadowMatrixPrevious[c] = r.previousCascades[c].uv;
		}
	}

	gpu::ViewConstants viewConstants{};
	XMStoreFloat4x4(&viewConstants.view, view);
	XMStoreFloat4x4(&viewConstants.invView, invView);
	XMStoreFloat4x4(&viewConstants.proj, proj);
	XMStoreFloat4x4(&viewConstants.invProj, XMMatrixInverse(nullptr, proj));
	XMStoreFloat4x4(&viewConstants.viewProj, viewProj);
	XMStoreFloat4x4(&viewConstants.invViewProj, XMMatrixInverse(nullptr, viewProj));
	viewConstants.eyePosW = renderView.position;
	viewConstants.nearZ = renderView.nearZ;
	viewConstants.farZ = renderView.farZ;
	viewConstants.renderTargetSize = { static_cast<float>(r.viewWidth), static_cast<float>(r.viewHeight) };
	viewConstants.invRenderTargetSize = { 1.0f / r.viewWidth, 1.0f / r.viewHeight };
	viewConstants.exposure = r.settings.exposure;

	const D3D12_GPU_VIRTUAL_ADDRESS frameCb = r.uploadRing.PushConstants(frame);
	const D3D12_GPU_VIRTUAL_ADDRESS viewCb = r.uploadRing.PushConstants(viewConstants);
	cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_ViewConstants, viewCb);
	cmd->SetGraphicsRootConstantBufferView(rhi::RootSlot_FrameConstants, frameCb);
	cmd->SetComputeRootConstantBufferView(rhi::RootSlot_ViewConstants, viewCb);
	cmd->SetComputeRootConstantBufferView(rhi::RootSlot_FrameConstants, frameCb);

	// ---- Mesh pass ----------------------------------------------------------
	r.hdr->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
	r.depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	const float clear[4] = { r.settings.clearColor.x, r.settings.clearColor.y, r.settings.clearColor.z, r.settings.clearColor.w };
	const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = r.hdr->Rtv();
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = r.depth->Dsv();
	cmd->ClearRenderTargetView(hdrRtv, clear, 0, nullptr);
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);
	r.SetViewport(cmd, r.viewWidth, r.viewHeight);

	auto drawMeshes = [&](std::vector<DrawItem>::const_iterator begin, std::vector<DrawItem>::const_iterator end)
	{
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		uint32_t boundPipeline = UINT32_MAX;
		uint32_t boundMesh = UINT32_MAX;
		const MeshResource* currentMesh = nullptr;
		for (auto it = begin; it != end; )
		{
			if (it->pipeline != boundPipeline)
			{
				cmd->SetPipelineState(r.SurfacePipeline(it->pipeline));
				boundPipeline = it->pipeline;
			}
			if (it->meshSlot != boundMesh)
			{
				currentMesh = r.meshes.AtSlot(it->meshSlot);
				const D3D12_VERTEX_BUFFER_VIEW vbv = currentMesh->vertexBuffer->VertexView(sizeof(Vertex));
				const D3D12_INDEX_BUFFER_VIEW ibv = currentMesh->indexBuffer->IndexView(DXGI_FORMAT_R32_UINT);
				cmd->IASetVertexBuffers(0, 1, &vbv);
				cmd->IASetIndexBuffer(&ibv);
				boundMesh = it->meshSlot;
			}
			// Every following draw of the same mesh with the same pipeline: one instanced draw.
			auto runEnd = it + 1;
			while (runEnd != end && runEnd->pipeline == it->pipeline && runEnd->meshSlot == it->meshSlot) ++runEnd;
			gpu::MeshDrawConstants constants{ it->instanceIndex };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawIndexedInstanced(currentMesh->indexCount, static_cast<UINT>(runEnd - it), 0, 0, 0);
			++stats.drawCalls;
			it = runEnd;
		}
	};

	// Opaque world meshes.
	drawMeshes(r.drawScratch.cbegin(), firstWorldTranslucent);
	stats.instancesSubmitted = static_cast<uint32_t>(r.drawScratch.size());
	r.gpuTimer.Mark(cmd, frameIndex, Mark_MeshEnd);

	// ---- Voxel pass (ray-marched, same HDR + depth targets) -----------------
	if (!r.voxelScratch.empty())
	{
		uint32_t boundPipeline = UINT32_MAX;
		for (uint32_t i = 0; i < r.voxelScratch.size(); ++i)
		{
			if (r.voxelDrawScratch[i] != boundPipeline)
			{
				boundPipeline = r.voxelDrawScratch[i];
				cmd->SetPipelineState(r.VoxelPipeline(boundPipeline));
			}
			gpu::VoxelDrawConstants constants{ i };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawInstanced(36, 1, 0, 0);
			++stats.drawCalls;
		}
		stats.voxelVolumes = static_cast<uint32_t>(r.voxelScratch.size());
	}
	// ---- Sky: wherever no mesh or voxel was drawn ----------------------------------
	if (r.settings.sky.enabled)
	{
		cmd->SetPipelineState(r.pipelines.Get("sky"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->DrawInstanced(3, 1, 0, 0);
		++stats.drawCalls;
	}
	// ---- Translucent world meshes (over opaque meshes, voxels and the sky, back to front) --
	if (firstWorldTranslucent != firstViewModel)
	{
		drawMeshes(firstWorldTranslucent, firstViewModel);
	}
	r.gpuTimer.Mark(cmd, frameIndex, Mark_VoxelEnd);

	// ---- Particles (after translucent meshes; depth-tested, not written) -------------
	if (particleCount > 0)
	{
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		if (coveringParticles > 0)
		{
			cmd->SetPipelineState(r.pipelines.Get("particles"));
			gpu::MeshDrawConstants constants{ 0 };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawInstanced(6, coveringParticles, 0, 0);
			++stats.drawCalls;
		}
		if (particleCount > coveringParticles)
		{
			cmd->SetPipelineState(r.pipelines.Get("particles_add"));
			gpu::MeshDrawConstants constants{ coveringParticles };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawInstanced(6, particleCount - coveringParticles, 0, 0);
			++stats.drawCalls;
		}
	}

	// ---- Debug lines (HDR, depth-tested against the world) ------------------
	if (!lines.empty())
	{
		cmd->SetPipelineState(r.pipelines.Get("debug"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		cmd->DrawInstanced(static_cast<UINT>(lines.size()), 1, 0, 0);
		++stats.drawCalls;
	}
	stats.debugLines = static_cast<uint32_t>(lines.size() / 2);

	// ---- Ambient occlusion (compute), from the world's depth -----------------------
	const SsaoSettings& ssaoSettings = r.settings.ssao;
	const bool ssaoOn = ssaoSettings.enabled && ssaoSettings.strength > 0.0f;
	if (ssaoOn)
	{
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const UINT groupsX = (r.viewWidth + 7) / 8, groupsY = (r.viewHeight + 7) / 8;
		r.aoRaw->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("ssao"));
		gpu::SsaoConstants constants{ CHERRY_INVALID_INDEX, r.aoRaw->UavIndex(), r.viewWidth, r.viewHeight, r.depth->SrvIndex(),
			std::max(ssaoSettings.radius, 0.01f), std::max(ssaoSettings.bias, 0.0f), 0 };
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch(groupsX, groupsY, 1);
		r.aoRaw->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.ao->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("ssao_blur"));
		constants.sourceIndex = r.aoRaw->SrvIndex();
		constants.destIndex = r.ao->UavIndex();
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch(groupsX, groupsY, 1);
		r.ao->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	// ---- Edge highlights (compute), from the world's depth --------------------------
	const EdgeHighlightSettings& edgeSettings = r.settings.edgeHighlights;
	const bool edgesOn = edgeSettings.enabled && edgeSettings.strength > 0.0f && r.edges;
	if (edgesOn)
	{
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.edges->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("edges"));
		gpu::SsaoConstants constants{ CHERRY_INVALID_INDEX, r.edges->UavIndex(), r.viewWidth, r.viewHeight, r.depth->SrvIndex(),
			std::max(edgeSettings.distance, 1.0f), std::max(edgeSettings.sharpness, 1e-5f), 0 };
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch((r.viewWidth + 7) / 8, (r.viewHeight + 7) / 8, 1);
		r.edges->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	// ---- Light shafts (compute, half size), from the world's depth ------------------
	const LightShaftSettings& shaftSettings = r.settings.lightShafts;
	const bool shaftsOn = shaftSettings.enabled && shaftSettings.intensity > 0.0f && r.shaft;
	if (shaftsOn)
	{
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (shadows)
			for (auto& map : r.shadowMaps) map->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const uint32_t w = r.shaft->Width(), h = r.shaft->Height();
		r.shaftRaw->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("shafts"));
		gpu::LightShaftConstants constants{ CHERRY_INVALID_INDEX, r.shaftRaw->UavIndex(), w, h, r.depth->SrvIndex(),
			std::max(shaftSettings.density, 0.0f), std::max(shaftSettings.interiorDensity, 0.0f), std::clamp(shaftSettings.anisotropy, 0.0f, 0.95f),
			shaftSettings.intensity, std::max(shaftSettings.maxDistance, 1.0f), static_cast<uint32_t>(std::clamp(shaftSettings.steps, 4, 128)), 0 };
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		r.shaftRaw->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.shaft->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("shafts_blur"));
		constants.sourceIndex = r.shaftRaw->SrvIndex();
		constants.destIndex = r.shaft->UavIndex();
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		r.shaft->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	// ---- View models: depth cleared so they always draw on top of the world --
	if (firstViewModel != r.drawScratch.cend())
	{
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		drawMeshes(firstViewModel, r.drawScratch.cend());
	}
	r.gpuTimer.Mark(cmd, frameIndex, Mark_DebugEnd);

	// ---- Bloom (compute): down the chain from the HDR view, then back up ---------
	r.hdr->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	const BloomSettings& bloom = r.settings.bloom;
	const bool bloomOn = bloom.enabled && bloom.intensity > 0.0f && !r.bloomChain.empty();
	if (bloomOn)
	{
		auto dispatch = [&](const char* pipeline, rhi::Texture& source, rhi::Texture& dest, bool first)
		{
			source.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			dest.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cmd->SetPipelineState(r.pipelines.Get(pipeline));
			gpu::BloomConstants constants{ source.SrvIndex(), dest.UavIndex(), dest.Width(), dest.Height(),
				bloom.threshold, std::max(bloom.knee, 0.001f), first ? 1u : 0u, bloom.radius };
			cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
			cmd->Dispatch((dest.Width() + 7) / 8, (dest.Height() + 7) / 8, 1);
			dest.UavBarrier(cmd);
		};
		dispatch("bloom_down", *r.hdr, *r.bloomChain[0], true);
		for (size_t i = 1; i < r.bloomChain.size(); ++i) dispatch("bloom_down", *r.bloomChain[i - 1], *r.bloomChain[i], false);
		for (size_t i = r.bloomChain.size() - 1; i > 0; --i) dispatch("bloom_up", *r.bloomChain[i], *r.bloomChain[i - 1], false);
		r.bloomChain[0]->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// ---- Eye adaptation (compute): the view's brightness eases the stored exposure ----
	const EyeAdaptationSettings& eye = r.settings.eyeAdaptation;
	const bool eyeOn = eye.enabled && r.luminanceBlocks && r.adaptedExposure;
	if (eyeOn)
	{
		const uint32_t blocksX = r.luminanceBlocks->Width(), blocksY = r.luminanceBlocks->Height();
		gpu::EyeAdaptationConstants constants{ r.hdr->SrvIndex(), r.luminanceBlocks->UavIndex(), r.luminanceBlocks->SrvIndex(),
			r.viewWidth, r.viewHeight, blocksX, blocksY, r.adaptedExposure->UavIndex(), -12.0f, 6.0f,
			std::max(eye.key, 1e-4f), std::max(eye.minExposure, 1e-4f), std::max(eye.maxExposure, eye.minExposure),
			std::max(eye.speedBrighter, 0.0f), std::max(eye.speedDarker, 0.0f), (eye.reset || !r.exposureValid) ? 1u : 0u };
		r.luminanceBlocks->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("eye_reduce"));
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(constants) / 4, &constants, 0);
		cmd->Dispatch(blocksX, blocksY, 1);
		r.luminanceBlocks->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.adaptedExposure->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("eye_adapt"));
		cmd->Dispatch(1, 1, 1);
		r.adaptedExposure->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.exposureValid = true;
	}
	else
	{
		r.exposureValid = false;   // start from the view's own exposure when it's switched on
	}

	// ---- Tonemap (compute), then anti-aliasing ----------------------------------
	const bool antiAliasing = r.settings.antiAliasing && r.ldrBeforeAA;
	rhi::Texture& tonemapped = antiAliasing ? *r.ldrBeforeAA : *r.ldr;
	tonemapped.Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetPipelineState(r.pipelines.Get("tonemap"));
	if (ssaoOn || shaftsOn || edgesOn) r.depth->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	gpu::TonemapConstants tonemap{ r.hdr->SrvIndex(), tonemapped.UavIndex(), r.viewWidth, r.viewHeight,
		bloomOn ? r.bloomChain[0]->SrvIndex() : CHERRY_INVALID_INDEX, bloom.intensity,
		ssaoOn ? r.ao->SrvIndex() : CHERRY_INVALID_INDEX, r.depth->SrvIndex(), std::clamp(ssaoSettings.strength, 0.0f, 1.0f),
		shaftsOn ? r.shaft->SrvIndex() : CHERRY_INVALID_INDEX, eyeOn ? r.adaptedExposure->SrvIndex() : CHERRY_INVALID_INDEX,
		edgesOn ? r.edges->SrvIndex() : CHERRY_INVALID_INDEX, edgeSettings.strength };
	cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(tonemap) / 4, &tonemap, 0);
	cmd->Dispatch((r.viewWidth + 7) / 8, (r.viewHeight + 7) / 8, 1);
	if (antiAliasing)
	{
		r.ldrBeforeAA->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.ldr->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetPipelineState(r.pipelines.Get("fxaa"));
		gpu::FxaaConstants fxaa{ r.ldrBeforeAA->SrvIndex(), r.ldr->UavIndex(), r.viewWidth, r.viewHeight };
		cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, sizeof(fxaa) / 4, &fxaa, 0);
		cmd->Dispatch((r.viewWidth + 7) / 8, (r.viewHeight + 7) / 8, 1);
	}
	r.ldr->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	r.gpuTimer.Mark(cmd, frameIndex, Mark_TonemapEnd);

	r.viewRendered = true;
}

void Renderer::Impl::ClearView(ID3D12GraphicsCommandList4* cmd)
{
	ldr->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const float black[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
	cmd->ClearRenderTargetView(ldr->Rtv(), black, 0, nullptr);
	ldr->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::EndFrame(bool viewFullscreen)
{
	Impl& r = *mImpl;
	if (!r.frameActive) return;
	ID3D12GraphicsCommandList4* cmd = r.device.CmdList();

	if (!r.viewRendered)
	{
		r.ClearView(cmd);
		// Keep every timestamp written so the resolve never reads unwritten queries.
		for (GpuMark mark : { Mark_MeshEnd, Mark_VoxelEnd, Mark_DebugEnd, Mark_TonemapEnd })
		{
			r.gpuTimer.Mark(cmd, r.device.FrameIndex(), mark);
		}
	}

	ID3D12Resource* backBuffer = r.swapChain.BackBuffer();
	D3D12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cmd->ResourceBarrier(1, &toRt);

	const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = r.swapChain.BackBufferRtv();
	cmd->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
	r.SetViewport(cmd, r.swapChain.Width(), r.swapChain.Height());

	if (viewFullscreen)
	{
		cmd->SetPipelineState(r.pipelines.Get("blit"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		gpu::BlitConstants blit{ r.ldr->SrvIndex() };
		cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &blit, 0);
		cmd->DrawInstanced(3, 1, 0, 0);
	}
	else
	{
		const float background[4] = { 0.08f, 0.08f, 0.09f, 1.0f };
		cmd->ClearRenderTargetView(backBufferRtv, background, 0, nullptr);
	}

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
	r.gpuTimer.Mark(cmd, r.device.FrameIndex(), Mark_FrameEnd);
	r.gpuTimer.Resolve(cmd, r.device.FrameIndex());

	D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	cmd->ResourceBarrier(1, &toPresent);

	r.device.EndFrame();
	r.swapChain.Present(r.settings.vsync);
	r.device.SignalEndOfFrame();
	r.frameActive = false;

	r.debugDraw.Clear();
	r.stats.viewWidth = r.viewWidth;
	r.stats.viewHeight = r.viewHeight;
	r.stats.descriptorsInUse = r.device.SrvHeap().Allocated();
	r.stats.d3dErrors = r.device.DebugErrorCount();
	r.stats.d3dWarnings = r.device.DebugWarningCount();
}
