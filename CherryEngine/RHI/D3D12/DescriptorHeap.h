#pragma once
#include "RHI/D3D12/D3DCommon.h"
#include <vector>
#include <mutex>

namespace rhi
{
	// A single descriptor slot. For the shader-visible CBV/SRV/UAV heap, `index`
	// is also the bindless index used with ResourceDescriptorHeap[] in HLSL.
	struct DescriptorHandle
	{
		D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
		uint32_t index = kInvalidIndex;

		bool IsValid() const { return index != kInvalidIndex; }
	};

	// Free-list descriptor allocator. Freed slots are only recycled after the GPU
	// has finished the frame in which they were freed (see ProcessDeferredFrees).
	class DescriptorHeap
	{
	public:
		void Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible, const char* name);
		void Shutdown();

		DescriptorHandle Allocate();
		// `fenceValue` is the fence value that must complete before the slot can be reused.
		void Free(DescriptorHandle& handle, uint64_t fenceValue);
		void ProcessDeferredFrees(uint64_t completedFenceValue);

		ID3D12DescriptorHeap* Heap() const { return mHeap.Get(); }
		uint32_t Capacity() const { return mCapacity; }
		uint32_t Allocated() const { return mAllocated; }
		uint32_t DescriptorSize() const { return mDescriptorSize; }
		bool IsShaderVisible() const { return mGpuStart.ptr != 0; }
		DescriptorHandle HandleAt(uint32_t index) const;

	private:
		struct PendingFree { uint32_t index; uint64_t fenceValue; };

		ComPtr<ID3D12DescriptorHeap> mHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE mCpuStart{};
		D3D12_GPU_DESCRIPTOR_HANDLE mGpuStart{};
		std::vector<uint32_t> mFreeList;
		std::vector<PendingFree> mPending;
		std::mutex mMutex;
		uint32_t mCapacity = 0;
		uint32_t mAllocated = 0;
		uint32_t mDescriptorSize = 0;
	};
}
