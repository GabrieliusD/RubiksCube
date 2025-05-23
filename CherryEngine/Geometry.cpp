#include "Geometry.h"

D3D12_VERTEX_BUFFER_VIEW Geometry::GetVertexBufferView()
{
    mVbv.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    mVbv.SizeInBytes = vbByteSize;
    mVbv.StrideInBytes = strideInBytes;
    return mVbv;
}

D3D12_INDEX_BUFFER_VIEW Geometry::GetIndexBufferView()
{
    mIbv.BufferLocation = indexBuffer->GetGPUVirtualAddress();
    mIbv.Format = ibFormat;
    mIbv.SizeInBytes = ibByteSize;
    return mIbv;
}
