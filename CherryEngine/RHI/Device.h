#pragma once
#include "RHI/D3DCommon.h"
#include "RHI/DescriptorHeap.h"
#include <functional>
#include <vector>
#include <array>

namespace rhi
{
	struct DeviceCaps
	{
		std::string adapterName;
		bool raytracing = false;     // DXR tier 1.0+ (logged for later use)
		bool tearing = false;        // DXGI_FEATURE_PRESENT_ALLOW_TEARING
	};

	struct DeviceOptions
	{
		bool debugLayer = false;
		bool gpuBasedValidation = false;
	};

	// Owns the D3D12 device, the direct queue, per-frame command allocators and
	// fences, the global descriptor heaps and deferred destruction.
	//
	// Frame model: kFramesInFlight frames may be queued on the GPU at once.
	// BeginFrame() waits only for the frame that previously used the same slot,
	// instead of flushing the whole queue every frame.
	class Device
	{
	public:
		Device() = default;
		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;
		~Device();

		// Returns false and fills `error` if the GPU doesn't meet the engine's baseline
		// (feature level 12_0, Shader Model 6.6, resource binding tier 3).
		bool Initialize(const DeviceOptions& options, std::string& error);
		void Shutdown();

		ID3D12Device5* D3D() const { return mDevice.Get(); }
		IDXGIFactory6* Factory() const { return mFactory.Get(); }
		ID3D12CommandQueue* Queue() const { return mQueue.Get(); }
		const DeviceCaps& Caps() const { return mCaps; }

		// Frame lifecycle. The command list is open between BeginFrame and EndFrame.
		// EndFrame submits the list; call SignalEndOfFrame after Present.
		void BeginFrame();
		void EndFrame();
		void SignalEndOfFrame();
		ID3D12GraphicsCommandList4* CmdList() const { return mCmdList.Get(); }
		uint32_t FrameIndex() const { return mFrameIndex; }
		uint64_t FrameNumber() const { return mFrameNumber; }

		// Blocks until the GPU has finished all submitted work.
		void WaitIdle();

		// Records `record` into a one-off command list, submits it and waits for it.
		// Used for resource uploads outside the frame command list.
		void ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList4*)>& record);

		// Releases `object` once the GPU has finished every frame that may use it.
		void DeferRelease(ComPtr<IUnknown> object);
		// Returns a descriptor to its heap once the GPU no longer references it.
		void FreeDescriptor(DescriptorHeap& heap, DescriptorHandle& handle);

		DescriptorHeap& SrvHeap() { return mSrvHeap; }   // shader-visible CBV/SRV/UAV (bindless)
		DescriptorHeap& RtvHeap() { return mRtvHeap; }
		DescriptorHeap& DsvHeap() { return mDsvHeap; }

		uint32_t DebugErrorCount() const { return mDebugErrors; }
		uint32_t DebugWarningCount() const { return mDebugWarnings; }

	private:
		struct FrameContext
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			uint64_t fenceValue = 0;
		};
		struct PendingRelease
		{
			ComPtr<IUnknown> object;
			uint64_t fenceValue;
		};

		void CreateAdapterAndDevice(const DeviceOptions& options);
		bool CheckCaps(std::string& error);
		void SetupDebugMessages();
		void WaitForFence(ID3D12Fence* fence, uint64_t value);
		void ProcessDeferred(uint64_t completedValue);
		static void CALLBACK OnDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR description, void* context);

		ComPtr<IDXGIFactory6> mFactory;
		ComPtr<IDXGIAdapter1> mAdapter;
		ComPtr<ID3D12Device5> mDevice;
		ComPtr<ID3D12CommandQueue> mQueue;
		ComPtr<ID3D12GraphicsCommandList4> mCmdList;
		ComPtr<ID3D12Fence> mFence;
		HANDLE mFenceEvent = nullptr;
		std::array<FrameContext, kFramesInFlight> mFrames;
		uint64_t mNextFenceValue = 1;
		uint32_t mFrameIndex = 0;
		uint64_t mFrameNumber = 0;

		// Immediate (upload) submission path, with its own fence.
		ComPtr<ID3D12CommandAllocator> mImmediateAllocator;
		ComPtr<ID3D12GraphicsCommandList4> mImmediateList;
		ComPtr<ID3D12Fence> mImmediateFence;
		uint64_t mImmediateFenceValue = 0;

		DescriptorHeap mSrvHeap;
		DescriptorHeap mRtvHeap;
		DescriptorHeap mDsvHeap;

		std::vector<PendingRelease> mPendingReleases;
		DeviceCaps mCaps;
		DWORD mDebugCallbackCookie = 0;
		uint32_t mDebugErrors = 0;
		uint32_t mDebugWarnings = 0;
		bool mInitialized = false;
	};
}
