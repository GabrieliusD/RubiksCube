// The D3D12 backend of the RHI: one global bindless root signature (root constants b0, root
// CBVs b1/b2, static samplers s0..s6, CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), committed resources,
// a flip-model swap chain, and fences for frames and uploads.
#include "RHI/D3D12/D3DCommon.h"
#include "RHI/D3D12/DescriptorHeap.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace rhi
{
	namespace
	{
		constexpr uint32_t kSrvHeapCapacity = 65536;
		constexpr uint32_t kRtvHeapCapacity = 256;
		constexpr uint32_t kDsvHeapCapacity = 64;
		constexpr uint32_t kSwapBufferCount = 3;
		constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		constexpr uint64_t kConstantBytesPerFrame = 4 * 1024 * 1024;
		constexpr uint32_t kTimestampCount = kFramesInFlight * kTimestampsPerFrame;

		enum RootSlot : uint32_t
		{
			RootSlot_DrawConstants = 0,
			RootSlot_ViewConstants = 1,
			RootSlot_FrameConstants = 2,
		};

		std::string WideToUtf8(const wchar_t* wide)
		{
			char buffer[256];
			WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer), nullptr, nullptr);
			return buffer;
		}

		DXGI_FORMAT ToDxgi(Format format)
		{
			switch (format)
			{
			case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case Format::RGBA8UnormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			case Format::BGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case Format::BGRA8UnormSrgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			case Format::BGRX8Unorm: return DXGI_FORMAT_B8G8R8X8_UNORM;
			case Format::BGRX8UnormSrgb: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
			case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case Format::RGBA32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case Format::RG32Float: return DXGI_FORMAT_R32G32_FLOAT;
			case Format::RG16Float: return DXGI_FORMAT_R16G16_FLOAT;
			case Format::R32Float: return DXGI_FORMAT_R32_FLOAT;
			case Format::R16Float: return DXGI_FORMAT_R16_FLOAT;
			case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
			case Format::R8Uint: return DXGI_FORMAT_R8_UINT;
			case Format::RG8Unorm: return DXGI_FORMAT_R8G8_UNORM;
			case Format::R16Unorm: return DXGI_FORMAT_R16_UNORM;
			case Format::RGB10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case Format::RG11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
			case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
			case Format::BC1Unorm: return DXGI_FORMAT_BC1_UNORM;
			case Format::BC1UnormSrgb: return DXGI_FORMAT_BC1_UNORM_SRGB;
			case Format::BC2Unorm: return DXGI_FORMAT_BC2_UNORM;
			case Format::BC2UnormSrgb: return DXGI_FORMAT_BC2_UNORM_SRGB;
			case Format::BC3Unorm: return DXGI_FORMAT_BC3_UNORM;
			case Format::BC3UnormSrgb: return DXGI_FORMAT_BC3_UNORM_SRGB;
			case Format::BC4Unorm: return DXGI_FORMAT_BC4_UNORM;
			case Format::BC5Unorm: return DXGI_FORMAT_BC5_UNORM;
			case Format::BC6HUfloat: return DXGI_FORMAT_BC6H_UF16;
			case Format::BC7Unorm: return DXGI_FORMAT_BC7_UNORM;
			case Format::BC7UnormSrgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}

		// The typeless format of a family, for resources viewed in two formats (UNORM and sRGB).
		DXGI_FORMAT Typeless(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
			case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
			case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;
			case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_TYPELESS;
			case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;
			case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;
			default: return format;
			}
		}

		D3D12_RESOURCE_STATES ToD3D(ResourceState state)
		{
			switch (state)
			{
			case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
			case ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
			case ResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
			case ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
			case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
			default: return D3D12_RESOURCE_STATE_COMMON;
			}
		}

		D3D12_COMPARISON_FUNC ToD3D(CompareOp op)
		{
			switch (op)
			{
			case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
			case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
			case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
			case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
			case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
			case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			default: return D3D12_COMPARISON_FUNC_ALWAYS;
			}
		}

		ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device)
		{
			D3D12_ROOT_PARAMETER1 params[3]{};
			params[RootSlot_DrawConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[RootSlot_DrawConstants].Constants.ShaderRegister = 0;
			params[RootSlot_DrawConstants].Constants.Num32BitValues = kDrawConstantBytes / 4;
			params[RootSlot_DrawConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[RootSlot_ViewConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[RootSlot_ViewConstants].Descriptor.ShaderRegister = 1;
			params[RootSlot_ViewConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			params[RootSlot_ViewConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[RootSlot_FrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[RootSlot_FrameConstants].Descriptor.ShaderRegister = 2;
			params[RootSlot_FrameConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			params[RootSlot_FrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			auto sampler = [](UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode)
			{
				D3D12_STATIC_SAMPLER_DESC s{};
				s.ShaderRegister = reg;
				s.Filter = filter;
				s.AddressU = s.AddressV = s.AddressW = mode;
				s.MaxAnisotropy = 8;
				s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
				s.MaxLOD = D3D12_FLOAT32_MAX;
				s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				return s;
			};
			// s6: shadow-map comparisons (lit where the reference depth <= the stored depth), with
			// bilinear filtering of the results; outside the map counts as lit.
			D3D12_STATIC_SAMPLER_DESC shadowSampler = sampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
			shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
			shadowSampler.MaxAnisotropy = 1;
			const D3D12_STATIC_SAMPLER_DESC samplers[] =
			{
				sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_WRAP),
				sampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
				sampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
				sampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
				sampler(4, D3D12_FILTER_ANISOTROPIC,        D3D12_TEXTURE_ADDRESS_MODE_WRAP),
				sampler(5, D3D12_FILTER_ANISOTROPIC,        D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
				shadowSampler,
			};

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
			desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			desc.Desc_1_1.NumParameters = _countof(params);
			desc.Desc_1_1.pParameters = params;
			desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
			desc.Desc_1_1.pStaticSamplers = samplers;
			desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
				| D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> error;
			if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)))
			{
				const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown error";
				throw std::runtime_error(std::string("Root signature serialization failed: ") + message);
			}
			ComPtr<ID3D12RootSignature> rootSignature;
			DX_CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
			SetDebugName(rootSignature.Get(), "BindlessRootSignature");
			return rootSignature;
		}

		class D3D12Device;

		class D3D12Pipeline final : public Pipeline
		{
		public:
			D3D12Pipeline(ComPtr<ID3D12PipelineState> pso, bool compute, Topology topology) : mPso(std::move(pso))
			{
				mCompute = compute;
				mTopology = topology;
			}
			ID3D12PipelineState* Pso() const { return mPso.Get(); }
		private:
			ComPtr<ID3D12PipelineState> mPso;
		};

		class D3D12Buffer final : public Buffer
		{
		public:
			D3D12Buffer(D3D12Device& device, const BufferDesc& desc);
			~D3D12Buffer() override;
			void Upload(const void* data, uint64_t size, uint64_t dstOffset) override;
			ID3D12Resource* Resource() const { return mResource.Get(); }
		private:
			D3D12Device& mDevice;
			ComPtr<ID3D12Resource> mResource;
			DescriptorHandle mSrv, mUav;
		};

		class D3D12Texture final : public Texture
		{
		public:
			D3D12Texture(D3D12Device& device, const TextureDesc& desc);
			~D3D12Texture() override;
			void Upload(const SubresourceData* data, uint32_t count, ResourceState finalState) override;
			void UploadRegion(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
				const void* data, uint32_t bytesPerTexel) override;
			ID3D12Resource* Resource() const { return mResource.Get(); }
			D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const { return mRtv.cpu; }
			D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return mDsv.cpu; }
			void Barrier(ID3D12GraphicsCommandList* cmd, ResourceState state)
			{
				if (mState == state) return;
				D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(mResource.Get(), ToD3D(mState), ToD3D(state));
				cmd->ResourceBarrier(1, &barrier);
				mState = state;
			}
		private:
			D3D12Device& mDevice;
			ComPtr<ID3D12Resource> mResource;
			DescriptorHandle mSrv, mUav, mRtv, mDsv;
		};

		class D3D12CommandList final : public CommandList
		{
		public:
			explicit D3D12CommandList(D3D12Device& device) : mDevice(device) {}
			ComPtr<ID3D12GraphicsCommandList4> list;

			void BeginRendering(const RenderPassDesc& pass) override;
			void EndRendering() override {}
			void SetViewport(float x, float y, float width, float height) override
			{
				D3D12_VIEWPORT viewport{ x, y, width, height, 0.0f, 1.0f };
				list->RSSetViewports(1, &viewport);
			}
			void SetScissor(int32_t left, int32_t top, int32_t right, int32_t bottom) override
			{
				D3D12_RECT rect{ left, top, right, bottom };
				list->RSSetScissorRects(1, &rect);
			}
			void SetPipeline(Pipeline* pipeline) override
			{
				auto* p = static_cast<D3D12Pipeline*>(pipeline);
				list->SetPipelineState(p->Pso());
				mCompute = p->IsCompute();
				if (!mCompute)
				{
					list->IASetPrimitiveTopology(p->GetTopology() == Topology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				}
			}
			void SetConstants(const void* data, uint32_t bytes) override
			{
				if (mCompute) list->SetComputeRoot32BitConstants(RootSlot_DrawConstants, bytes / 4, data, 0);
				else list->SetGraphicsRoot32BitConstants(RootSlot_DrawConstants, bytes / 4, data, 0);
			}
			void SetViewConstants(const ConstantAllocation& constants) override
			{
				list->SetGraphicsRootConstantBufferView(RootSlot_ViewConstants, constants.gpuAddress);
				list->SetComputeRootConstantBufferView(RootSlot_ViewConstants, constants.gpuAddress);
			}
			void SetFrameConstants(const ConstantAllocation& constants) override
			{
				list->SetGraphicsRootConstantBufferView(RootSlot_FrameConstants, constants.gpuAddress);
				list->SetComputeRootConstantBufferView(RootSlot_FrameConstants, constants.gpuAddress);
			}
			void SetVertexBuffer(Buffer& buffer, uint32_t stride, uint64_t offset) override
			{
				ID3D12Resource* resource = static_cast<D3D12Buffer&>(buffer).Resource();
				D3D12_VERTEX_BUFFER_VIEW view{ resource->GetGPUVirtualAddress() + offset, static_cast<UINT>(buffer.Size() - offset), stride };
				list->IASetVertexBuffers(0, 1, &view);
			}
			void SetIndexBuffer(Buffer& buffer, bool sixteenBit, uint64_t offset) override
			{
				ID3D12Resource* resource = static_cast<D3D12Buffer&>(buffer).Resource();
				D3D12_INDEX_BUFFER_VIEW view{ resource->GetGPUVirtualAddress() + offset, static_cast<UINT>(buffer.Size() - offset),
					sixteenBit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
				list->IASetIndexBuffer(&view);
			}
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
			{
				list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
			}
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) override
			{
				list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
			}
			void Dispatch(uint32_t x, uint32_t y, uint32_t z) override { list->Dispatch(x, y, z); }
			void Barrier(Texture& texture, ResourceState before, ResourceState after) override
			{
				D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(static_cast<D3D12Texture&>(texture).Resource(), ToD3D(before), ToD3D(after));
				list->ResourceBarrier(1, &barrier);
			}
			void UavBarrier(Texture& texture) override
			{
				D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(static_cast<D3D12Texture&>(texture).Resource());
				list->ResourceBarrier(1, &barrier);
			}
			void Timestamp(uint32_t index) override;
			void ResolveTimestamps(uint32_t first, uint32_t count) override;
			void BeginEvent(const char*) override {}
			void EndEvent() override {}

			void ResetForFrame() { mCompute = false; }

		private:
			D3D12Device& mDevice;
			bool mCompute = false;
		};

		class D3D12Device final : public Device
		{
		public:
			~D3D12Device() override { Shutdown(); }
			bool Initialize(const DeviceOptions& options, HWND hwnd, uint32_t width, uint32_t height, std::string& error);
			void Shutdown();

			Backend GetBackend() const override { return Backend::D3D12; }
			ShaderTarget GetShaderTarget() const override { return ShaderTarget::Dxil; }
			const DeviceCaps& Caps() const override { return mCaps; }

			void BeginFrame() override;
			CommandList& Cmd() override { return mCmd; }
			void EndFrame(bool vsync) override;
			uint32_t FrameIndex() const override { return mFrameIndex; }
			uint64_t FrameNumber() const override { return mFrameNumber; }
			void WaitIdle() override;

			void ResizeSwapChain(uint32_t width, uint32_t height) override;
			uint32_t SwapChainWidth() const override { return mSwapWidth; }
			uint32_t SwapChainHeight() const override { return mSwapHeight; }
			Format SwapChainFormat() const override { return Format::RGBA8Unorm; }

			std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) override { return std::make_unique<D3D12Buffer>(*this, desc); }
			std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) override { return std::make_unique<D3D12Texture>(*this, desc); }
			std::shared_ptr<Pipeline> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const std::vector<uint8_t>& vs,
				const std::vector<uint8_t>& ps, const std::string& name, std::string& error) override;
			std::shared_ptr<Pipeline> CreateComputePipeline(const std::vector<uint8_t>& cs, const std::string& name, std::string& error) override;

			void DeferRelease(std::shared_ptr<void> object) override
			{
				if (!object) return;
				std::lock_guard lock(mReleaseMutex);
				mPendingReleases.push_back({ std::move(object), {}, mNextFenceValue });
			}
			void DeferRelease(ComPtr<IUnknown> object)
			{
				if (!object) return;
				std::lock_guard lock(mReleaseMutex);
				mPendingReleases.push_back({ {}, std::move(object), mNextFenceValue });
			}
			void FreeDescriptor(DescriptorHeap& heap, DescriptorHandle& handle) { heap.Free(handle, mNextFenceValue); }

			ConstantAllocation AllocateConstants(uint32_t size) override;
			const uint64_t* Timestamps() const override { return mTimestampsMapped; }
			double TimestampTicksToMs() const override { return mTicksToMs; }
			uint32_t DescriptorsInUse() const override { return mSrvHeap.Allocated(); }
			uint32_t DebugErrorCount() const override { return mDebugErrors; }
			uint32_t DebugWarningCount() const override { return mDebugWarnings; }

			ID3D12Device5* D3D() const { return mDevice.Get(); }
			DescriptorHeap& SrvHeap() { return mSrvHeap; }
			DescriptorHeap& RtvHeap() { return mRtvHeap; }
			DescriptorHeap& DsvHeap() { return mDsvHeap; }
			ID3D12QueryHeap* QueryHeap() const { return mQueryHeap.Get(); }
			ID3D12Resource* TimestampReadback() const { return mTimestampReadback.Get(); }

			// Records `record` into a one-off command list and submits it without waiting: the queue
			// runs it before any frame submitted later. `keepAlive` (the staging buffer) is released
			// once the GPU has run it.
			void ExecuteAsync(const std::function<void(ID3D12GraphicsCommandList4*)>& record, ComPtr<IUnknown> keepAlive);

			// The back buffer as a render target (BeginRendering with backBuffer).
			D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv();

		private:
			struct PendingRelease
			{
				std::shared_ptr<void> object;
				ComPtr<IUnknown> com;
				uint64_t fenceValue;
			};
			struct AsyncContext
			{
				ComPtr<ID3D12CommandAllocator> allocator;
				ComPtr<ID3D12GraphicsCommandList4> list;
				uint64_t fenceValue = 0;
			};

			void CreateAdapterAndDevice(const DeviceOptions& options);
			bool CheckCaps(std::string& error);
			void CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height);
			void CreateBackBufferViews();
			void WaitForFence(ID3D12Fence* fence, uint64_t value);
			void ProcessDeferred(uint64_t completedValue);
			void ProcessAsync(uint64_t completedValue);
			static void CALLBACK OnDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* context);

			ComPtr<IDXGIFactory6> mFactory;
			ComPtr<IDXGIAdapter1> mAdapter;
			ComPtr<ID3D12Device5> mDevice;
			ComPtr<ID3D12CommandQueue> mQueue;
			ComPtr<ID3D12RootSignature> mRootSignature;
			D3D12CommandList mCmd{ *this };
			ComPtr<ID3D12Fence> mFence;
			HANDLE mFenceEvent = nullptr;
			std::array<ComPtr<ID3D12CommandAllocator>, kFramesInFlight> mAllocators;
			std::array<uint64_t, kFramesInFlight> mFrameFenceValues{};
			uint64_t mNextFenceValue = 1;
			uint32_t mFrameIndex = 0;
			uint64_t mFrameNumber = 0;

			ComPtr<ID3D12Fence> mAsyncFence;
			uint64_t mAsyncFenceValue = 0;
			std::vector<AsyncContext> mAsync;
			std::vector<std::pair<ComPtr<IUnknown>, uint64_t>> mAsyncReleases;
			std::mutex mAsyncMutex;

			DescriptorHeap mSrvHeap, mRtvHeap, mDsvHeap;
			std::mutex mReleaseMutex;
			std::vector<PendingRelease> mPendingReleases;

			// Swap chain.
			ComPtr<IDXGISwapChain3> mSwapChain;
			std::array<ComPtr<ID3D12Resource>, kSwapBufferCount> mBackBuffers;
			std::array<DescriptorHandle, kSwapBufferCount> mBackBufferRtvs;
			uint32_t mSwapWidth = 0, mSwapHeight = 0;
			bool mBackBufferIsTarget = false;

			// Constant ring.
			ComPtr<ID3D12Resource> mConstants;
			uint8_t* mConstantsMapped = nullptr;
			uint64_t mConstantOffset = 0;

			// Timestamps.
			ComPtr<ID3D12QueryHeap> mQueryHeap;
			ComPtr<ID3D12Resource> mTimestampReadback;
			uint64_t* mTimestampsMapped = nullptr;
			double mTicksToMs = 0.0;

			DeviceCaps mCaps;
			DWORD mDebugCallbackCookie = 0;
			uint32_t mDebugErrors = 0, mDebugWarnings = 0;
			bool mInitialized = false;
		};

		// =========================================================================================
		// Device
		// =========================================================================================

		bool D3D12Device::Initialize(const DeviceOptions& options, HWND hwnd, uint32_t width, uint32_t height, std::string& error)
		{
			CreateAdapterAndDevice(options);
			if (!mDevice)
			{
				error = "No Direct3D 12 GPU with feature level 12_0 was found.";
				return false;
			}
			if (!CheckCaps(error)) return false;
			if (options.debugLayer)
			{
				ComPtr<ID3D12InfoQueue1> infoQueue1;
				if (SUCCEEDED(mDevice.As(&infoQueue1)))
					infoQueue1->RegisterMessageCallback(&D3D12Device::OnDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &mDebugCallbackCookie);
				else
					CHERRY_WARN("ID3D12InfoQueue1 unavailable; debug layer messages only go to the debugger output");
			}

			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			DX_CHECK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mQueue)));
			SetDebugName(mQueue.Get(), "DirectQueue");
			for (auto& allocator : mAllocators)
				DX_CHECK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
			DX_CHECK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mAllocators[0].Get(), nullptr, IID_PPV_ARGS(&mCmd.list)));
			DX_CHECK(mCmd.list->Close());
			SetDebugName(mCmd.list.Get(), "FrameCommandList");
			DX_CHECK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
			DX_CHECK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mAsyncFence)));
			mFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

			mSrvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapCapacity, true, "BindlessHeap");
			mRtvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity, false, "RtvHeap");
			mDsvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity, false, "DsvHeap");
			mRootSignature = CreateBindlessRootSignature(mDevice.Get());

			// The constant ring: kConstantBytesPerFrame per frame slot, persistently mapped.
			{
				CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
				CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kConstantBytesPerFrame * kFramesInFlight);
				DX_CHECK(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstants)));
				SetDebugName(mConstants.Get(), "ConstantRing");
				D3D12_RANGE noRead{ 0, 0 };
				DX_CHECK(mConstants->Map(0, &noRead, reinterpret_cast<void**>(&mConstantsMapped)));
			}

			// Timestamp queries and their readback buffer.
			{
				D3D12_QUERY_HEAP_DESC desc{};
				desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
				desc.Count = kTimestampCount;
				DX_CHECK(mDevice->CreateQueryHeap(&desc, IID_PPV_ARGS(&mQueryHeap)));
				CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
				CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * kTimestampCount);
				DX_CHECK(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mTimestampReadback)));
				SetDebugName(mTimestampReadback.Get(), "TimestampReadback");
				DX_CHECK(mTimestampReadback->Map(0, nullptr, reinterpret_cast<void**>(&mTimestampsMapped)));
				uint64_t frequency = 1;
				mQueue->GetTimestampFrequency(&frequency);
				mTicksToMs = 1000.0 / static_cast<double>(frequency);
			}

			CreateSwapChain(hwnd, width, height);
			mInitialized = true;
			return true;
		}

		void D3D12Device::CreateAdapterAndDevice(const DeviceOptions& options)
		{
			if (options.debugLayer)
			{
				ComPtr<ID3D12Debug1> debug;
				if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
				{
					debug->EnableDebugLayer();
					if (options.gpuBasedValidation)
					{
						debug->SetEnableGPUBasedValidation(TRUE);
						CHERRY_LOG("D3D12 GPU-based validation enabled");
					}
				}
				else
				{
					CHERRY_WARN("D3D12 debug layer requested but not available (install Graphics Tools)");
				}
			}
			const UINT factoryFlags = options.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
			DX_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&mFactory)));
			for (UINT i = 0; ; ++i)
			{
				ComPtr<IDXGIAdapter1> adapter;
				if (mFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
				if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice))))
				{
					mAdapter = adapter;
					mCaps.adapterName = WideToUtf8(desc.Description);
					break;
				}
			}
		}

		bool D3D12Device::CheckCaps(std::string& error)
		{
			D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_6 };
			if (FAILED(mDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
				|| shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6)
			{
				error = "GPU/driver does not support Shader Model 6.6 (required for bindless resources): " + mCaps.adapterName;
				return false;
			}
			D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
			mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
			if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
			{
				error = "GPU does not support resource binding tier 3 (required for bindless resources): " + mCaps.adapterName;
				return false;
			}
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
			mDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
			mCaps.raytracing = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
			BOOL tearing = FALSE;
			if (SUCCEEDED(mFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
				mCaps.tearing = tearing == TRUE;
			CHERRY_LOG("GPU: %s | D3D12 | SM 6.6: yes | binding tier 3: yes | DXR: %s | tearing: %s",
				mCaps.adapterName.c_str(), mCaps.raytracing ? "yes" : "no", mCaps.tearing ? "yes" : "no");
			return true;
		}

		void CALLBACK D3D12Device::OnDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* context)
		{
			// Performance hints only: render targets are cleared to a colour that can change at runtime.
			if (id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
				id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
				return;
			auto* self = static_cast<D3D12Device*>(context);
			switch (severity)
			{
			case D3D12_MESSAGE_SEVERITY_CORRUPTION:
			case D3D12_MESSAGE_SEVERITY_ERROR:
				++self->mDebugErrors;
				CHERRY_ERROR("D3D12: %s", description);
				break;
			case D3D12_MESSAGE_SEVERITY_WARNING:
				++self->mDebugWarnings;
				CHERRY_WARN("D3D12: %s", description);
				break;
			default:
				break;
			}
		}

		void D3D12Device::CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height)
		{
			mSwapWidth = std::max(width, 1u);
			mSwapHeight = std::max(height, 1u);
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = mSwapWidth;
			desc.Height = mSwapHeight;
			desc.Format = kSwapFormat;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = kSwapBufferCount;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.Flags = mCaps.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
			ComPtr<IDXGISwapChain1> swapChain1;
			DX_CHECK(mFactory->CreateSwapChainForHwnd(mQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1));
			DX_CHECK(swapChain1.As(&mSwapChain));
			mFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
			for (auto& rtv : mBackBufferRtvs) rtv = mRtvHeap.Allocate();
			CreateBackBufferViews();
		}

		void D3D12Device::CreateBackBufferViews()
		{
			for (uint32_t i = 0; i < kSwapBufferCount; ++i)
			{
				DX_CHECK(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mBackBuffers[i])));
				SetDebugName(mBackBuffers[i].Get(), "BackBuffer");
				mDevice->CreateRenderTargetView(mBackBuffers[i].Get(), nullptr, mBackBufferRtvs[i].cpu);
			}
		}

		void D3D12Device::ResizeSwapChain(uint32_t width, uint32_t height)
		{
			if (width == 0 || height == 0 || (width == mSwapWidth && height == mSwapHeight)) return;
			WaitIdle();
			for (auto& buffer : mBackBuffers) buffer.Reset();
			DX_CHECK(mSwapChain->ResizeBuffers(kSwapBufferCount, width, height, kSwapFormat, mCaps.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
			mSwapWidth = width;
			mSwapHeight = height;
			CreateBackBufferViews();
		}

		D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::BackBufferRtv()
		{
			const UINT index = mSwapChain->GetCurrentBackBufferIndex();
			if (!mBackBufferIsTarget)
			{
				D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(mBackBuffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				mCmd.list->ResourceBarrier(1, &barrier);
				mBackBufferIsTarget = true;
			}
			return mBackBufferRtvs[index].cpu;
		}

		void D3D12Device::Shutdown()
		{
			if (!mInitialized) return;
			WaitIdle();
			ProcessDeferred(UINT64_MAX);
			for (auto& buffer : mBackBuffers) buffer.Reset();
			for (auto& rtv : mBackBufferRtvs) mRtvHeap.Free(rtv, 0);
			mSwapChain.Reset();
			if (mConstants && mConstantsMapped) mConstants->Unmap(0, nullptr);
			mConstants.Reset();
			if (mTimestampReadback && mTimestampsMapped) mTimestampReadback->Unmap(0, nullptr);
			mTimestampReadback.Reset();
			mQueryHeap.Reset();
			mSrvHeap.Shutdown();
			mRtvHeap.Shutdown();
			mDsvHeap.Shutdown();
			mCmd.list.Reset();
			ProcessAsync(UINT64_MAX);
			mAsync.clear();
			for (auto& allocator : mAllocators) allocator.Reset();
			mFence.Reset();
			mAsyncFence.Reset();
			mRootSignature.Reset();
			mQueue.Reset();
			if (mFenceEvent) CloseHandle(mFenceEvent);
			mFenceEvent = nullptr;
			if (mDebugCallbackCookie)
			{
				ComPtr<ID3D12InfoQueue1> infoQueue1;
				if (SUCCEEDED(mDevice.As(&infoQueue1))) infoQueue1->UnregisterMessageCallback(mDebugCallbackCookie);
				mDebugCallbackCookie = 0;
			}
			mDevice.Reset();
			mAdapter.Reset();
			mFactory.Reset();
			mInitialized = false;
		}

		void D3D12Device::WaitForFence(ID3D12Fence* fence, uint64_t value)
		{
			if (fence->GetCompletedValue() < value)
			{
				DX_CHECK(fence->SetEventOnCompletion(value, mFenceEvent));
				WaitForSingleObject(mFenceEvent, INFINITE);
			}
		}

		void D3D12Device::BeginFrame()
		{
			WaitForFence(mFence.Get(), mFrameFenceValues[mFrameIndex]);
			ProcessDeferred(mFence->GetCompletedValue());
			ProcessAsync(mAsyncFence->GetCompletedValue());
			DX_CHECK(mAllocators[mFrameIndex]->Reset());
			DX_CHECK(mCmd.list->Reset(mAllocators[mFrameIndex].Get(), nullptr));
			mCmd.ResetForFrame();
			mConstantOffset = 0;
			mBackBufferIsTarget = false;

			// Directly-indexed heaps must be bound before the root signature.
			ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Heap() };
			mCmd.list->SetDescriptorHeaps(1, heaps);
			mCmd.list->SetGraphicsRootSignature(mRootSignature.Get());
			mCmd.list->SetComputeRootSignature(mRootSignature.Get());
		}

		void D3D12Device::EndFrame(bool vsync)
		{
			if (mBackBufferIsTarget)
			{
				D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(mBackBuffers[mSwapChain->GetCurrentBackBufferIndex()].Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
				mCmd.list->ResourceBarrier(1, &barrier);
				mBackBufferIsTarget = false;
			}
			DX_CHECK(mCmd.list->Close());
			ID3D12CommandList* lists[] = { mCmd.list.Get() };
			mQueue->ExecuteCommandLists(1, lists);
			const UINT flags = (!vsync && mCaps.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
			DX_CHECK(mSwapChain->Present(vsync ? 1 : 0, flags));
			mFrameFenceValues[mFrameIndex] = mNextFenceValue++;
			DX_CHECK(mQueue->Signal(mFence.Get(), mFrameFenceValues[mFrameIndex]));
			mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
			++mFrameNumber;
		}

		void D3D12Device::WaitIdle()
		{
			if (!mQueue) return;
			const uint64_t value = mNextFenceValue++;
			DX_CHECK(mQueue->Signal(mFence.Get(), value));
			WaitForFence(mFence.Get(), value);
			ProcessDeferred(mFence->GetCompletedValue());
		}

		ConstantAllocation D3D12Device::AllocateConstants(uint32_t size)
		{
			const uint64_t aligned = (mConstantOffset + 255) & ~uint64_t(255);
			if (aligned + size > kConstantBytesPerFrame) throw std::runtime_error("Constant ring out of memory for this frame");
			mConstantOffset = aligned + size;
			const uint64_t offset = mFrameIndex * kConstantBytesPerFrame + aligned;
			ConstantAllocation allocation;
			allocation.cpu = mConstantsMapped + offset;
			allocation.offset = offset;
			allocation.gpuAddress = mConstants->GetGPUVirtualAddress() + offset;
			allocation.size = size;
			return allocation;
		}

		void D3D12Device::ExecuteAsync(const std::function<void(ID3D12GraphicsCommandList4*)>& record, ComPtr<IUnknown> keepAlive)
		{
			std::lock_guard lock(mAsyncMutex);
			// A command list the GPU has finished with, or a new one. Many uploads in flight at
			// once (a burst of mesh rebuilds) wait for the oldest rather than growing forever.
			constexpr size_t kMaxContexts = 64;
			const uint64_t completed = mAsyncFence->GetCompletedValue();
			AsyncContext* context = nullptr;
			for (AsyncContext& c : mAsync)
				if (c.fenceValue <= completed) { context = &c; break; }
			if (!context && mAsync.size() >= kMaxContexts)
			{
				AsyncContext* oldest = &mAsync.front();
				for (AsyncContext& c : mAsync)
					if (c.fenceValue < oldest->fenceValue) oldest = &c;
				WaitForFence(mAsyncFence.Get(), oldest->fenceValue);
				context = oldest;
			}
			if (!context)
			{
				AsyncContext c;
				DX_CHECK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator)));
				DX_CHECK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list)));
				DX_CHECK(c.list->Close());
				SetDebugName(c.list.Get(), "AsyncUploadCommandList");
				mAsync.push_back(std::move(c));
				context = &mAsync.back();
			}
			DX_CHECK(context->allocator->Reset());
			DX_CHECK(context->list->Reset(context->allocator.Get(), nullptr));
			record(context->list.Get());
			DX_CHECK(context->list->Close());
			ID3D12CommandList* lists[] = { context->list.Get() };
			mQueue->ExecuteCommandLists(1, lists);
			context->fenceValue = ++mAsyncFenceValue;
			DX_CHECK(mQueue->Signal(mAsyncFence.Get(), context->fenceValue));
			if (keepAlive) mAsyncReleases.push_back({ std::move(keepAlive), context->fenceValue });
		}

		void D3D12Device::ProcessAsync(uint64_t completedValue)
		{
			std::lock_guard lock(mAsyncMutex);
			mAsyncReleases.erase(std::remove_if(mAsyncReleases.begin(), mAsyncReleases.end(),
				[&](const auto& release) { return release.second <= completedValue; }), mAsyncReleases.end());
		}

		void D3D12Device::ProcessDeferred(uint64_t completedValue)
		{
			std::vector<PendingRelease> done;
			{
				std::lock_guard lock(mReleaseMutex);
				auto split = std::stable_partition(mPendingReleases.begin(), mPendingReleases.end(),
					[&](const PendingRelease& release) { return release.fenceValue > completedValue; });
				done.assign(std::make_move_iterator(split), std::make_move_iterator(mPendingReleases.end()));
				mPendingReleases.erase(split, mPendingReleases.end());
			}
			done.clear();   // releases outside the lock (a released object may defer more)
			mSrvHeap.ProcessDeferredFrees(completedValue);
			mRtvHeap.ProcessDeferredFrees(completedValue);
			mDsvHeap.ProcessDeferredFrees(completedValue);
		}

		std::shared_ptr<Pipeline> D3D12Device::CreateComputePipeline(const std::vector<uint8_t>& cs, const std::string& name, std::string& error)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = mRootSignature.Get();
			psoDesc.CS = { cs.data(), cs.size() };
			ComPtr<ID3D12PipelineState> pso;
			const HRESULT hr = mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				error = "CreateComputePipelineState failed: " + HResultToString(hr);
				return nullptr;
			}
			SetDebugName(pso.Get(), name.c_str());
			return std::make_shared<D3D12Pipeline>(std::move(pso), true, Topology::Triangles);
		}

		std::shared_ptr<Pipeline> D3D12Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc, const std::vector<uint8_t>& vs,
			const std::vector<uint8_t>& ps, const std::string& name, std::string& error)
		{
			static const D3D12_INPUT_ELEMENT_DESC kMeshLayout[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};
			// MeshFormat::Compact: the input assembler widens these to the same shader inputs.
			static const D3D12_INPUT_ELEMENT_DESC kMeshCompactLayout[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R8G8B8A8_SNORM,     0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,       0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};
			static const D3D12_INPUT_ELEMENT_DESC kImGuiLayout[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = mRootSignature.Get();
			psoDesc.VS = { vs.data(), vs.size() };
			if (!ps.empty()) psoDesc.PS = { ps.data(), ps.size() };
			switch (desc.vertexLayout)
			{
			case VertexLayout::Mesh: psoDesc.InputLayout = { kMeshLayout, _countof(kMeshLayout) }; break;
			case VertexLayout::MeshCompact: psoDesc.InputLayout = { kMeshCompactLayout, _countof(kMeshCompactLayout) }; break;
			case VertexLayout::ImGui: psoDesc.InputLayout = { kImGuiLayout, _countof(kImGuiLayout) }; break;
			default: break;
			}

			psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
			psoDesc.RasterizerState.CullMode = desc.cullMode == CullMode::None ? D3D12_CULL_MODE_NONE
				: desc.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
			psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
			psoDesc.RasterizerState.DepthBias = desc.depthBias;
			psoDesc.RasterizerState.SlopeScaledDepthBias = desc.slopeScaledDepthBias;
			psoDesc.RasterizerState.DepthBiasClamp = desc.depthBiasClamp;

			psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
			auto& rt = psoDesc.BlendState.RenderTarget[0];
			if (desc.alphaBlend)
			{
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ONE;
				rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			}
			else if (desc.additiveBlend)
			{
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rt.DestBlend = D3D12_BLEND_ONE;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
				rt.DestBlendAlpha = D3D12_BLEND_ONE;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			}

			psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
			psoDesc.DepthStencilState.DepthEnable = desc.depthTest && desc.depthFormat != Format::Unknown ? TRUE : FALSE;
			psoDesc.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
			psoDesc.DepthStencilState.DepthFunc = ToD3D(desc.depthFunc);
			psoDesc.DSVFormat = ToDxgi(desc.depthFormat);
			psoDesc.SampleMask = UINT_MAX;
			psoDesc.PrimitiveTopologyType = desc.topology == Topology::Lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			psoDesc.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());
			for (size_t i = 0; i < desc.colorFormats.size(); ++i) psoDesc.RTVFormats[i] = ToDxgi(desc.colorFormats[i]);
			psoDesc.SampleDesc.Count = 1;

			ComPtr<ID3D12PipelineState> pso;
			const HRESULT hr = mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				error = "CreateGraphicsPipelineState failed: " + HResultToString(hr);
				return nullptr;
			}
			SetDebugName(pso.Get(), name.c_str());
			return std::make_shared<D3D12Pipeline>(std::move(pso), false, desc.topology);
		}

		// =========================================================================================
		// Command list
		// =========================================================================================

		void D3D12CommandList::BeginRendering(const RenderPassDesc& pass)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
			uint32_t width = 1, height = 1;
			const bool hasColor = pass.color || pass.backBuffer;
			if (pass.backBuffer)
			{
				rtv = mDevice.BackBufferRtv();
				width = mDevice.SwapChainWidth();
				height = mDevice.SwapChainHeight();
			}
			else if (pass.color)
			{
				rtv = static_cast<D3D12Texture*>(pass.color)->Rtv();
				width = pass.color->Width();
				height = pass.color->Height();
			}
			if (pass.depth)
			{
				dsv = static_cast<D3D12Texture*>(pass.depth)->Dsv();
				if (!hasColor)
				{
					width = pass.depth->Width();
					height = pass.depth->Height();
				}
			}
			if (hasColor && pass.clearColor) list->ClearRenderTargetView(rtv, pass.clearValue, 0, nullptr);
			if (pass.depth && pass.clearDepth) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, pass.depthValue, 0, 0, nullptr);
			list->OMSetRenderTargets(hasColor ? 1 : 0, hasColor ? &rtv : nullptr, FALSE, pass.depth ? &dsv : nullptr);
			SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
			SetScissor(0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
		}

		void D3D12CommandList::Timestamp(uint32_t index)
		{
			list->EndQuery(mDevice.QueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, index);
		}

		void D3D12CommandList::ResolveTimestamps(uint32_t first, uint32_t count)
		{
			list->ResolveQueryData(mDevice.QueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, mDevice.TimestampReadback(), sizeof(uint64_t) * first);
		}

		// =========================================================================================
		// Buffer
		// =========================================================================================

		D3D12Buffer::D3D12Buffer(D3D12Device& device, const BufferDesc& desc) : mDevice(device)
		{
			mDesc = desc;
			const bool upload = desc.heap == HeapKind::Upload;
			CD3DX12_HEAP_PROPERTIES heapProps(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
			const D3D12_RESOURCE_FLAGS flags = desc.createUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
			CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.size, flags);
			DX_CHECK(device.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
				upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mResource)));
			SetDebugName(mResource.Get(), desc.name);
			if (upload)
			{
				D3D12_RANGE noRead{ 0, 0 };
				DX_CHECK(mResource->Map(0, &noRead, &mMapped));
			}
			const bool structured = desc.stride > 0;
			if (desc.createSrv)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				if (structured)
				{
					srv.Format = DXGI_FORMAT_UNKNOWN;
					srv.Buffer.NumElements = static_cast<UINT>(desc.size / desc.stride);
					srv.Buffer.StructureByteStride = desc.stride;
				}
				else
				{
					srv.Format = DXGI_FORMAT_R32_TYPELESS;
					srv.Buffer.NumElements = static_cast<UINT>(desc.size / 4);
					srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				}
				mSrv = device.SrvHeap().Allocate();
				mSrvIndex = mSrv.index;
				device.D3D()->CreateShaderResourceView(mResource.Get(), &srv, mSrv.cpu);
			}
			if (desc.createUav)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				if (structured)
				{
					uav.Format = DXGI_FORMAT_UNKNOWN;
					uav.Buffer.NumElements = static_cast<UINT>(desc.size / desc.stride);
					uav.Buffer.StructureByteStride = desc.stride;
				}
				else
				{
					uav.Format = DXGI_FORMAT_R32_TYPELESS;
					uav.Buffer.NumElements = static_cast<UINT>(desc.size / 4);
					uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
				}
				mUav = device.SrvHeap().Allocate();
				mUavIndex = mUav.index;
				device.D3D()->CreateUnorderedAccessView(mResource.Get(), nullptr, &uav, mUav.cpu);
			}
		}

		D3D12Buffer::~D3D12Buffer()
		{
			if (mMapped) mResource->Unmap(0, nullptr);
			mDevice.FreeDescriptor(mDevice.SrvHeap(), mSrv);
			mDevice.FreeDescriptor(mDevice.SrvHeap(), mUav);
			mDevice.DeferRelease(ComPtr<IUnknown>(mResource));
		}

		void D3D12Buffer::Upload(const void* data, uint64_t size, uint64_t dstOffset)
		{
			if (mMapped)
			{
				std::memcpy(static_cast<uint8_t*>(mMapped) + dstOffset, data, static_cast<size_t>(size));
				return;
			}
			ComPtr<ID3D12Resource> staging;
			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
			CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
			DX_CHECK(mDevice.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
			void* mapped = nullptr;
			D3D12_RANGE noRead{ 0, 0 };
			DX_CHECK(staging->Map(0, &noRead, &mapped));
			std::memcpy(mapped, data, static_cast<size_t>(size));
			staging->Unmap(0, nullptr);
			// Not waited for: the queue copies before any later frame reads the buffer.
			mDevice.ExecuteAsync([&](ID3D12GraphicsCommandList4* cmd)
			{
				// COMMON -> COPY_DEST is an implicit promotion for buffers.
				cmd->CopyBufferRegion(mResource.Get(), dstOffset, staging.Get(), 0, size);
			}, staging);
		}

		// =========================================================================================
		// Texture
		// =========================================================================================

		D3D12Texture::D3D12Texture(D3D12Device& device, const TextureDesc& desc) : mDevice(device)
		{
			mDesc = desc;
			const bool is3D = desc.dimension == TextureDimension::Texture3D;
			const bool depth = desc.format == Format::D32Float;
			const DXGI_FORMAT format = ToDxgi(desc.format);
			const DXGI_FORMAT srvFormat = depth ? DXGI_FORMAT_R32_FLOAT : ToDxgi(desc.srvFormat != Format::Unknown ? desc.srvFormat : desc.format);

			D3D12_RESOURCE_DESC resourceDesc{};
			resourceDesc.Dimension = is3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			resourceDesc.Width = desc.width;
			resourceDesc.Height = desc.height;
			resourceDesc.DepthOrArraySize = static_cast<UINT16>(is3D ? desc.depth : 1);
			resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
			resourceDesc.Format = depth && (desc.usage & TextureUsage_ShaderResource) ? DXGI_FORMAT_R32_TYPELESS
				: srvFormat != format ? Typeless(format) : format;
			resourceDesc.SampleDesc.Count = 1;
			resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			if (desc.usage & TextureUsage_RenderTarget)    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (desc.usage & TextureUsage_DepthStencil)    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			if (desc.usage & TextureUsage_UnorderedAccess) resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			D3D12_CLEAR_VALUE clear{};
			const D3D12_CLEAR_VALUE* clearPtr = nullptr;
			if (desc.usage & TextureUsage_DepthStencil)
			{
				clear.Format = DXGI_FORMAT_D32_FLOAT;
				clear.DepthStencil.Depth = 1.0f;
				clearPtr = &clear;
			}
			else if (desc.usage & TextureUsage_RenderTarget)
			{
				clear.Format = format;
				std::memcpy(clear.Color, desc.clearColor, sizeof(clear.Color));
				clearPtr = &clear;
			}
			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
			DX_CHECK(device.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
				ToD3D(desc.initialState), clearPtr, IID_PPV_ARGS(&mResource)));
			SetDebugName(mResource.Get(), desc.name);
			mState = desc.initialState;

			ID3D12Device* d3d = device.D3D();
			if (desc.usage & TextureUsage_ShaderResource)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Format = srvFormat;
				if (is3D)
				{
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
					srv.Texture3D.MipLevels = desc.mipLevels;
				}
				else
				{
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					srv.Texture2D.MipLevels = desc.mipLevels;
				}
				mSrv = device.SrvHeap().Allocate();
				mSrvIndex = mSrv.index;
				d3d->CreateShaderResourceView(mResource.Get(), &srv, mSrv.cpu);
			}
			if (desc.usage & TextureUsage_UnorderedAccess)
			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = format;
				if (is3D)
				{
					uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
					uav.Texture3D.WSize = desc.depth;
				}
				else
				{
					uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				}
				mUav = device.SrvHeap().Allocate();
				mUavIndex = mUav.index;
				d3d->CreateUnorderedAccessView(mResource.Get(), nullptr, &uav, mUav.cpu);
			}
			if (desc.usage & TextureUsage_RenderTarget)
			{
				D3D12_RENDER_TARGET_VIEW_DESC rtv{};
				rtv.Format = format;
				rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				mRtv = device.RtvHeap().Allocate();
				d3d->CreateRenderTargetView(mResource.Get(), &rtv, mRtv.cpu);
			}
			if (desc.usage & TextureUsage_DepthStencil)
			{
				D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
				dsv.Format = DXGI_FORMAT_D32_FLOAT;
				dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				mDsv = device.DsvHeap().Allocate();
				d3d->CreateDepthStencilView(mResource.Get(), &dsv, mDsv.cpu);
			}
		}

		D3D12Texture::~D3D12Texture()
		{
			mDevice.FreeDescriptor(mDevice.SrvHeap(), mSrv);
			mDevice.FreeDescriptor(mDevice.SrvHeap(), mUav);
			mDevice.FreeDescriptor(mDevice.RtvHeap(), mRtv);
			mDevice.FreeDescriptor(mDevice.DsvHeap(), mDsv);
			mDevice.DeferRelease(ComPtr<IUnknown>(mResource));
		}

		void D3D12Texture::Upload(const SubresourceData* data, uint32_t count, ResourceState finalState)
		{
			const UINT64 stagingSize = GetRequiredIntermediateSize(mResource.Get(), 0, count);
			ComPtr<ID3D12Resource> staging;
			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
			CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(stagingSize);
			DX_CHECK(mDevice.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
			std::vector<D3D12_SUBRESOURCE_DATA> subresources(count);
			for (uint32_t i = 0; i < count; ++i)
				subresources[i] = { data[i].data, static_cast<LONG_PTR>(data[i].rowPitch), static_cast<LONG_PTR>(data[i].slicePitch) };
			// UpdateSubresources copies the data into the staging buffer while recording, so the
			// submission isn't waited for.
			mDevice.ExecuteAsync([&](ID3D12GraphicsCommandList4* cmd)
			{
				Barrier(cmd, ResourceState::CopyDest);
				UpdateSubresources(cmd, mResource.Get(), staging.Get(), 0, 0, count, subresources.data());
				Barrier(cmd, finalState);
			}, staging);
		}

		void D3D12Texture::UploadRegion(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
			const void* data, uint32_t bytesPerTexel)
		{
			if (width == 0 || height == 0 || depth == 0) return;
			// Rows into a staging buffer laid out with D3D12's row-pitch alignment.
			const uint32_t rowBytes = width * bytesPerTexel;
			const uint32_t rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
			const uint64_t stagingSize = static_cast<uint64_t>(rowPitch) * height * depth;
			ComPtr<ID3D12Resource> staging;
			CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
			CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(stagingSize);
			DX_CHECK(mDevice.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
			uint8_t* mapped = nullptr;
			D3D12_RANGE noRead{ 0, 0 };
			DX_CHECK(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
			const uint8_t* src = static_cast<const uint8_t*>(data);
			for (uint32_t slice = 0; slice < depth; ++slice)
				for (uint32_t row = 0; row < height; ++row)
					std::memcpy(mapped + (static_cast<uint64_t>(slice) * height + row) * rowPitch,
						src + (static_cast<uint64_t>(slice) * height + row) * rowBytes, rowBytes);
			staging->Unmap(0, nullptr);

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			footprint.Footprint.Format = ToDxgi(mDesc.format);
			footprint.Footprint.Width = width;
			footprint.Footprint.Height = height;
			footprint.Footprint.Depth = depth;
			footprint.Footprint.RowPitch = rowPitch;
			const ResourceState restore = mState;
			mDevice.ExecuteAsync([&](ID3D12GraphicsCommandList4* cmd)
			{
				Barrier(cmd, ResourceState::CopyDest);
				CD3DX12_TEXTURE_COPY_LOCATION dst(mResource.Get(), 0);
				CD3DX12_TEXTURE_COPY_LOCATION srcLocation(staging.Get(), footprint);
				cmd->CopyTextureRegion(&dst, x, y, z, &srcLocation, nullptr);
				Barrier(cmd, restore);
			}, staging);
		}
	}

	std::unique_ptr<Device> CreateD3D12Device(const DeviceOptions& options, void* nativeWindow, uint32_t width, uint32_t height, std::string& error)
	{
		auto device = std::make_unique<D3D12Device>();
		try
		{
			if (!device->Initialize(options, static_cast<HWND>(nativeWindow), width, height, error)) return nullptr;
		}
		catch (const std::exception& e)
		{
			error = e.what();
			return nullptr;
		}
		return device;
	}
}
