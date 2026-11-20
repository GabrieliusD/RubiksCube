#pragma once

#include <filesystem>
#include <string>
#include <vector>

enum class ShaderType
{
	Vertex,
	Pixel,
	Compute,
};

struct ShaderDesc
{
	ShaderType type = ShaderType::Vertex;
	std::string entryPoint;
	std::filesystem::path filePath;            // the file to compile; with `source`, only the name for diagnostics
	std::string source;                        // HLSL to compile instead of reading filePath (generated shaders)
	std::string profile;                       // empty = default 6.6 profile for `type`
	std::vector<std::string> defines;          // "NAME" or "NAME=VALUE"
	std::vector<std::filesystem::path> includeDirs;
};
