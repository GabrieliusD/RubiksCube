#pragma once

#include "ShaderDesc.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct CompiledShader
{
	bool success = false;
	std::vector<uint8_t> bytecode;
	std::string errors;   // compiler diagnostics (file:line: message), also set on success for warnings
};

// Compiles HLSL to DXIL or SPIR-V with DXC (Windows). Compile errors are returned, not thrown,
// so callers can keep the previous pipeline alive when a hot-reloaded shader fails to compile.
//
// Android has no DXC: there, Compile reads SPIR-V cooked on Windows (ShaderSystem/ShaderCache.h)
// from CacheDirectory().
class ShaderCompiler
{
public:
	ShaderCompiler();
	~ShaderCompiler();
	ShaderCompiler(const ShaderCompiler&) = delete;
	ShaderCompiler& operator=(const ShaderCompiler&) = delete;

	void Initialize();
	CompiledShader Compile(const ShaderDesc& desc);

	// Cooking (Windows): while set, every compile also writes its SPIR-V to `directory`.
	static void SetCookDirectory(const std::filesystem::path& directory);
	static std::filesystem::path CookDirectory();
	// How many shaders have been cooked so far.
	static uint32_t CookedCount();
	// Where cooked shaders are read from when this build can't compile (default: ExecutableDir()/ShaderCache).
	static void SetCacheDirectory(const std::filesystem::path& directory);
	static std::filesystem::path CacheDirectory();
	// False where shaders only come from the cache (Android).
	static bool CanCompile();

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
