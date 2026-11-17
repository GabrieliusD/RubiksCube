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
	std::filesystem::path filePath;
	std::string profile;                       // empty = default 6.6 profile for `type`
	std::vector<std::string> defines;          // "NAME" or "NAME=VALUE"
	std::vector<std::filesystem::path> includeDirs;
};
