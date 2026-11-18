#pragma once
#include "Core/Assets.h"
#include <filesystem>
#include <string>
#include <vector>

// File decoders used by AssetManager (engine-internal).
namespace asset_import
{
	struct Image
	{
		uint32_t width = 0, height = 0;
		std::vector<uint8_t> rgba;
	};

	bool LoadImageFile(const std::filesystem::path& file, Image& out, std::string& error);
	bool LoadImageMemory(const uint8_t* data, size_t size, Image& out, std::string& error);

	struct GltfImage
	{
		std::string uri;                 // file next to the glTF ("" when embedded)
		std::vector<uint8_t> encoded;    // embedded PNG/JPG bytes
	};

	struct GltfFile
	{
		MeshData mesh;                       // every primitive of every node, merged, in engine (left-handed) space
		std::vector<MaterialFile> materials; // albedoTexture holds "#image<i>" for images; the caller resolves it
		std::vector<int> materialImages;     // per material: image index of its base colour texture, or -1
		std::vector<GltfImage> images;
	};

	// `loadGeometry` false: only materials and images (cheap; for listing sub-assets).
	bool LoadGltf(const std::filesystem::path& file, bool loadGeometry, GltfFile& out, std::string& error);
}
