#pragma once
#include <d3d12.h>
#include "d3dUtil.h"

class RenderTexture 
{
public:
	RenderTexture(DXGI_FORMAT format);

	void SetDevice(ID3D12Device* device, descriptor_handle srvDescriptor, descriptor_handle rtvDescriptor);
	void SizeResources(size_t width, size_t height);
	void ReleaseDevice();

	void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES afterState);
	void BeginScene(ID3D12GraphicsCommandList* commandList) 
	{
		TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	void EndScene(ID3D12GraphicsCommandList* commandList) 
	{
		TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	void Clear(ID3D12GraphicsCommandList* commandList) 
	{
		commandList->ClearRenderTargetView(mRtvDescriptorHandle.cpu, mClearColor, 0, nullptr);
	}

	void SetClearColor(DirectX::FXMVECTOR color) 
	{
		DirectX::XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(mClearColor), color);
	}

	ID3D12Resource* GetResource() const { return mResource.Get(); }
	D3D12_RESOURCE_STATES GetCurrentState() const { return mState;  }
	void UpdateState(D3D12_RESOURCE_STATES state) { mState = state; }

	void SetWindow(const RECT& rect);

	DXGI_FORMAT GetFormat() const { return mFormat; }

	descriptor_handle GetSrvDescriptorHandle() { return mSrvDescriptorHandle; }
	descriptor_handle GetRtvDescriptorHandle() { return mRtvDescriptorHandle; }

private:

	Microsoft::WRL::ComPtr<ID3D12Device> mDevice;
	Microsoft::WRL::ComPtr<ID3D12Resource> mResource;
	D3D12_RESOURCE_STATES mState;
	descriptor_handle mSrvDescriptorHandle;
	descriptor_handle mRtvDescriptorHandle;
	float mClearColor[4];
	DXGI_FORMAT mFormat;

	size_t mWidth;
	size_t mHeight;
};