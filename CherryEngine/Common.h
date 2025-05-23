#pragma once
#include <DirectXMath.h>
#include "MathHelper.h"
#include <string>
using namespace DirectX;
struct Vertex
{
	XMFLOAT3 Pos;
	XMFLOAT4 Color;
	XMFLOAT3 Normal;
	XMFLOAT2 TexC;
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


#ifndef DISABLE_COPY
#define DISABLE_COPY(T) \
	explicit T(const T&) = delete; \
	T& operator=(const T&) = delete;
#endif

#ifndef DISABLE_MOVE
#define DISABLE_MOVE(T) \
	explicit T(const T&&) = delete; \
	T& operator=(const T&&) = delete;
#endif


#ifndef DISABLE_COPY_AND_MOVE
#define DISABLE_COPY_AND_MOVE(T) DISABLE_COPY(T) DISABLE_MOVE(T)
#endif

#ifdef _DEBUG
#ifndef DXCall
#define DXCall(x) \
if(FAILED(x)) \
{ \
	char line_number[32];\
	sprintf_s(line_number, "%u", __LINE__); \
	OutputDebugStringA("Error in: "); \
	OutputDebugStringA(__FILE__); \
	OutputDebugStringA("\nLine: "); \
	OutputDebugStringA(line_number); \
	OutputDebugStringA("\n"); \
	OutputDebugStringA(#x); \
	OutputDebugStringA("\n"); \
	__debugbreak(); \
} 
#endif // !DXCall
#else
#ifndef
#define DXCall(x) x
#endif
#endif

#ifdef _DEBUG
#define NAME_D3D12_OBJECT(obj, name) obj->SetName(name); OutputDebugString(L"::D3D12 Object Created: "); OutputDebugString(name); OutputDebugString(L"\n");
#define NAME_D3D12_OBJECT_INDEXED(obj, n, name) \
{\
wchar_t full_name[128];\
if(swprintf_s(full_name, L"%s[%u]", name, n) > 0)\
{ \
	obj->SetName(full_name); \
}} 
#else
#define NAME_D3D12_OBJECT(x, name);
#define NAME_D3D12_OBJECT_INDEXED(x, n, name);
#endif
