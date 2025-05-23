#pragma once
#include "../d3dUtil.h"


using Microsoft::WRL::ComPtr;

template <typename T>
class ConstantBuffer
{
public:
	ConstantBuffer(ID3D12Device* device, UINT elementCount) 
	{
		elementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(T));
		device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(elementByteSize * elementCount),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mBuffer)
		);

		ThrowIfFailed(mBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));
	}
	~ConstantBuffer()
	{
		if (mBuffer != nullptr)
		{
			mBuffer->Unmap(0, nullptr);
		}
		mMappedData = nullptr;
	}
	void CopyData(UINT elementIndex, T& Data)
	{
		memcpy(&mMappedData[elementIndex * elementByteSize], &Data, sizeof(T));
	}
	ID3D12Resource* GetBuffer()
	{
		return mBuffer.Get();
	}
private:
	BYTE* mMappedData;
	ComPtr<ID3D12Resource> mBuffer;
	UINT elementByteSize;
};


