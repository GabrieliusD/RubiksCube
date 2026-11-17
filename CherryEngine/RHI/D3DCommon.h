#pragma once

// Common D3D12 includes and error helpers for the RHI layer.
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>
#include <stdexcept>
#include "d3dx12.h"

namespace rhi
{
	using Microsoft::WRL::ComPtr;

	constexpr uint32_t kFramesInFlight = 2;
	constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

	// Thrown for unrecoverable D3D failures. The message includes the call site.
	class DxError : public std::runtime_error
	{
	public:
		DxError(HRESULT hr, const char* expr, const char* file, int line);
		HRESULT Result() const { return mResult; }
	private:
		HRESULT mResult;
	};

	void SetDebugName(ID3D12Object* object, const char* name);
	std::string HResultToString(HRESULT hr);
}

#define DX_CHECK(expr)                                                  \
	do {                                                                \
		HRESULT hr__ = (expr);                                          \
		if (FAILED(hr__)) throw ::rhi::DxError(hr__, #expr, __FILE__, __LINE__); \
	} while (0)
