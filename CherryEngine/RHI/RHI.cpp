#include "RHI/RHI.h"
#include <algorithm>
#include <cctype>

namespace rhi
{
#if defined(_WIN32)
	std::unique_ptr<Device> CreateD3D12Device(const DeviceOptions& options, void* nativeWindow, uint32_t width, uint32_t height, std::string& error);
#endif
#if defined(CHERRY_RHI_VULKAN)
	std::unique_ptr<Device> CreateVulkanDevice(const DeviceOptions& options, void* nativeWindow, uint32_t width, uint32_t height, std::string& error);
#endif

	std::unique_ptr<Device> Device::Create(Backend backend, const DeviceOptions& options, void* nativeWindow,
		uint32_t width, uint32_t height, std::string& error)
	{
		switch (backend)
		{
#if defined(_WIN32)
		case Backend::D3D12: return CreateD3D12Device(options, nativeWindow, width, height, error);
#endif
#if defined(CHERRY_RHI_VULKAN)
		case Backend::Vulkan: return CreateVulkanDevice(options, nativeWindow, width, height, error);
#endif
		default: break;
		}
		error = std::string("The ") + BackendName(backend) + " backend isn't built into this engine.";
		return nullptr;
	}

	const char* BackendName(Backend backend)
	{
		return backend == Backend::Vulkan ? "Vulkan" : "D3D12";
	}

	bool ParseBackend(const std::string& text, Backend& backend)
	{
		std::string lower = text;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lower == "d3d12" || lower == "dx12" || lower == "directx12" || lower == "directx") { backend = Backend::D3D12; return true; }
		if (lower == "vulkan" || lower == "vk") { backend = Backend::Vulkan; return true; }
		return false;
	}

	bool IsBlockCompressed(Format format)
	{
		return format >= Format::BC1Unorm && format <= Format::BC7UnormSrgb;
	}

	uint32_t FormatBytes(Format format)
	{
		switch (format)
		{
		case Format::RGBA8Unorm: case Format::RGBA8UnormSrgb: case Format::BGRA8Unorm: case Format::BGRA8UnormSrgb:
		case Format::BGRX8Unorm: case Format::BGRX8UnormSrgb: case Format::RG16Float: case Format::R32Float:
		case Format::RGB10A2Unorm: case Format::RG11B10Float: case Format::D32Float:
			return 4;
		case Format::RGBA16Float: case Format::RG32Float: return 8;
		case Format::RGBA32Float: return 16;
		case Format::R16Float: case Format::RG8Unorm: case Format::R16Unorm: return 2;
		case Format::R8Unorm: case Format::R8Uint: return 1;
		case Format::BC1Unorm: case Format::BC1UnormSrgb: case Format::BC4Unorm: return 8;
		case Format::BC2Unorm: case Format::BC2UnormSrgb: case Format::BC3Unorm: case Format::BC3UnormSrgb:
		case Format::BC5Unorm: case Format::BC6HUfloat: case Format::BC7Unorm: case Format::BC7UnormSrgb:
			return 16;
		default: return 0;
		}
	}

	Format ToSrgb(Format format)
	{
		switch (format)
		{
		case Format::RGBA8Unorm: return Format::RGBA8UnormSrgb;
		case Format::BGRA8Unorm: return Format::BGRA8UnormSrgb;
		case Format::BGRX8Unorm: return Format::BGRX8UnormSrgb;
		case Format::BC1Unorm: return Format::BC1UnormSrgb;
		case Format::BC2Unorm: return Format::BC2UnormSrgb;
		case Format::BC3Unorm: return Format::BC3UnormSrgb;
		case Format::BC7Unorm: return Format::BC7UnormSrgb;
		default: return format;
		}
	}
}
