// ShaderCompiler where there's no DXC (Android): shaders come from the SPIR-V cooked on Windows
// (ShaderSystem/ShaderCache.h), unpacked with the game's files.
#include "ShaderSystem/ShaderCompiler.h"
#include "ShaderSystem/ShaderCache.h"
#include "Core/Project.h"
#include <mutex>

namespace
{
	std::mutex gDirectoryMutex;
	std::filesystem::path gCacheDirectory;
}

struct ShaderCompiler::Impl {};

ShaderCompiler::ShaderCompiler() : mImpl(std::make_unique<Impl>()) {}
ShaderCompiler::~ShaderCompiler() = default;

void ShaderCompiler::Initialize() {}

CompiledShader ShaderCompiler::Compile(const ShaderDesc& desc)
{
	CompiledShader output;
	if (!desc.spirv)
	{
		output.errors = "Only SPIR-V shaders are available here (" + shadercache::Describe(desc) + ")";
		return output;
	}
	if (!shadercache::Read(CacheDirectory(), shadercache::Key(desc), output.bytecode))
	{
		output.errors = "This shader wasn't cooked: " + shadercache::Describe(desc) + " (export the project again)";
		return output;
	}
	output.success = true;
	return output;
}

void ShaderCompiler::SetCookDirectory(const std::filesystem::path&) {}
std::filesystem::path ShaderCompiler::CookDirectory() { return {}; }
uint32_t ShaderCompiler::CookedCount() { return 0; }

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

bool ShaderCompiler::CanCompile() { return false; }
