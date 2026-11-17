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
		uint32_t meshSlot;
		uint32_t instanceIndex;
	};

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
	MaterialHandle defaultMaterial;
	GpuTimer gpuTimer;

	DynamicStructuredBuffer instanceBuffer{ "InstanceBuffer", sizeof(gpu::InstanceData) };
	DynamicStructuredBuffer materialBuffer{ "MaterialBuffer", sizeof(gpu::MaterialData) };
	DynamicStructuredBuffer voxelBuffer{ "VoxelVolumeBuffer", sizeof(gpu::VoxelVolumeData) };
	DynamicStructuredBuffer debugVertexBuffer{ "DebugVertexBuffer", sizeof(gpu::DebugVertex) };

	// Offscreen view targets.
	std::unique_ptr<rhi::Texture> hdr;
	std::unique_ptr<rhi::Texture> depth;
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
	std::vector<gpu::MaterialData> materialScratch;
	std::vector<gpu::VoxelVolumeData> voxelScratch;
	std::vector<DrawItem> drawScratch;
	std::vector<uint32_t> voxelDrawScratch;

	void CreatePipelines();
	void CreateViewTargets(uint32_t width, uint32_t height);
	void InitImGui();
	void ShutdownImGui();
	void SetViewport(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height);
	void BindGlobals(ID3D12GraphicsCommandList4* cmd);
	gpu::FrameConstants BuildFrameConstants();
	void ClearView(ID3D12GraphicsCommandList4* cmd);
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

	rhi::GraphicsPipelineDesc blit;
	blit.shaderFile = "Blit.hlsl";
	blit.vertexLayout = rhi::VertexLayout::None;
	blit.rtvFormats = { rhi::SwapChain::kFormat };
	blit.depthTest = false;
	blit.depthWrite = false;
	blit.cullMode = D3D12_CULL_MODE_NONE;
	pipelines.AddGraphics("blit", blit);
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

	viewWidth = width;
	viewHeight = height;
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
	r.instanceBuffer.Reset();
	r.materialBuffer.Reset();
	r.voxelBuffer.Reset();
	r.debugVertexBuffer.Reset();
	r.meshes.Clear();
	r.textures.Clear();
	r.materials.Clear();
	r.voxelVolumes.Clear();
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

XMUINT3 Renderer::GetVoxelVolumeSize(VoxelVolumeHandle volume) const
{
	const VoxelResource* resource = mImpl->voxelVolumes.Get(volume);
	return resource ? resource->size : XMUINT3(0, 0, 0);
}

RenderSettings& Renderer::Settings() { return mImpl->settings; }
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

