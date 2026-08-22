#pragma once

#include <filesystem>
#include <string>

enum class ShaderType
{
	Vertex,
	Fragment,
	Geometry,
	Compute,
	TessellationControl,
	TessellationEvaluation
};

struct ShaderDesc
{
	ShaderType type;
	std::string entryPoint;
	std::filesystem::path filePath;
	std::string profile;
};