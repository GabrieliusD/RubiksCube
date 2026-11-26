#include "ShaderCompiler.h"
#include "ShaderCache.h"
#include "Core/Project.h"
#include "Core/Log.h"
#include "RHI/D3D12/D3DCommon.h"
#include <dxcapi.h>
#include <atomic>
#include <mutex>

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

namespace
{
	std::mutex gDirectoryMutex;
	std::filesystem::path gCookDirectory;
	std::filesystem::path gCacheDirectory;
	std::atomic<uint32_t> gCooked{ 0 };
}

struct ShaderCompiler::Impl
{
	ComPtr<IDxcUtils> utils;
	ComPtr<IDxcCompiler3> compiler;
	ComPtr<IDxcIncludeHandler> includeHandler;
	CompiledShader Run(const ShaderDesc& desc);
};

ShaderCompiler::ShaderCompiler() : mImpl(std::make_unique<Impl>()) {}
ShaderCompiler::~ShaderCompiler() = default;

void ShaderCompiler::SetCookDirectory(const std::filesystem::path& directory)
{
	std::lock_guard lock(gDirectoryMutex);
	gCookDirectory = directory;
}

std::filesystem::path ShaderCompiler::CookDirectory()
{
	std::lock_guard lock(gDirectoryMutex);
	return gCookDirectory;
}

uint32_t ShaderCompiler::CookedCount() { return gCooked.load(); }

void ShaderCompiler::SetCacheDirectory(const std::filesystem::path& directory)
{
	std::lock_guard lock(gDirectoryMutex);
	gCacheDirectory = directory;
}

std::filesystem::path ShaderCompiler::CacheDirectory()
{
	std::lock_guard lock(gDirectoryMutex);
	return gCacheDirectory.empty() ? cherry::ExecutableDir() / "ShaderCache" : gCacheDirectory;
}

bool ShaderCompiler::CanCompile() { return true; }

void ShaderCompiler::Initialize()
{
	DX_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&mImpl->utils)));
	DX_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&mImpl->compiler)));
	DX_CHECK(mImpl->utils->CreateDefaultIncludeHandler(&mImpl->includeHandler));
}

namespace
{
	// CHERRY_SHADER_CACHE=<dir>: SPIR-V comes only from cooked shaders, as on Android (for
	// checking a cook on Windows with --rhi vulkan).
	const std::filesystem::path& CacheOnlyDirectory()
	{
		static const std::filesystem::path directory = []
		{
			char* value = nullptr;
			size_t length = 0;
			std::filesystem::path result;
			if (_dupenv_s(&value, &length, "CHERRY_SHADER_CACHE") == 0 && value && *value) result = value;
			free(value);
			return result;
		}();
		return directory;
	}
}

CompiledShader ShaderCompiler::Compile(const ShaderDesc& desc)
{
	if (desc.spirv && !CacheOnlyDirectory().empty())
	{
		CompiledShader cached;
		if (shadercache::Read(CacheOnlyDirectory(), shadercache::Key(desc), cached.bytecode)) cached.success = true;
		else cached.errors = "This shader wasn't cooked: " + shadercache::Describe(desc);
		return cached;
	}
	if (!mImpl->utils || !mImpl->compiler)
	{
		Initialize();
	}
	CompiledShader output = mImpl->Run(desc);
	const std::filesystem::path cook = CookDirectory();
	if (output.success && !cook.empty())
	{
		// Cooking: the SPIR-V of this compile goes into the cache too.
		ShaderDesc spirvDesc = desc;
		spirvDesc.spirv = true;
		const CompiledShader spirv = desc.spirv ? output : mImpl->Run(spirvDesc);
		if (spirv.success && shadercache::Write(cook, shadercache::Key(spirvDesc), spirv.bytecode)) ++gCooked;
		else if (!spirv.success) CHERRY_ERROR("Cooking %s for Vulkan failed: %s", shadercache::Describe(spirvDesc).c_str(), spirv.errors.c_str());
	}
	return output;
}

CompiledShader ShaderCompiler::Impl::Run(const ShaderDesc& desc)
{
	auto& m_utils = utils;
	auto& m_compiler = compiler;
	auto& m_includeHandler = includeHandler;

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

	if (desc.spirv)
	{
		for (const wchar_t* arg : { L"-spirv", L"-fspv-target-env=vulkan1.1", L"-fspv-entrypoint-name=main", L"-fvk-use-dx-layout", L"-fspv-use-unknown-image-format",
			L"-fvk-bind-resource-heap", L"0", L"1", L"-fvk-b-shift", L"0", L"0", L"-fvk-s-shift", L"16", L"0", L"-D", L"CHERRY_VULKAN=1" })
		{
			args.push_back(arg);
		}
	}
#if defined(_DEBUG)
	args.push_back(L"-Od");
	if (!desc.spirv)
	{
		args.push_back(L"-Zi");
		args.push_back(L"-Qembed_debug");
	}
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
