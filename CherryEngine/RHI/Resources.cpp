#include "RHI/Resources.h"
#include "RHI/Device.h"
#include <cstring>

namespace rhi
{
	// ===========================================================================
	// Buffer
	// ===========================================================================
	std::unique_ptr<Buffer> Buffer::Create(Device& device, const BufferDesc& desc)
	{
		std::unique_ptr<Buffer> buffer(new Buffer(device, desc));
		const bool upload = desc.heap == HeapKind::Upload;

		CD3DX12_HEAP_PROPERTIES heapProps(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_FLAGS flags = desc.createUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.size, flags);
		const D3D12_RESOURCE_STATES initialState = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;

		DX_CHECK(device.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
			initialState, nullptr, IID_PPV_ARGS(&buffer->mResource)));
		SetDebugName(buffer->mResource.Get(), desc.name);

		if (upload)
		{
			D3D12_RANGE noRead{ 0, 0 };
			DX_CHECK(buffer->mResource->Map(0, &noRead, &buffer->mMapped));
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
			buffer->mSrv = device.SrvHeap().Allocate();
			device.D3D()->CreateShaderResourceView(buffer->mResource.Get(), &srv, buffer->mSrv.cpu);
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
			buffer->mUav = device.SrvHeap().Allocate();
			device.D3D()->CreateUnorderedAccessView(buffer->mResource.Get(), nullptr, &uav, buffer->mUav.cpu);
		}
		return buffer;
	}

	Buffer::~Buffer()
	{
		if (mMapped)
		{
			mResource->Unmap(0, nullptr);
			mMapped = nullptr;
		}
		mDevice.FreeDescriptor(mDevice.SrvHeap(), mSrv);
		mDevice.FreeDescriptor(mDevice.SrvHeap(), mUav);
		mDevice.DeferRelease(mResource);
	}

	void Buffer::Upload(const void* data, uint64_t size, uint64_t dstOffset)
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

		mDevice.ExecuteImmediate([&](ID3D12GraphicsCommandList4* cmd)
		{
			// COMMON -> COPY_DEST is an implicit promotion for buffers.
			cmd->CopyBufferRegion(mResource.Get(), dstOffset, staging.Get(), 0, size);
		});
	}

	D3D12_VERTEX_BUFFER_VIEW Buffer::VertexView(uint32_t stride) const
	{
		D3D12_VERTEX_BUFFER_VIEW view{};
		view.BufferLocation = GpuAddress();
		view.SizeInBytes = static_cast<UINT>(mDesc.size);
		view.StrideInBytes = stride;
		return view;
	}

	D3D12_INDEX_BUFFER_VIEW Buffer::IndexView(DXGI_FORMAT format) const
	{
		D3D12_INDEX_BUFFER_VIEW view{};
		view.BufferLocation = GpuAddress();
		view.SizeInBytes = static_cast<UINT>(mDesc.size);
		view.Format = format;
		return view;
	}

	// ===========================================================================
	// Texture
	// ===========================================================================
	std::unique_ptr<Texture> Texture::Create(Device& device, const TextureDesc& desc)
	{
		std::unique_ptr<Texture> texture(new Texture(device, desc));

		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = desc.dimension;
		resourceDesc.Width = desc.width;
		resourceDesc.Height = desc.height;
		resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depthOrArraySize);
		resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
		resourceDesc.Format = desc.format;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (desc.usage & TextureUsage_RenderTarget)    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (desc.usage & TextureUsage_DepthStencil)    resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		if (desc.usage & TextureUsage_UnorderedAccess) resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		D3D12_CLEAR_VALUE clear{};
		const D3D12_CLEAR_VALUE* clearPtr = nullptr;
		if (desc.usage & TextureUsage_DepthStencil)
		{
			clear.Format = desc.dsvFormat != DXGI_FORMAT_UNKNOWN ? desc.dsvFormat : desc.format;
			clear.DepthStencil.Depth = desc.clearDepth;
			clearPtr = &clear;
		}
		else if (desc.usage & TextureUsage_RenderTarget)
		{
			clear.Format = desc.rtvFormat != DXGI_FORMAT_UNKNOWN ? desc.rtvFormat : desc.format;
			std::memcpy(clear.Color, desc.clearColor, sizeof(clear.Color));
			clearPtr = &clear;
		}

		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		DX_CHECK(device.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
			desc.initialState, clearPtr, IID_PPV_ARGS(&texture->mResource)));
		SetDebugName(texture->mResource.Get(), desc.name);
		texture->mState = desc.initialState;
		texture->CreateViews();
		return texture;
	}

	std::unique_ptr<Texture> Texture::Wrap(Device& device, ComPtr<ID3D12Resource> resource, D3D12_RESOURCE_STATES state, DXGI_FORMAT srvFormat, const char* name)
	{
		const D3D12_RESOURCE_DESC rd = resource->GetDesc();
		TextureDesc desc;
		desc.dimension = rd.Dimension;
		desc.width = static_cast<uint32_t>(rd.Width);
		desc.height = rd.Height;
		desc.depthOrArraySize = rd.DepthOrArraySize;
		desc.mipLevels = rd.MipLevels;
		desc.format = rd.Format;
		desc.srvFormat = srvFormat;
		desc.usage = TextureUsage_ShaderResource;
		desc.name = name;

		std::unique_ptr<Texture> texture(new Texture(device, desc));
		texture->mResource = std::move(resource);
		texture->mState = state;
		SetDebugName(texture->mResource.Get(), name);
		texture->CreateViews();
		return texture;
	}

	void Texture::CreateViews()
	{
		ID3D12Device* d3d = mDevice.D3D();
		const bool is3D = mDesc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		auto pick = [&](DXGI_FORMAT view) { return view != DXGI_FORMAT_UNKNOWN ? view : mDesc.format; };

		if (mDesc.usage & TextureUsage_ShaderResource)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Format = pick(mDesc.srvFormat);
			if (is3D)
			{
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				srv.Texture3D.MipLevels = mDesc.mipLevels;
			}
			else
			{
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Texture2D.MipLevels = mDesc.mipLevels;
			}
			mSrv = mDevice.SrvHeap().Allocate();
			d3d->CreateShaderResourceView(mResource.Get(), &srv, mSrv.cpu);
		}
		if (mDesc.usage & TextureUsage_UnorderedAccess)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = pick(mDesc.uavFormat);
			if (is3D)
			{
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
				uav.Texture3D.WSize = mDesc.depthOrArraySize;
			}
			else
			{
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			}
			mUav = mDevice.SrvHeap().Allocate();
			d3d->CreateUnorderedAccessView(mResource.Get(), nullptr, &uav, mUav.cpu);
		}
		if (mDesc.usage & TextureUsage_RenderTarget)
		{
			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = pick(mDesc.rtvFormat);
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			mRtv = mDevice.RtvHeap().Allocate();
			d3d->CreateRenderTargetView(mResource.Get(), &rtv, mRtv.cpu);
		}
		if (mDesc.usage & TextureUsage_DepthStencil)
		{
			D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
			dsv.Format = pick(mDesc.dsvFormat);
			dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			mDsv = mDevice.DsvHeap().Allocate();
			d3d->CreateDepthStencilView(mResource.Get(), &dsv, mDsv.cpu);
		}
	}

	Texture::~Texture()
	{
		mDevice.FreeDescriptor(mDevice.SrvHeap(), mSrv);
		mDevice.FreeDescriptor(mDevice.SrvHeap(), mUav);
		mDevice.FreeDescriptor(mDevice.RtvHeap(), mRtv);
		mDevice.FreeDescriptor(mDevice.DsvHeap(), mDsv);
		mDevice.DeferRelease(mResource);
	}

	void Texture::Upload(const D3D12_SUBRESOURCE_DATA* data, uint32_t count, D3D12_RESOURCE_STATES finalState)
	{
		const UINT64 stagingSize = GetRequiredIntermediateSize(mResource.Get(), 0, count);
		ComPtr<ID3D12Resource> staging;
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(stagingSize);
		DX_CHECK(mDevice.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));

		mDevice.ExecuteImmediate([&](ID3D12GraphicsCommandList4* cmd)
		{
			Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
			UpdateSubresources(cmd, mResource.Get(), staging.Get(), 0, 0, count, const_cast<D3D12_SUBRESOURCE_DATA*>(data));
			Transition(cmd, finalState);
		});
	}

	void Texture::Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES newState)
	{
		if (mState == newState) return;
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(mResource.Get(), mState, newState);
		cmd->ResourceBarrier(1, &barrier);
		mState = newState;
	}

	void Texture::UavBarrier(ID3D12GraphicsCommandList* cmd)
	{
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(mResource.Get());
		cmd->ResourceBarrier(1, &barrier);
	}
}
