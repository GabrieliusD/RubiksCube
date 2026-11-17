#include "RHI/UploadRing.h"
#include "RHI/Device.h"
#include <cstring>

namespace rhi
{
	void UploadRing::Initialize(Device& device, uint64_t bytesPerFrame)
	{
		mBytesPerFrame = bytesPerFrame;
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytesPerFrame * kFramesInFlight);
		DX_CHECK(device.D3D()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mBuffer)));
		SetDebugName(mBuffer.Get(), "UploadRing");
		D3D12_RANGE noRead{ 0, 0 };
		DX_CHECK(mBuffer->Map(0, &noRead, reinterpret_cast<void**>(&mMapped)));
	}

	void UploadRing::Shutdown()
	{
		if (mBuffer && mMapped)
		{
			mBuffer->Unmap(0, nullptr);
		}
		mMapped = nullptr;
		mBuffer.Reset();
	}

	void UploadRing::BeginFrame(uint32_t frameIndex)
	{
		mFrameStart = frameIndex * mBytesPerFrame;
		mOffset = 0;
	}

	UploadAllocation UploadRing::Allocate(uint64_t size, uint64_t alignment)
	{
		const uint64_t aligned = (mOffset + alignment - 1) & ~(alignment - 1);
		if (aligned + size > mBytesPerFrame)
		{
			throw std::runtime_error("UploadRing out of memory for this frame");
		}
		mOffset = aligned + size;

		UploadAllocation alloc;
		alloc.cpu = mMapped + mFrameStart + aligned;
		alloc.gpu = mBuffer->GetGPUVirtualAddress() + mFrameStart + aligned;
		return alloc;
	}
}
