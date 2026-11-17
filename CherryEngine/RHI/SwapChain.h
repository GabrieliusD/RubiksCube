#pragma once
#include "RHI/D3DCommon.h"
#include "RHI/DescriptorHeap.h"
#include <array>

namespace rhi
{
	class Device;

	// Flip-model swap chain with tearing support and in-place resize.
	class SwapChain
	{
	public:
		static constexpr uint32_t kBufferCount = 3;
		static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

		void Initialize(Device& device, HWND hwnd, uint32_t width, uint32_t height);
		void Shutdown();

		// Waits for the GPU, then resizes the back buffers. No-op for zero size.
		void Resize(uint32_t width, uint32_t height);
		void Present(bool vsync);

		ID3D12Resource* BackBuffer() const { return mBuffers[mSwapChain->GetCurrentBackBufferIndex()].Get(); }
		D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv() const { return mRtvs[mSwapChain->GetCurrentBackBufferIndex()].cpu; }
		uint32_t Width() const { return mWidth; }
		uint32_t Height() const { return mHeight; }

	private:
		void CreateBufferViews();
		void ReleaseBuffers();

		Device* mDevice = nullptr;
		ComPtr<IDXGISwapChain3> mSwapChain;
		std::array<ComPtr<ID3D12Resource>, kBufferCount> mBuffers;
		std::array<DescriptorHandle, kBufferCount> mRtvs;
		uint32_t mWidth = 0;
		uint32_t mHeight = 0;
		bool mTearing = false;
	};
}
