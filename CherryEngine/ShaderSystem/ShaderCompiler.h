#pragma once

#include "ShaderDesc.h"
#include <windows.h>
#include <dxcapi.h>
#include <cstdint>
#include <vector>
#include <wrl/client.h>

struct CompiledShader
{
	std::vector<uint8_t> bytecode;
};

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