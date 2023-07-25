#include "Geometry.h"

D3D12_VERTEX_BUFFER_VIEW Geometry::GetVertexBufferView()
{
    mVbv.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVbv.SizeInBytes = mVbByteSize;
    mVbv.StrideInBytes = mStrideInBytes;
    return mVbv;
}

D3D12_INDEX_BUFFER_VIEW Geometry::GetIndexBufferView()
{
    mIbv.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIbv.Format = mIbFormat;
    mIbv.SizeInBytes = mIbByteSize;
    return mIbv;
}
