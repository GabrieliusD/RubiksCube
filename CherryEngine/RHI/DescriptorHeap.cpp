#include "RHI/DescriptorHeap.h"

namespace rhi
{
	void DescriptorHeap::Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible, const char* name)
	{
		if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
		{
			shaderVisible = false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC desc{};
		desc.Type = type;
		desc.NumDescriptors = capacity;
		desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		DX_CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&mHeap)));
		SetDebugName(mHeap.Get(), name);

		mCapacity = capacity;
		mDescriptorSize = device->GetDescriptorHandleIncrementSize(type);
		mCpuStart = mHeap->GetCPUDescriptorHandleForHeapStart();
		mGpuStart = shaderVisible ? mHeap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };

		// Hand out low indices first.
		mFreeList.resize(capacity);
		for (uint32_t i = 0; i < capacity; ++i)
		{
			mFreeList[i] = capacity - 1 - i;
		}
		mAllocated = 0;
	}

	void DescriptorHeap::Shutdown()
	{
		mHeap.Reset();
		mFreeList.clear();
		mPending.clear();
		mCapacity = mAllocated = 0;
	}

	DescriptorHandle DescriptorHeap::HandleAt(uint32_t index) const
	{
		DescriptorHandle handle;
		handle.index = index;
		handle.cpu.ptr = mCpuStart.ptr + static_cast<SIZE_T>(index) * mDescriptorSize;
		if (IsShaderVisible())
		{
			handle.gpu.ptr = mGpuStart.ptr + static_cast<UINT64>(index) * mDescriptorSize;
		}
		return handle;
	}

	DescriptorHandle DescriptorHeap::Allocate()
	{
		std::lock_guard lock(mMutex);
		if (mFreeList.empty())
		{
			throw std::runtime_error("Descriptor heap is full");
		}
		const uint32_t index = mFreeList.back();
		mFreeList.pop_back();
		++mAllocated;
		return HandleAt(index);
	}

	void DescriptorHeap::Free(DescriptorHandle& handle, uint64_t fenceValue)
	{
		if (!handle.IsValid()) return;
		std::lock_guard lock(mMutex);
		mPending.push_back({ handle.index, fenceValue });
		handle = {};
	}

	void DescriptorHeap::ProcessDeferredFrees(uint64_t completedFenceValue)
	{
		std::lock_guard lock(mMutex);
		auto it = mPending.begin();
		while (it != mPending.end())
		{
			if (it->fenceValue <= completedFenceValue)
			{
				mFreeList.push_back(it->index);
				--mAllocated;
				it = mPending.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}
