#pragma once
#include "RHI/D3DCommon.h"
#include <array>
#include <cstring>

namespace rhi
{
	class Device;

	struct UploadAllocation
	{
		void* cpu = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	};

	// Per-frame linear allocator in persistently-mapped upload memory. Each frame in
	// flight owns one region, reset at the start of that frame. Used for root CBVs
	// and other small per-frame data.
	class UploadRing
	{
	public:
		void Initialize(Device& device, uint64_t bytesPerFrame);
		void Shutdown();

		void BeginFrame(uint32_t frameIndex);
		UploadAllocation Allocate(uint64_t size, uint64_t alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

		template <typename T>
		D3D12_GPU_VIRTUAL_ADDRESS PushConstants(const T& data)
		{
			UploadAllocation alloc = Allocate(sizeof(T));
			std::memcpy(alloc.cpu, &data, sizeof(T));
			return alloc.gpu;
		}

	private:
		ComPtr<ID3D12Resource> mBuffer;
		uint8_t* mMapped = nullptr;
		uint64_t mBytesPerFrame = 0;
		uint64_t mFrameStart = 0;
		uint64_t mOffset = 0;
	};
}
