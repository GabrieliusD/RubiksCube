#pragma once
#include "d3dUtil.h"
using Microsoft::WRL::ComPtr;
class Geometry
{
public:
	ComPtr<ID3D12Resource> mVertexUploadBuffer;
	ComPtr<ID3D12Resource> mVertexBuffer;

	ComPtr<ID3D12Resource> mIndexUploadBuffer;
	ComPtr<ID3D12Resource> mIndexBuffer;

public:
	D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView();
	D3D12_INDEX_BUFFER_VIEW GetIndexBufferView();

	UINT64 mVbByteSize;
	UINT64 mIbByteSize;
	UINT64 mStrideInBytes;
	DXGI_FORMAT mIbFormat;
	UINT mIndexCount;
private:
	D3D12_VERTEX_BUFFER_VIEW mVbv;
	D3D12_INDEX_BUFFER_VIEW mIbv;
};

