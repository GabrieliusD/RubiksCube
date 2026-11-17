#pragma once

#include "ShaderDesc.h"
#include <windows.h>
#include <dxcapi.h>
#include <cstdint>
#include <vector>
#include <wrl/client.h>

struct CompiledShader
{
	bool success = false;
	std::vector<uint8_t> bytecode;
	std::string errors;   // compiler diagnostics (file:line: message), also set on success for warnings
};

// Thin wrapper over DXC. Compile errors are returned, not thrown, so callers can
// keep the previous pipeline alive when a hot-reloaded shader fails to compile.
class ShaderCompiler
{
public:
	void Initialize();
	CompiledShader Compile(const ShaderDesc& desc);

private:
	Microsoft::WRL::ComPtr<IDxcUtils> m_utils;
	Microsoft::WRL::ComPtr<IDxcCompiler3> m_compiler;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> m_includeHandler;
};
