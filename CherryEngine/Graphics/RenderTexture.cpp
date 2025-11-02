#include "RenderTexture.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

RenderTexture::RenderTexture(DXGI_FORMAT format) :
	mState(D3D12_RESOURCE_STATE_COMMON),
	mSrvDescriptorHandle{},
	mRtvDescriptorHandle{},
	mClearColor{},
	mFormat(format),
	mWidth(0),
	mHeight(0)
{
}

void RenderTexture::SetDevice(ID3D12Device* device, descriptor_handle srvDescriptor, descriptor_handle rtvDescriptor)
{
	if (device == mDevice.Get() && srvDescriptor.cpu.ptr == mSrvDescriptorHandle.cpu.ptr && rtvDescriptor.cpu.ptr == mRtvDescriptorHandle.cpu.ptr)
	{
		return;
	}

	if (mDevice)
	{
		ReleaseDevice();
	}

	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { mFormat, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
		if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
		{
			throw "CheckFeatureSupport";
		}

		UINT required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
		if ((formatSupport.Support1 & required) != required)
		{
			throw "RenderTexture";
		}

		if (!srvDescriptor.cpu.ptr || !rtvDescriptor.cpu.ptr)
		{
			throw "Invalid descriptors";
		}

		mDevice = device;

		mSrvDescriptorHandle = srvDescriptor;
		mRtvDescriptorHandle = rtvDescriptor;
	}
}

void RenderTexture::SizeResources(size_t width, size_t height)
{
	if (width == mWidth && height == mHeight)
	{
		return;
	}

	if (mWidth > UINT32_MAX || mHeight > UINT32_MAX)
	{
		throw "Invalid width/height";
	}

	if (!mDevice)
	{
		return;
	}

	mWidth = mHeight = 0;

	auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(mFormat,
		static_cast<UINT64>(width),
		static_cast<UINT64>(height),
		1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	D3D12_CLEAR_VALUE clearValue = { mFormat, {} };
	memcpy(clearValue.Color, mClearColor, sizeof(clearValue.Color));

	mState = D3D12_RESOURCE_STATE_RENDER_TARGET;

	ThrowIfFailed(
		mDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES,
			&desc, mState, &clearValue,
			IID_PPV_ARGS(mResource.ReleaseAndGetAddressOf()))
	);

	mDevice->CreateRenderTargetView(mResource.Get(), nullptr, mRtvDescriptorHandle.cpu);
	mDevice->CreateShaderResourceView(mResource.Get(), nullptr, mSrvDescriptorHandle.cpu);

	mWidth = width;
	mHeight = height;
}

void RenderTexture::ReleaseDevice()
{
	mResource.Reset();
	mDevice.Reset();
	mState = D3D12_RESOURCE_STATE_COMMON;
	mWidth = mHeight = 0;
	mSrvDescriptorHandle = mRtvDescriptorHandle = {};
}

void RenderTexture::TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES afterState)
{
	if (mState == afterState)
	{
		return;
	}

	commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mResource.Get(),
		mState, afterState));
	mState = afterState;
}

void RenderTexture::SetWindow(const RECT& output)
{
	auto width = size_t(std::max<LONG>(output.right - output.left, 1));
	auto height = size_t(std::max<LONG>(output.bottom - output.top, 1));
	SizeResources(width, height);
}