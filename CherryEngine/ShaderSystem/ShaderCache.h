#pragma once
#include "ShaderDesc.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Compiled SPIR-V shaders on disk, for devices that can't compile (Android has no DXC).
//
// The editor cooks them (CherryLaunch <project> --cook-shaders <dir>): while that runs, every
// shader the engine compiles is also compiled to SPIR-V and written to <dir>/<key>.spv. On
// Android, ShaderCompiler reads them back by the same key instead of compiling. The key is
// made from what identifies a compile: stage, entry point, defines, the file name (relative to
// the shader root) and the source text of generated shaders (surface shaders, graphs), so a
// surface shader whose text changed after cooking is missing, not stale.
namespace shadercache
{
	uint64_t Key(const ShaderDesc& desc);
	std::filesystem::path FileFor(const std::filesystem::path& directory, uint64_t key);
	bool Write(const std::filesystem::path& directory, uint64_t key, const std::vector<uint8_t>& spirv);
	bool Read(const std::filesystem::path& directory, uint64_t key, std::vector<uint8_t>& spirv);
	// What a key stands for, for messages ("MeshPass.hlsli VSMain").
	std::string Describe(const ShaderDesc& desc);
}
