#include "RHI/D3DCommon.h"
#include <cstdio>

namespace rhi
{
	static std::string FormatDxError(HRESULT hr, const char* expr, const char* file, int line)
	{
		char buffer[1024];
		std::snprintf(buffer, sizeof(buffer), "%s failed (%s) at %s:%d", expr, HResultToString(hr).c_str(), file, line);
		return buffer;
	}

	DxError::DxError(HRESULT hr, const char* expr, const char* file, int line)
		: std::runtime_error(FormatDxError(hr, expr, file, line)), mResult(hr)
	{
	}

	void SetDebugName(ID3D12Object* object, const char* name)
	{
		if (!object || !name) return;
		wchar_t wide[256];
		MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 256);
		object->SetName(wide);
	}

	std::string HResultToString(HRESULT hr)
	{
		char* message = nullptr;
		FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
		std::string result = buffer;
		if (message)
		{
			result += " ";
			result += message;
			LocalFree(message);
			while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
		}
		return result;
	}
}
