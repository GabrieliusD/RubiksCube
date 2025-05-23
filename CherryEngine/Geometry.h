#pragma once
#include "d3dUtil.h"
using Microsoft::WRL::ComPtr;
class Geometry
{
public:
	ComPtr<ID3D12Resource> vertexUploadBuffer;
	ComPtr<ID3D12Resource> vertexBuffer;

	ComPtr<ID3D12Resource> indexUploadBuffer;
	ComPtr<ID3D12Resource> indexBuffer;

	D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView();
	D3D12_INDEX_BUFFER_VIEW GetIndexBufferView();

	UINT64 vbByteSize;
	UINT64 ibByteSize;
	UINT64 strideInBytes;
	DXGI_FORMAT ibFormat;
	UINT indexCount;
	DirectX::BoundingBox bounds;
private:
	D3D12_VERTEX_BUFFER_VIEW mVbv;
	D3D12_INDEX_BUFFER_VIEW mIbv;
};

