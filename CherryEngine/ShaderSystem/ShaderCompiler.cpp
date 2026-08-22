#include "ShaderCompiler.h"
#include "CommonMacros.h"

#include <dxcapi.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
	std::wstring ToWString(const std::string& value)
	{
		return std::wstring(value.begin(), value.end());
	}

	std::wstring GetDefaultProfile(ShaderType type)
	{
		switch (type)
		{
		case ShaderType::Vertex: return L"vs_6_6";
		case ShaderType::Fragment: return L"ps_6_6";
		case ShaderType::Geometry: return L"gs_6_6";
		case ShaderType::Compute: return L"cs_6_6";
		case ShaderType::TessellationControl: return L"hs_6_6";
		case ShaderType::TessellationEvaluation: return L"ds_6_6";
		default: return L"ps_6_6";
		}
	}
}

void ShaderCompiler::Initialize()
{
	ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)));
	ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)));
	ThrowIfFailed(m_utils->CreateDefaultIncludeHandler(&m_includeHandler));
}

CompiledShader ShaderCompiler::Compile(const ShaderDesc& desc)
{
	if (!m_utils || !m_compiler)
	{
		Initialize();
	}

	const std::wstring filePath = desc.filePath.wstring();
	ComPtr<IDxcBlobEncoding> sourceBlob;
	ThrowIfFailed(m_utils->LoadFile(filePath.c_str(), nullptr, &sourceBlob));

	DxcBuffer sourceBuffer = {};
	sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
	sourceBuffer.Size = sourceBlob->GetBufferSize();
	sourceBuffer.Encoding = DXC_CP_UTF8;

	const std::wstring entryPoint = ToWString(desc.entryPoint);
	const std::wstring profile = desc.profile.empty() ? GetDefaultProfile(desc.type) : ToWString(desc.profile);
	const std::wstring shaderDir = desc.filePath.parent_path().wstring();

	std::vector<std::wstring> argStorage;
	argStorage.push_back(filePath);      // important for include/diagnostics context
	argStorage.push_back(L"-E");
	argStorage.push_back(entryPoint);
	argStorage.push_back(L"-T");
	argStorage.push_back(profile);
	argStorage.push_back(L"-Zpc");

	if (!shaderDir.empty())
	{
		argStorage.push_back(L"-I");
		argStorage.push_back(shaderDir); // resolves #include "LightingUtil.hlsl"
	}

#if defined(DEBUG) || defined(_DEBUG)
	argStorage.push_back(L"-Zi");
	argStorage.push_back(L"-Od");
	argStorage.push_back(L"-Qembed_debug");
#else
	argStorage.push_back(L"-O3");
#endif

	std::vector<LPCWSTR> arguments;
	arguments.reserve(argStorage.size());
	for (const auto& arg : argStorage)
	{
		arguments.push_back(arg.c_str());
	}

	ComPtr<IDxcResult> result;
	ThrowIfFailed(m_compiler->Compile(
		&sourceBuffer,
		arguments.data(),
		static_cast<UINT32>(arguments.size()),
		m_includeHandler.Get(),
		IID_PPV_ARGS(&result)));

	ComPtr<IDxcBlobUtf8> errors;
	result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (errors && errors->GetStringLength() > 0)
	{
		OutputDebugStringA(errors->GetStringPointer());
	}

	HRESULT hrStatus = S_OK;
	ThrowIfFailed(result->GetStatus(&hrStatus));
	ThrowIfFailed(hrStatus);

	ComPtr<IDxcBlob> shaderBlob;
	ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr));

	CompiledShader compiledShader;
	compiledShader.bytecode.resize(shaderBlob->GetBufferSize());
	std::memcpy(compiledShader.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
	return compiledShader;
}
