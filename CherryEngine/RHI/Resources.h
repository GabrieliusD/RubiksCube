#pragma once
#include "RHI/D3DCommon.h"
#include "RHI/DescriptorHeap.h"
#include <memory>

namespace rhi
{
	class Device;

	// ---------------------------------------------------------------------------
	// Buffer
	// ---------------------------------------------------------------------------
	enum class HeapKind { Default, Upload };

	struct BufferDesc
	{
		uint64_t size = 0;
		uint32_t stride = 0;        // > 0 for structured buffers (SRV/UAV view stride)
		HeapKind heap = HeapKind::Default;
		bool createSrv = false;     // structured SRV if stride > 0, otherwise raw (ByteAddressBuffer)
		bool createUav = false;
		const char* name = "Buffer";
	};

	// Default-heap buffers live in COMMON and rely on implicit state promotion and
	// decay, so they need no explicit barriers for copy -> read usage.
	// Upload-heap buffers are persistently mapped.
	class Buffer
	{
	public:
		static std::unique_ptr<Buffer> Create(Device& device, const BufferDesc& desc);
		~Buffer();

		// Synchronously uploads `size` bytes into a default-heap buffer via a staging buffer.
		void Upload(const void* data, uint64_t size, uint64_t dstOffset = 0);

		ID3D12Resource* Resource() const { return mResource.Get(); }
		D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return mResource->GetGPUVirtualAddress(); }
		void* Mapped() const { return mMapped; }
		uint64_t Size() const { return mDesc.size; }
		uint32_t Stride() const { return mDesc.stride; }
		uint32_t SrvIndex() const { return mSrv.index; }
		uint32_t UavIndex() const { return mUav.index; }

		D3D12_VERTEX_BUFFER_VIEW VertexView(uint32_t stride) const;
		D3D12_INDEX_BUFFER_VIEW IndexView(DXGI_FORMAT format) const;

	private:
		Buffer(Device& device, const BufferDesc& desc) : mDevice(device), mDesc(desc) {}

		Device& mDevice;
		BufferDesc mDesc;
		ComPtr<ID3D12Resource> mResource;
		DescriptorHandle mSrv;
		DescriptorHandle mUav;
		void* mMapped = nullptr;
	};

	// ---------------------------------------------------------------------------
	// Texture
	// ---------------------------------------------------------------------------
	enum TextureUsage : uint32_t
	{
		TextureUsage_ShaderResource = 1 << 0,
		TextureUsage_RenderTarget   = 1 << 1,
		TextureUsage_DepthStencil   = 1 << 2,
		TextureUsage_UnorderedAccess= 1 << 3,
	};

	struct TextureDesc
	{
		D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depthOrArraySize = 1;
		uint32_t mipLevels = 1;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;  // resource format (may be TYPELESS)
		uint32_t usage = TextureUsage_ShaderResource;
		// View formats; UNKNOWN means "same as format".
		DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
		float clearColor[4] = { 0, 0, 0, 1 };
		float clearDepth = 1.0f;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
		const char* name = "Texture";
	};

	// A texture with optional SRV (bindless), UAV (bindless), RTV and DSV views.
	// It tracks its whole-resource state so passes can call Transition() freely.
	class Texture
	{
	public:
		static std::unique_ptr<Texture> Create(Device& device, const TextureDesc& desc);
		// Wraps an existing resource (e.g. one created by the DDS loader) with an SRV.
		static std::unique_ptr<Texture> Wrap(Device& device, ComPtr<ID3D12Resource> resource, D3D12_RESOURCE_STATES state, DXGI_FORMAT srvFormat, const char* name);
		~Texture();

		// Synchronously uploads subresource data (all subresources, tightly described by `data`).
		void Upload(const D3D12_SUBRESOURCE_DATA* data, uint32_t count, D3D12_RESOURCE_STATES finalState);

		void Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState);
		// UAV barrier between two dispatches that touch this texture.
		void UavBarrier(ID3D12GraphicsCommandList* cmd);

		ID3D12Resource* Resource() const { return mResource.Get(); }
		D3D12_RESOURCE_STATES State() const { return mState; }
		const TextureDesc& Desc() const { return mDesc; }
		uint32_t Width() const { return mDesc.width; }
		uint32_t Height() const { return mDesc.height; }

		uint32_t SrvIndex() const { return mSrv.index; }
		uint32_t UavIndex() const { return mUav.index; }
		D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu() const { return mSrv.gpu; }
		D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const { return mRtv.cpu; }
		D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return mDsv.cpu; }

	private:
		Texture(Device& device, const TextureDesc& desc) : mDevice(device), mDesc(desc) {}
		void CreateViews();

		Device& mDevice;
		TextureDesc mDesc;
		ComPtr<ID3D12Resource> mResource;
		D3D12_RESOURCE_STATES mState = D3D12_RESOURCE_STATE_COMMON;
		DescriptorHandle mSrv;
		DescriptorHandle mUav;
		DescriptorHandle mRtv;
		DescriptorHandle mDsv;
	};
}
