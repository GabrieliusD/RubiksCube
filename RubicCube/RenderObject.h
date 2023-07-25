#pragma once
#include "d3dUtil.h"
#include "Geometry.h"
struct RenderObject
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	Geometry* Geometry = nullptr;
	UINT ConstantBufferIndex = -1;
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	UINT IndexCount = 0;
	Material* Mat;
};

