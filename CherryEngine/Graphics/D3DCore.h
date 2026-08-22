#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <d3dUtil.h>

using Microsoft::WRL::ComPtr;

class D3DCore
{
	inline static ComPtr<ID3D12Device5> mMainDevice;
	inline static ComPtr<IDXGIFactory4> mdxgiFactory = nullptr;


public:
	bool Initialize();
	static void Shutdown();
	static ComPtr<ID3D12Device5> Device()
	{
		return mMainDevice;
	}

	static ComPtr<IDXGIFactory4> Factory()
	{
		return mdxgiFactory;
	}
};
