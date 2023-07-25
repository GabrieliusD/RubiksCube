#pragma once
#include <DirectXMath.h>
#include "MathHelper.h"
#include <string>
using namespace DirectX;
struct Vertex
{
	XMFLOAT3 Pos;
	XMFLOAT4 Color;
};

struct VPosData
{
	XMFLOAT3 Pos;
};

struct VColorData
{
	XMFLOAT4 Color;
};

struct ObjectConstants
{
	XMFLOAT4X4 World = MathHelper::Identity4x4();
};

class Common
{
};

