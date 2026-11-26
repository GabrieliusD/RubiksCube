#include "Renderer/DdsReader.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace
{
	constexpr uint32_t FourCC(char a, char b, char c, char d)
	{
		return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
	}

	struct PixelFormat
	{
		uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
	};
	struct Header
	{
		uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
		uint32_t reserved1[11];
		PixelFormat pixelFormat;
		uint32_t caps, caps2, caps3, caps4, reserved2;
	};
	struct HeaderDx10
	{
		uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
	};
	static_assert(sizeof(Header) == 124, "DDS_HEADER");

	constexpr uint32_t kFourCCFlag = 0x4;
	constexpr uint32_t kRgbFlag = 0x40;
	constexpr uint32_t kVolumeCaps2 = 0x200000;

	rhi::Format FromDxgi(uint32_t dxgi)
	{
		switch (dxgi)
		{
		case 28: return rhi::Format::RGBA8Unorm;      // R8G8B8A8_UNORM
		case 29: return rhi::Format::RGBA8UnormSrgb;
		case 87: return rhi::Format::BGRA8Unorm;      // B8G8R8A8_UNORM
		case 91: return rhi::Format::BGRA8UnormSrgb;
		case 88: return rhi::Format::BGRX8Unorm;
		case 93: return rhi::Format::BGRX8UnormSrgb;
		case 10: return rhi::Format::RGBA16Float;
		case 2:  return rhi::Format::RGBA32Float;
		case 61: return rhi::Format::R8Unorm;
		case 71: return rhi::Format::BC1Unorm;
		case 72: return rhi::Format::BC1UnormSrgb;
		case 74: return rhi::Format::BC2Unorm;
		case 75: return rhi::Format::BC2UnormSrgb;
		case 77: return rhi::Format::BC3Unorm;
		case 78: return rhi::Format::BC3UnormSrgb;
		case 80: return rhi::Format::BC4Unorm;
		case 83: return rhi::Format::BC5Unorm;
		case 95: return rhi::Format::BC6HUfloat;
		case 98: return rhi::Format::BC7Unorm;
		case 99: return rhi::Format::BC7UnormSrgb;
		default: return rhi::Format::Unknown;
		}
	}
}

bool ReadDds(const std::filesystem::path& path, DdsImage& image, std::string& error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		error = "can't open the file";
		return false;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (data.size() < 4 + sizeof(Header) || std::memcmp(data.data(), "DDS ", 4) != 0)
	{
		error = "not a DDS file";
		return false;
	}
	Header header;
	std::memcpy(&header, data.data() + 4, sizeof(Header));
	size_t offset = 4 + sizeof(Header);
	const PixelFormat& pf = header.pixelFormat;
	uint32_t arraySize = 1;
	bool cube = (header.caps2 & 0x200) != 0;
	if ((pf.flags & kFourCCFlag) && pf.fourCC == FourCC('D', 'X', '1', '0'))
	{
		if (data.size() < offset + sizeof(HeaderDx10)) { error = "truncated DX10 header"; return false; }
		HeaderDx10 dx10;
		std::memcpy(&dx10, data.data() + offset, sizeof(dx10));
		offset += sizeof(dx10);
		image.format = FromDxgi(dx10.dxgiFormat);
		arraySize = std::max(dx10.arraySize, 1u);
		cube = (dx10.miscFlag & 0x4) != 0;
	}
	else if (pf.flags & kFourCCFlag)
	{
		switch (pf.fourCC)
		{
		case FourCC('D', 'X', 'T', '1'): image.format = rhi::Format::BC1Unorm; break;
		case FourCC('D', 'X', 'T', '2'): case FourCC('D', 'X', 'T', '3'): image.format = rhi::Format::BC2Unorm; break;
		case FourCC('D', 'X', 'T', '4'): case FourCC('D', 'X', 'T', '5'): image.format = rhi::Format::BC3Unorm; break;
		case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): image.format = rhi::Format::BC4Unorm; break;
		case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): image.format = rhi::Format::BC5Unorm; break;
		default: break;
		}
	}
	else if ((pf.flags & kRgbFlag) && pf.rgbBitCount == 32)
	{
		if (pf.rMask == 0x000000ff && pf.gMask == 0x0000ff00 && pf.bMask == 0x00ff0000) image.format = rhi::Format::RGBA8Unorm;
		else if (pf.rMask == 0x00ff0000 && pf.gMask == 0x0000ff00 && pf.bMask == 0x000000ff)
			image.format = pf.aMask ? rhi::Format::BGRA8Unorm : rhi::Format::BGRX8Unorm;
	}
	if (image.format == rhi::Format::Unknown)
	{
		error = "unsupported DDS pixel format";
		return false;
	}

	image.width = std::max(header.width, 1u);
	image.height = std::max(header.height, 1u);
	image.depth = (header.caps2 & kVolumeCaps2) ? std::max(header.depth, 1u) : 1u;
	const uint32_t mipCount = std::max(header.mipMapCount, 1u);
	const bool blocks = rhi::IsBlockCompressed(image.format);
	const uint32_t unit = rhi::FormatBytes(image.format);

	// The first face or slice: its mips are stored one after another.
	uint32_t w = image.width, h = image.height, d = image.depth;
	size_t cursor = offset;
	for (uint32_t mip = 0; mip < mipCount; ++mip)
	{
		const uint32_t rowPitch = blocks ? std::max(1u, (w + 3) / 4) * unit : w * unit;
		const uint32_t rows = blocks ? std::max(1u, (h + 3) / 4) : h;
		const uint32_t slicePitch = rowPitch * rows;
		const size_t bytes = static_cast<size_t>(slicePitch) * d;
		if (cursor + bytes > data.size()) break;
		image.mips.push_back({ cursor - offset, rowPitch, slicePitch });
		cursor += bytes;
		w = std::max(w / 2, 1u);
		h = std::max(h / 2, 1u);
		d = std::max(d / 2, 1u);
	}
	if (image.mips.empty())
	{
		error = "truncated DDS data";
		return false;
	}
	(void)cube;
	(void)arraySize;
	image.bytes.assign(data.begin() + offset, data.begin() + cursor);
	return true;
}