gpu::FrameConstants Renderer::Impl::BuildFrameConstants()
{
	gpu::FrameConstants frame{};
	frame.time = totalTime;
	frame.deltaTime = deltaTime;
	frame.frameNumber = static_cast<uint32_t>(device.FrameNumber());
	frame.ambientLight = { settings.ambient.x, settings.ambient.y, settings.ambient.z, 1.0f };

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

	const uint32_t pointCount = std::min<uint32_t>(static_cast<uint32_t>(settings.pointLights.size()), CHERRY_MAX_POINT_LIGHTS);
	for (uint32_t i = 0; i < pointCount; ++i)
	{
		const PointLight& light = settings.pointLights[i];
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
	return frame;
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

	// ---- Materials: one GPU entry per pool slot ---------------------------
	r.materialScratch.resize(r.materials.SlotCount());
	for (uint32_t slot = 0; slot < r.materials.SlotCount(); ++slot)
	{
		gpu::MaterialData& gpuMaterial = r.materialScratch[slot];
		gpuMaterial = {};
		gpuMaterial.albedoTextureIndex = CHERRY_INVALID_INDEX;
		if (const MaterialDesc* desc = r.materials.AtSlot(slot))
		{
			gpuMaterial.baseColor = desc->baseColor;
			gpuMaterial.fresnelR0 = desc->fresnelR0;
			gpuMaterial.roughness = desc->roughness;
			if (const TextureResource* texture = r.textures.Get(desc->albedoTexture))
			{
				gpuMaterial.albedoTextureIndex = texture->texture->SrvIndex();
			}
		}
	}

	// ---- Mesh instances (CPU frustum culling) ------------------------------
	r.instanceScratch.clear();
	r.drawScratch.clear();
	const uint32_t boundsColor = DebugDraw::Color(0.2f, 1.0f, 0.3f);
	for (Entity entity : scene.MeshEntities())
	{
		const MeshRenderer& renderer = scene.Get<MeshRenderer>(entity);
		if (!renderer.visible) continue;
		const MeshResource* mesh = r.meshes.Get(renderer.mesh);
		if (!mesh) continue;

		const XMMATRIX world = scene.Get<Transform>(entity).Matrix();
		const bool viewModel = renderer.layer == RenderLayer::ViewModel;
		BoundingBox worldBounds;
		mesh->bounds.Transform(worldBounds, world);
		if (!viewModel && r.settings.cullingEnabled && !frustum.Intersects(worldBounds))
		{
			++stats.instancesCulled;
			continue;
		}
		if (!viewModel && r.settings.drawBounds)
		{
			r.debugDraw.Box(worldBounds, boundsColor);
		}

		gpu::InstanceData instance{};
		XMStoreFloat4x4(&instance.world, world);
		XMStoreFloat4x4(&instance.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
		instance.materialIndex = r.materials.IsValid(renderer.material) ? renderer.material.index : r.defaultMaterial.index;

		r.drawScratch.push_back({ static_cast<uint32_t>(renderer.layer), renderer.mesh.index, static_cast<uint32_t>(r.instanceScratch.size()) });
		r.instanceScratch.push_back(instance);
	}
	// Layer first (world before view models), then by mesh so vertex/index buffers change rarely.
	std::sort(r.drawScratch.begin(), r.drawScratch.end(), [](const DrawItem& a, const DrawItem& b)
	{
		return a.layer != b.layer ? a.layer < b.layer : a.meshSlot < b.meshSlot;
	});
	const auto firstViewModel = std::find_if(r.drawScratch.begin(), r.drawScratch.end(),
		[](const DrawItem& item) { return item.layer == static_cast<uint32_t>(RenderLayer::ViewModel); });

	// ---- Voxel volumes ------------------------------------------------------
	r.voxelScratch.clear();
	for (Entity entity : scene.VoxelEntities())
	{
		const VoxelVolumeRenderer& renderer = scene.Get<VoxelVolumeRenderer>(entity);
		if (!renderer.visible) continue;
		const VoxelResource* volume = r.voxelVolumes.Get(renderer.volume);
		if (!volume) continue;

		const XMMATRIX world = XMMatrixScaling(renderer.voxelSize, renderer.voxelSize, renderer.voxelSize) * scene.Get<Transform>(entity).Matrix();
		BoundingOrientedBox box;
		BoundingOrientedBox::CreateFromBoundingBox(box, BoundingBox(
			XMFLOAT3(volume->size.x * 0.5f, volume->size.y * 0.5f, volume->size.z * 0.5f),
			XMFLOAT3(volume->size.x * 0.5f, volume->size.y * 0.5f, volume->size.z * 0.5f)));
		box.Transform(box, world);
		if (r.settings.cullingEnabled && !frustum.Intersects(box)) continue;
		if (r.settings.drawBounds)
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
		r.voxelScratch.push_back(data);
	}

	// ---- Upload per-frame data ---------------------------------------------
	gpu::FrameConstants frame = r.BuildFrameConstants();
	frame.materialBufferIndex = r.materialBuffer.Write(r.device, frameIndex, r.materialScratch.data(), static_cast<uint32_t>(r.materialScratch.size()));
	frame.instanceBufferIndex = r.instanceBuffer.Write(r.device, frameIndex, r.instanceScratch.data(), static_cast<uint32_t>(r.instanceScratch.size()));
	frame.voxelVolumeBufferIndex = r.voxelBuffer.Write(r.device, frameIndex, r.voxelScratch.data(), static_cast<uint32_t>(r.voxelScratch.size()));
	const auto& lines = r.debugDraw.Vertices();
	frame.debugVertexBufferIndex = r.debugVertexBuffer.Write(r.device, frameIndex, lines.data(), static_cast<uint32_t>(lines.size()));

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
		cmd->SetPipelineState(r.pipelines.Get("mesh"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		uint32_t boundMesh = UINT32_MAX;
		const MeshResource* currentMesh = nullptr;
		for (auto it = begin; it != end; ++it)
		{
			if (it->meshSlot != boundMesh)
			{
				currentMesh = r.meshes.AtSlot(it->meshSlot);
				const D3D12_VERTEX_BUFFER_VIEW vbv = currentMesh->vertexBuffer->VertexView(sizeof(Vertex));
				const D3D12_INDEX_BUFFER_VIEW ibv = currentMesh->indexBuffer->IndexView(DXGI_FORMAT_R32_UINT);
				cmd->IASetVertexBuffers(0, 1, &vbv);
				cmd->IASetIndexBuffer(&ibv);
				boundMesh = it->meshSlot;
			}
			gpu::MeshDrawConstants constants{ it->instanceIndex };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawIndexedInstanced(currentMesh->indexCount, 1, 0, 0, 0);
			++stats.drawCalls;
		}
	};

	// World meshes.
	drawMeshes(r.drawScratch.cbegin(), firstViewModel);
	stats.instancesSubmitted = static_cast<uint32_t>(r.drawScratch.size());
	r.gpuTimer.Mark(cmd, frameIndex, Mark_MeshEnd);

	// ---- Voxel pass (ray-marched, same HDR + depth targets) -----------------
	if (!r.voxelScratch.empty())
	{
		cmd->SetPipelineState(r.pipelines.Get("voxel"));
		for (uint32_t i = 0; i < r.voxelScratch.size(); ++i)
		{
			gpu::VoxelDrawConstants constants{ i };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootSlot_DrawConstants, 1, &constants, 0);
			cmd->DrawInstanced(36, 1, 0, 0);
			++stats.drawCalls;
		}
		stats.voxelVolumes = static_cast<uint32_t>(r.voxelScratch.size());
	}
	r.gpuTimer.Mark(cmd, frameIndex, Mark_VoxelEnd);

	// ---- Debug lines (HDR, depth-tested against the world) ------------------
	if (!lines.empty())
	{
		cmd->SetPipelineState(r.pipelines.Get("debug"));
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		cmd->DrawInstanced(static_cast<UINT>(lines.size()), 1, 0, 0);
		++stats.drawCalls;
	}
	stats.debugLines = static_cast<uint32_t>(lines.size() / 2);

	// ---- View models: depth cleared so they always draw on top of the world --
	if (firstViewModel != r.drawScratch.cend())
	{
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		drawMeshes(firstViewModel, r.drawScratch.cend());
	}
	r.gpuTimer.Mark(cmd, frameIndex, Mark_DebugEnd);

	// ---- Tonemap (compute) --------------------------------------------------
	r.hdr->Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	r.ldr->Transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetPipelineState(r.pipelines.Get("tonemap"));
	gpu::TonemapConstants tonemap{ r.hdr->SrvIndex(), r.ldr->UavIndex(), r.viewWidth, r.viewHeight };
	cmd->SetComputeRoot32BitConstants(rhi::RootSlot_DrawConstants, 4, &tonemap, 0);
	cmd->Dispatch((r.viewWidth + 7) / 8, (r.viewHeight + 7) / 8, 1);
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
