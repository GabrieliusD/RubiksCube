#include "ShaderCompiler.h"
#include "RHI/D3DCommon.h"

#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
	std::wstring ToWString(const std::string& value)
	{
		if (value.empty()) return {};
		const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		std::wstring result(static_cast<size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
		result.pop_back();
		return result;
	}

	const wchar_t* DefaultProfile(ShaderType type)
	{
		switch (type)
		{
		case ShaderType::Vertex:  return L"vs_6_6";
		case ShaderType::Pixel:   return L"ps_6_6";
		case ShaderType::Compute: return L"cs_6_6";
		}
		return L"ps_6_6";
	}
}

void ShaderCompiler::Initialize()
{
	DX_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)));
	DX_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)));
	DX_CHECK(m_utils->CreateDefaultIncludeHandler(&m_includeHandler));
}

CompiledShader ShaderCompiler::Compile(const ShaderDesc& desc)
{
	if (!m_utils || !m_compiler)
	{
		Initialize();
	}

	CompiledShader output;
	const std::wstring filePath = desc.filePath.wstring();

	DxcBuffer sourceBuffer{};
	sourceBuffer.Encoding = DXC_CP_UTF8;
	ComPtr<IDxcBlobEncoding> sourceBlob;
	if (!desc.source.empty())
	{
		sourceBuffer.Ptr = desc.source.data();
		sourceBuffer.Size = desc.source.size();
	}
	else
	{
		if (FAILED(m_utils->LoadFile(filePath.c_str(), nullptr, &sourceBlob)))
		{
			output.errors = "Could not open shader file: " + desc.filePath.string();
			return output;
		}
		sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
		sourceBuffer.Size = sourceBlob->GetBufferSize();
	}

	std::vector<std::wstring> args;
	args.push_back(filePath);   // file name for diagnostics
	args.push_back(L"-E");
	args.push_back(ToWString(desc.entryPoint));
	args.push_back(L"-T");
	args.push_back(desc.profile.empty() ? DefaultProfile(desc.type) : ToWString(desc.profile));
	args.push_back(L"-Zpr");    // row-major matrices: matches DirectXMath without transposes
	args.push_back(L"-HV");
	args.push_back(L"2021");

	args.push_back(L"-I");
	args.push_back(desc.filePath.parent_path().wstring());
	for (const auto& dir : desc.includeDirs)
	{
		args.push_back(L"-I");
		args.push_back(dir.wstring());
	}
	for (const auto& define : desc.defines)
	{
		args.push_back(L"-D");
		args.push_back(ToWString(define));
	}

#if defined(_DEBUG)
	args.push_back(L"-Zi");
	args.push_back(L"-Od");
	args.push_back(L"-Qembed_debug");
#else
	args.push_back(L"-O3");
#endif

	std::vector<LPCWSTR> argPtrs;
	argPtrs.reserve(args.size());
	for (const auto& arg : args)
	{
		argPtrs.push_back(arg.c_str());
	}

	ComPtr<IDxcResult> result;
	if (FAILED(m_compiler->Compile(&sourceBuffer, argPtrs.data(), static_cast<UINT32>(argPtrs.size()),
		m_includeHandler.Get(), IID_PPV_ARGS(&result))))
	{
		output.errors = "DXC failed to run for " + desc.filePath.string();
		return output;
	}

	ComPtr<IDxcBlobUtf8> errors;
	result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (errors && errors->GetStringLength() > 0)
	{
		output.errors = errors->GetStringPointer();
	}

	HRESULT status = S_OK;
	result->GetStatus(&status);
	if (FAILED(status))
	{
		return output;
	}

	ComPtr<IDxcBlob> shaderBlob;
	if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr)) || !shaderBlob)
	{
		output.errors += "DXC produced no bytecode for " + desc.filePath.string();
		return output;
	}

	output.bytecode.resize(shaderBlob->GetBufferSize());
	std::memcpy(output.bytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
	output.success = true;
	return output;
}
