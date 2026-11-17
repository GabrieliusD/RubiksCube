#include "RHI/SwapChain.h"
#include "RHI/Device.h"

namespace rhi
{
	void SwapChain::Initialize(Device& device, HWND hwnd, uint32_t width, uint32_t height)
	{
		mDevice = &device;
		mTearing = device.Caps().tearing;
		mWidth = width > 0 ? width : 1;
		mHeight = height > 0 ? height : 1;

		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = mWidth;
		desc.Height = mHeight;
		desc.Format = kFormat;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = kBufferCount;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.Flags = mTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

		ComPtr<IDXGISwapChain1> swapChain1;
		DX_CHECK(device.Factory()->CreateSwapChainForHwnd(device.Queue(), hwnd, &desc, nullptr, nullptr, &swapChain1));
		DX_CHECK(swapChain1.As(&mSwapChain));
		device.Factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

		for (auto& rtv : mRtvs)
		{
			rtv = device.RtvHeap().Allocate();
		}
		CreateBufferViews();
	}

	void SwapChain::Shutdown()
	{
		if (!mDevice) return;
		ReleaseBuffers();
		for (auto& rtv : mRtvs)
		{
			mDevice->FreeDescriptor(mDevice->RtvHeap(), rtv);
		}
		mSwapChain.Reset();
		mDevice = nullptr;
	}

	void SwapChain::CreateBufferViews()
	{
		for (uint32_t i = 0; i < kBufferCount; ++i)
		{
			DX_CHECK(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mBuffers[i])));
			SetDebugName(mBuffers[i].Get(), "BackBuffer");
			mDevice->D3D()->CreateRenderTargetView(mBuffers[i].Get(), nullptr, mRtvs[i].cpu);
		}
	}

	void SwapChain::ReleaseBuffers()
	{
		for (auto& buffer : mBuffers)
		{
			buffer.Reset();
		}
	}

	void SwapChain::Resize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || (width == mWidth && height == mHeight))
		{
			return;
		}
		mDevice->WaitIdle();
		ReleaseBuffers();
		DX_CHECK(mSwapChain->ResizeBuffers(kBufferCount, width, height, kFormat, mTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
		mWidth = width;
		mHeight = height;
		CreateBufferViews();
	}

	void SwapChain::Present(bool vsync)
	{
		const UINT flags = (!vsync && mTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
		DX_CHECK(mSwapChain->Present(vsync ? 1 : 0, flags));
	}
}
