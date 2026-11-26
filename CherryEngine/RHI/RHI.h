#pragma once

// The render hardware interface: what the renderer needs from a graphics API, with a D3D12
// backend (RHI/D3D12) and a Vulkan backend (RHI/Vulkan) behind it, picked at start-up.
//
// Binding model (the same on both, see Shaders/Common.hlsli):
//   b0      per-draw constants, up to kDrawConstantBytes (CommandList::SetConstants)
//   b1, b2  ViewConstants and FrameConstants (SetViewConstants / SetFrameConstants, from
//           Device::AllocateConstants)
//   s0..s6  fixed samplers
//   everything else through bindless indices (ResourceDescriptorHeap[] in HLSL): every buffer
//   and texture view gets an index when it's made (SrvIndex / UavIndex).
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace rhi
{
	constexpr uint32_t kFramesInFlight = 2;
	constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
	constexpr uint32_t kDrawConstantBytes = 64;      // b0: 16 dwords
	constexpr uint32_t kTimestampsPerFrame = 8;     // timestamp queries per frame slot

	enum class Backend { D3D12, Vulkan };
	const char* BackendName(Backend backend);       // "D3D12", "Vulkan"
	// "d3d12" / "dx12" / "vulkan" / "vk" (any case); false if it's neither.
	bool ParseBackend(const std::string& text, Backend& backend);

	enum class Format : uint8_t
	{
		Unknown,
		RGBA8Unorm, RGBA8UnormSrgb, BGRA8Unorm, BGRA8UnormSrgb, BGRX8Unorm, BGRX8UnormSrgb,
		RGBA16Float, RGBA32Float, RG32Float, RG16Float, R32Float, R16Float, R8Unorm, R8Uint, RG8Unorm, R16Unorm,
		RGB10A2Unorm, RG11B10Float,
		D32Float,   // a depth texture; sampled as one float
		BC1Unorm, BC1UnormSrgb, BC2Unorm, BC2UnormSrgb, BC3Unorm, BC3UnormSrgb, BC4Unorm, BC5Unorm,
		BC6HUfloat, BC7Unorm, BC7UnormSrgb,
	};
	bool IsBlockCompressed(Format format);
	// Bytes per texel, or per 4x4 block for block-compressed formats.
	uint32_t FormatBytes(Format format);
	Format ToSrgb(Format format);

	enum class ResourceState : uint8_t
	{
		Common,
		RenderTarget,
		DepthWrite,
		ShaderResource,     // read by any shader stage
		UnorderedAccess,
		CopyDest,
		CopySource,
		Present,
	};

	enum TextureUsage : uint32_t
	{
		TextureUsage_ShaderResource  = 1 << 0,
		TextureUsage_RenderTarget    = 1 << 1,
		TextureUsage_DepthStencil    = 1 << 2,
		TextureUsage_UnorderedAccess = 1 << 3,
	};

	enum class TextureDimension : uint8_t { Texture2D, Texture3D };

	struct TextureDesc
	{
		TextureDimension dimension = TextureDimension::Texture2D;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;               // 3D textures
		uint32_t mipLevels = 1;
		Format format = Format::RGBA8Unorm;
		Format srvFormat = Format::Unknown;   // a different view format for sampling (an sRGB view); Unknown = format
		uint32_t usage = TextureUsage_ShaderResource;
		float clearColor[4] = { 0, 0, 0, 1 };  // the fast-clear value (D3D12)
		ResourceState initialState = ResourceState::Common;
		const char* name = "Texture";
	};

	// One subresource (mip level) of tightly described data for Texture::Upload.
	struct SubresourceData
	{
		const void* data = nullptr;
		uint32_t rowPitch = 0;      // bytes from one row (of blocks) to the next
		uint32_t slicePitch = 0;    // bytes from one depth slice to the next
	};

	enum class HeapKind { Default, Upload };

	struct BufferDesc
	{
		uint64_t size = 0;
		uint32_t stride = 0;        // > 0: structured buffer views of this stride; 0: raw (ByteAddressBuffer)
		HeapKind heap = HeapKind::Default;
		bool createSrv = false;
		bool createUav = false;
		const char* name = "Buffer";
	};

	class CommandList;

	// Default-heap buffers are filled with Upload (copied on the GPU queue before any later
	// frame reads them); upload-heap buffers stay mapped (Mapped()).
	class Buffer
	{
	public:
		virtual ~Buffer() = default;
		virtual void Upload(const void* data, uint64_t size, uint64_t dstOffset = 0) = 0;

		void* Mapped() const { return mMapped; }
		uint64_t Size() const { return mDesc.size; }
		uint32_t Stride() const { return mDesc.stride; }
		uint32_t SrvIndex() const { return mSrvIndex; }
		uint32_t UavIndex() const { return mUavIndex; }
		const BufferDesc& Desc() const { return mDesc; }

	protected:
		BufferDesc mDesc;
		void* mMapped = nullptr;
		uint32_t mSrvIndex = kInvalidIndex;
		uint32_t mUavIndex = kInvalidIndex;
	};

	// A texture with optional bindless SRV and UAV indices; usable as a render target or depth
	// target when its usage says so. It tracks its whole-resource state, so passes can call
	// Transition() freely.
	class Texture
	{
	public:
		virtual ~Texture() = default;
		// Fills every subresource (mip levels in order); not waited for.
		virtual void Upload(const SubresourceData* data, uint32_t count, ResourceState finalState) = 0;
		// Overwrites a box of mip 0 with tightly packed texels (x fastest, then y, then z).
		virtual void UploadRegion(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
			const void* data, uint32_t bytesPerTexel) = 0;

		void Transition(CommandList& cmd, ResourceState state);
		void UavBarrier(CommandList& cmd);

		ResourceState State() const { return mState; }
		void SetTrackedState(ResourceState state) { mState = state; }   // backends only
		const TextureDesc& Desc() const { return mDesc; }
		uint32_t Width() const { return mDesc.width; }
		uint32_t Height() const { return mDesc.height; }
		uint32_t SrvIndex() const { return mSrvIndex; }
		uint32_t UavIndex() const { return mUavIndex; }

	protected:
		TextureDesc mDesc;
		ResourceState mState = ResourceState::Common;
		uint32_t mSrvIndex = kInvalidIndex;
		uint32_t mUavIndex = kInvalidIndex;
	};

	// ---- Pipelines ---------------------------------------------------------------------------

	// Mesh: the engine's Vertex (48 bytes); MeshCompact: MeshFormat::Compact (28 bytes);
	// ImGui: ImDrawVert (20 bytes); RmlUi: Rml::Vertex (20 bytes: position, RGBA8 colour, uv).
	enum class VertexLayout : uint8_t { None, Mesh, MeshCompact, ImGui, RmlUi };
	enum class Topology : uint8_t { Triangles, Lines };
	enum class CullMode : uint8_t { None, Front, Back };
	enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

	struct GraphicsPipelineDesc
	{
		std::string shaderFile;            // relative to the shader root; with `source`, only a name for diagnostics
		std::string source;                // HLSL compiled instead of shaderFile (generated shaders)
		std::string vsEntry = "VSMain";
		std::string psEntry = "PSMain";    // empty: depth only
		std::vector<std::string> defines;
		VertexLayout vertexLayout = VertexLayout::Mesh;
		Topology topology = Topology::Triangles;
		std::vector<Format> colorFormats;
		Format depthFormat = Format::Unknown;
		bool depthTest = true;
		bool depthWrite = true;
		CompareOp depthFunc = CompareOp::LessEqual;
		CullMode cullMode = CullMode::Back;   // front faces wind clockwise on screen (D3D's default)
		bool alphaBlend = false;
		bool additiveBlend = false;        // colour x alpha added to what's there (glow, sparks)
		bool premultipliedBlend = false;   // colour already multiplied by alpha: colour + what's there x (1 - alpha) (RmlUi)
		// Depth bias (shadow maps): constant in depth-buffer units, plus a slope-scaled part.
		int depthBias = 0;
		float slopeScaledDepthBias = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	struct ComputePipelineDesc
	{
		std::string shaderFile;
		std::string entry = "CSMain";
		std::vector<std::string> defines;
	};

	class Pipeline
	{
	public:
		virtual ~Pipeline() = default;
		bool IsCompute() const { return mCompute; }
		Topology GetTopology() const { return mTopology; }
	protected:
		bool mCompute = false;
		Topology mTopology = Topology::Triangles;
	};

	// ---- Commands ----------------------------------------------------------------------------

	// A view of the per-frame constant ring (Device::AllocateConstants).
	struct ConstantAllocation
	{
		void* cpu = nullptr;
		uint64_t offset = 0;        // in the ring (Vulkan)
		uint64_t gpuAddress = 0;    // (D3D12)
		uint32_t size = 0;
	};

	// The targets of a run of draws. Without `color` and `depth`, `backBuffer` draws to the
	// swap chain. BeginRendering also sets the viewport and scissor to the whole target.
	struct RenderPassDesc
	{
		Texture* color = nullptr;
		Texture* depth = nullptr;
		bool backBuffer = false;
		bool clearColor = false;
		float clearValue[4] = { 0, 0, 0, 1 };
		bool clearDepth = false;
		float depthValue = 1.0f;
	};

	// The frame's command list. Draws happen between BeginRendering and EndRendering;
	// transitions, dispatches and timestamp resolves happen outside them.
	class CommandList
	{
	public:
		virtual ~CommandList() = default;
		virtual void BeginRendering(const RenderPassDesc& pass) = 0;
		virtual void EndRendering() = 0;
		virtual void SetViewport(float x, float y, float width, float height) = 0;
		virtual void SetScissor(int32_t left, int32_t top, int32_t right, int32_t bottom) = 0;

		virtual void SetPipeline(Pipeline* pipeline) = 0;
		// b0 for the bound pipeline (graphics or compute).
		virtual void SetConstants(const void* data, uint32_t bytes) = 0;
		template <typename T> void SetConstants(const T& data)
		{
			static_assert(sizeof(T) <= kDrawConstantBytes, "too many draw constants");
			SetConstants(&data, sizeof(T));
		}
		// b1 / b2, for graphics and compute.
		virtual void SetViewConstants(const ConstantAllocation& constants) = 0;
		virtual void SetFrameConstants(const ConstantAllocation& constants) = 0;

		virtual void SetVertexBuffer(Buffer& buffer, uint32_t stride, uint64_t offset = 0) = 0;
		virtual void SetIndexBuffer(Buffer& buffer, bool sixteenBit = false, uint64_t offset = 0) = 0;
		virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
		virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0) = 0;
		virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

		virtual void Barrier(Texture& texture, ResourceState before, ResourceState after) = 0;
		virtual void UavBarrier(Texture& texture) = 0;

		// Timestamp queries: kTimestampsPerFrame per frame slot (index = slot * per frame + n).
		virtual void Timestamp(uint32_t index) = 0;
		// Makes [first, first + count) readable through Device::Timestamps once the frame is done.
		virtual void ResolveTimestamps(uint32_t first, uint32_t count) = 0;

		// Debug markers (RenderDoc, PIX).
		virtual void BeginEvent(const char* name) = 0;
		virtual void EndEvent() = 0;
	};

	inline void Texture::Transition(CommandList& cmd, ResourceState state)
	{
		if (mState == state) return;
		cmd.Barrier(*this, mState, state);
		mState = state;
	}

	inline void Texture::UavBarrier(CommandList& cmd) { cmd.UavBarrier(*this); }

	// ---- Device ------------------------------------------------------------------------------

	struct DeviceOptions
	{
		bool debugLayer = false;           // D3D12 debug layer / Vulkan validation layers
		bool gpuBasedValidation = false;
	};

	struct DeviceCaps
	{
		std::string adapterName;
		bool raytracing = false;
		bool tearing = false;
	};

	enum class ShaderTarget { Dxil, Spirv };

	// The GPU, its queue, the swap chain of one window, the frame's command list, the bindless
	// descriptors, the constant ring and deferred destruction.
	//
	// Frame model: kFramesInFlight frames may be queued on the GPU at once. BeginFrame waits only
	// for the frame that last used the same slot.
	class Device
	{
	public:
		// `nativeWindow` is an HWND (Windows) or ANativeWindow* (Android). Returns null with `error` filled when the GPU can't run the engine.
		static std::unique_ptr<Device> Create(Backend backend, const DeviceOptions& options, void* nativeWindow,
			uint32_t width, uint32_t height, std::string& error);
		virtual ~Device() = default;

		virtual Backend GetBackend() const = 0;
		virtual ShaderTarget GetShaderTarget() const = 0;
		virtual const DeviceCaps& Caps() const = 0;

		// Waits for the frame slot, then opens the command list.
		virtual void BeginFrame() = 0;
		virtual CommandList& Cmd() = 0;
		// Submits the command list and presents.
		virtual void EndFrame(bool vsync) = 0;
		virtual uint32_t FrameIndex() const = 0;
		virtual uint64_t FrameNumber() const = 0;
		virtual void WaitIdle() = 0;

		virtual void ResizeSwapChain(uint32_t width, uint32_t height) = 0;
		// The window was replaced or taken away (null): Android does this when the app goes to the
		// background. Until a window comes back, frames draw into a stand-in and aren't shown.
		virtual void SetWindow(void* nativeWindow, uint32_t width, uint32_t height) = 0;
		virtual uint32_t SwapChainWidth() const = 0;
		virtual uint32_t SwapChainHeight() const = 0;
		virtual Format SwapChainFormat() const = 0;

		virtual std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) = 0;
		virtual std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) = 0;

		// Safe to call from worker threads. Null with `error` filled on failure.
		virtual std::shared_ptr<Pipeline> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const std::vector<uint8_t>& vs,
			const std::vector<uint8_t>& ps, const std::string& name, std::string& error) = 0;
		virtual std::shared_ptr<Pipeline> CreateComputePipeline(const std::vector<uint8_t>& cs, const std::string& name, std::string& error) = 0;

		// Keeps `object` alive until the GPU has finished every frame that may use it.
		virtual void DeferRelease(std::shared_ptr<void> object) = 0;

		// Per-frame constant memory (256-byte aligned), valid until this frame slot comes round again.
		virtual ConstantAllocation AllocateConstants(uint32_t size) = 0;
		template <typename T> ConstantAllocation PushConstants(const T& data)
		{
			ConstantAllocation allocation = AllocateConstants(static_cast<uint32_t>(sizeof(T)));
			std::memcpy(allocation.cpu, &data, sizeof(T));
			return allocation;
		}

		// Resolved timestamps (ResolveTimestamps), kFramesInFlight * kTimestampsPerFrame of them.
		virtual const uint64_t* Timestamps() const = 0;
		virtual double TimestampTicksToMs() const = 0;

		virtual uint32_t DescriptorsInUse() const = 0;
		virtual uint32_t DebugErrorCount() const = 0;
		virtual uint32_t DebugWarningCount() const = 0;
	};
}
