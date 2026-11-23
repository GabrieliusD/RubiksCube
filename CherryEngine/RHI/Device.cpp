#include "RHI/Device.h"
#include "Core/Log.h"

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

		std::string WideToUtf8(const wchar_t* wide)
		{
			char buffer[256];
			WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer), nullptr, nullptr);
			return buffer;
		}
	}

	Device::~Device()
	{
		Shutdown();
	}

	bool Device::Initialize(const DeviceOptions& options, std::string& error)
	{
		CreateAdapterAndDevice(options);
		if (!mDevice)
		{
			error = "No Direct3D 12 GPU with feature level 12_0 was found.";
			return false;
		}
		if (!CheckCaps(error))
		{
			return false;
		}
		if (options.debugLayer)
		{
			SetupDebugMessages();
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		DX_CHECK(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mQueue)));
		SetDebugName(mQueue.Get(), "DirectQueue");

		for (uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			DX_CHECK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mFrames[i].allocator)));
		}
		DX_CHECK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mFrames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
		DX_CHECK(mCmdList->Close());
		SetDebugName(mCmdList.Get(), "FrameCommandList");

		DX_CHECK(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mImmediateAllocator)));
		DX_CHECK(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mImmediateAllocator.Get(), nullptr, IID_PPV_ARGS(&mImmediateList)));
		DX_CHECK(mImmediateList->Close());
		SetDebugName(mImmediateList.Get(), "ImmediateCommandList");

		DX_CHECK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
		DX_CHECK(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mImmediateFence)));
		mFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

		mSrvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapCapacity, true, "BindlessHeap");
		mRtvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity, false, "RtvHeap");
		mDsvHeap.Initialize(mDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity, false, "DsvHeap");

		mInitialized = true;
		return true;
	}

	void Device::CreateAdapterAndDevice(const DeviceOptions& options)
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
			if (mFactory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				continue;
			}
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice))))
			{
				mAdapter = adapter;
				mCaps.adapterName = WideToUtf8(desc.Description);
				break;
			}
		}
	}

	bool Device::CheckCaps(std::string& error)
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
		{
			mCaps.tearing = tearing == TRUE;
		}

		CHERRY_LOG("GPU: %s | SM 6.6: yes | binding tier 3: yes | DXR: %s | tearing: %s",
			mCaps.adapterName.c_str(), mCaps.raytracing ? "yes" : "no", mCaps.tearing ? "yes" : "no");
		return true;
	}

	void Device::SetupDebugMessages()
	{
		ComPtr<ID3D12InfoQueue1> infoQueue1;
		if (SUCCEEDED(mDevice.As(&infoQueue1)))
		{
			infoQueue1->RegisterMessageCallback(&Device::OnDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &mDebugCallbackCookie);
		}
		else
		{
			CHERRY_WARN("ID3D12InfoQueue1 unavailable; debug layer messages only go to the debugger output");
		}
	}

	void CALLBACK Device::OnDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* context)
	{
		// Performance hints only: render targets are cleared to a colour that can change at runtime.
		if (id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
			id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
		{
			return;
		}
		auto* self = static_cast<Device*>(context);
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

	void Device::Shutdown()
	{
		if (!mInitialized) return;
		WaitIdle();
		ProcessDeferred(UINT64_MAX);
		mSrvHeap.Shutdown();
		mRtvHeap.Shutdown();
		mDsvHeap.Shutdown();
		mCmdList.Reset();
		ProcessAsync(UINT64_MAX);
		mAsync.clear();
		mImmediateList.Reset();
		mImmediateAllocator.Reset();
		for (auto& frame : mFrames) frame.allocator.Reset();
		mFence.Reset();
		mImmediateFence.Reset();
		mQueue.Reset();
		if (mFenceEvent) CloseHandle(mFenceEvent);
		mFenceEvent = nullptr;

		if (mDebugCallbackCookie)
		{
			ComPtr<ID3D12InfoQueue1> infoQueue1;
			if (SUCCEEDED(mDevice.As(&infoQueue1)))
			{
				infoQueue1->UnregisterMessageCallback(mDebugCallbackCookie);
			}
			mDebugCallbackCookie = 0;
		}
		mDevice.Reset();
		mAdapter.Reset();
		mFactory.Reset();
		mInitialized = false;
	}

	void Device::WaitForFence(ID3D12Fence* fence, uint64_t value)
	{
		if (fence->GetCompletedValue() < value)
		{
			DX_CHECK(fence->SetEventOnCompletion(value, mFenceEvent));
			WaitForSingleObject(mFenceEvent, INFINITE);
		}
	}

	void Device::BeginFrame()
	{
		FrameContext& frame = mFrames[mFrameIndex];
		WaitForFence(mFence.Get(), frame.fenceValue);
		ProcessDeferred(mFence->GetCompletedValue());
		ProcessAsync(mImmediateFence->GetCompletedValue());

		DX_CHECK(frame.allocator->Reset());
		DX_CHECK(mCmdList->Reset(frame.allocator.Get(), nullptr));
	}

	void Device::EndFrame()
	{
		DX_CHECK(mCmdList->Close());
		ID3D12CommandList* lists[] = { mCmdList.Get() };
		mQueue->ExecuteCommandLists(1, lists);
	}

	void Device::SignalEndOfFrame()
	{
		FrameContext& frame = mFrames[mFrameIndex];
		frame.fenceValue = mNextFenceValue++;
		DX_CHECK(mQueue->Signal(mFence.Get(), frame.fenceValue));
		mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
		++mFrameNumber;
	}

	void Device::WaitIdle()
	{
		if (!mQueue) return;
		const uint64_t value = mNextFenceValue++;
		DX_CHECK(mQueue->Signal(mFence.Get(), value));
		WaitForFence(mFence.Get(), value);
		ProcessDeferred(mFence->GetCompletedValue());
	}

	void Device::ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList4*)>& record)
	{
		DX_CHECK(mImmediateAllocator->Reset());
		DX_CHECK(mImmediateList->Reset(mImmediateAllocator.Get(), nullptr));
		record(mImmediateList.Get());
		DX_CHECK(mImmediateList->Close());
		ID3D12CommandList* lists[] = { mImmediateList.Get() };
		mQueue->ExecuteCommandLists(1, lists);
		DX_CHECK(mQueue->Signal(mImmediateFence.Get(), ++mImmediateFenceValue));
		WaitForFence(mImmediateFence.Get(), mImmediateFenceValue);
	}

	void Device::ExecuteAsync(const std::function<void(ID3D12GraphicsCommandList4*)>& record, ComPtr<IUnknown> keepAlive)
	{
		// A command list the GPU has finished with, or a new one. Many uploads in flight at
		// once (a burst of mesh rebuilds) wait for the oldest rather than growing forever.
		constexpr size_t kMaxContexts = 64;
		uint64_t completed = mImmediateFence->GetCompletedValue();
		AsyncContext* context = nullptr;
		for (AsyncContext& c : mAsync)
			if (c.fenceValue <= completed) { context = &c; break; }
		if (!context && mAsync.size() >= kMaxContexts)
		{
			AsyncContext* oldest = &mAsync.front();
			for (AsyncContext& c : mAsync)
				if (c.fenceValue < oldest->fenceValue) oldest = &c;
			WaitForFence(mImmediateFence.Get(), oldest->fenceValue);
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
		context->fenceValue = ++mImmediateFenceValue;
		DX_CHECK(mQueue->Signal(mImmediateFence.Get(), context->fenceValue));
		if (keepAlive) mAsyncReleases.push_back({ std::move(keepAlive), context->fenceValue });
	}

	void Device::ProcessAsync(uint64_t completedValue)
	{
		auto it = mAsyncReleases.begin();
		while (it != mAsyncReleases.end())
		{
			if (it->fenceValue <= completedValue) it = mAsyncReleases.erase(it);
			else ++it;
		}
	}

	void Device::DeferRelease(ComPtr<IUnknown> object)
	{
		if (!object) return;
		// mNextFenceValue is the value the current frame will signal when it ends.
		mPendingReleases.push_back({ std::move(object), mNextFenceValue });
	}

	void Device::FreeDescriptor(DescriptorHeap& heap, DescriptorHandle& handle)
	{
		heap.Free(handle, mNextFenceValue);
	}

	void Device::ProcessDeferred(uint64_t completedValue)
	{
		auto it = mPendingReleases.begin();
		while (it != mPendingReleases.end())
		{
			if (it->fenceValue <= completedValue) it = mPendingReleases.erase(it);
			else ++it;
		}
		mSrvHeap.ProcessDeferredFrees(completedValue);
		mRtvHeap.ProcessDeferredFrees(completedValue);
		mDsvHeap.ProcessDeferredFrees(completedValue);
	}
}
