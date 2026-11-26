#pragma once
#include "RHI/RHI.h"
#include <filesystem>
#include <string>
#include <vector>

// A .dds file's top-level image and its mip chain, ready for Texture::Upload. Reads BC1-BC7
// (DXT1/3/5, ATI1/2 and DX10 headers) and 8-bit RGBA/BGRA; of a cube map or an array, only the
// first face or slice.
struct DdsImage
{
	struct Mip
	{
		size_t offset = 0;        // into `bytes`
		uint32_t rowPitch = 0;
		uint32_t slicePitch = 0;
	};
	uint32_t width = 0, height = 0, depth = 1;
	rhi::Format format = rhi::Format::Unknown;
	std::vector<uint8_t> bytes;
	std::vector<Mip> mips;
};

bool ReadDds(const std::filesystem::path& path, DdsImage& image, std::string& error);
