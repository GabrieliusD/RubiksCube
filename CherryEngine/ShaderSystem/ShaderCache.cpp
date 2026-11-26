#include "ShaderSystem/ShaderCache.h"
#include <cstdio>
#include <fstream>
#include <system_error>

namespace shadercache
{
	namespace
	{
		struct Hash
		{
			uint64_t value = 1469598103934665603ull;
			void Bytes(const void* data, size_t size)
			{
				const auto* bytes = static_cast<const uint8_t*>(data);
				for (size_t i = 0; i < size; ++i)
				{
					value ^= bytes[i];
					value *= 1099511628211ull;
				}
			}
			void Text(const std::string& text)
			{
				const uint64_t size = text.size();
				Bytes(&size, sizeof(size));
				Bytes(text.data(), text.size());
			}
		};

		// The file relative to the shader root (the first include directory), with / separators.
		std::string RelativeName(const ShaderDesc& desc)
		{
			std::filesystem::path name = desc.filePath;
			if (!desc.includeDirs.empty())
			{
				const std::filesystem::path relative = desc.filePath.lexically_relative(desc.includeDirs.front());
				if (!relative.empty() && relative.native()[0] != '.') name = relative;
			}
			return name.generic_string();
		}
	}

	uint64_t Key(const ShaderDesc& desc)
	{
		Hash hash;
		hash.Text("cherry-spirv-1");
		const uint32_t type = static_cast<uint32_t>(desc.type);
		hash.Bytes(&type, sizeof(type));
		hash.Text(desc.entryPoint);
		hash.Text(desc.profile);
		hash.Text(RelativeName(desc));
		for (const std::string& define : desc.defines) hash.Text(define);
		hash.Text(desc.source);
		return hash.value;
	}

	std::filesystem::path FileFor(const std::filesystem::path& directory, uint64_t key)
	{
		char name[32];
		std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(key));
		return directory / name;
	}

	bool Write(const std::filesystem::path& directory, uint64_t key, const std::vector<uint8_t>& spirv)
	{
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		const std::filesystem::path file = FileFor(directory, key);
		// Through a temporary name: worker threads may cook the same shader at once.
		std::filesystem::path temporary = file;
		temporary += "." + std::to_string(reinterpret_cast<uintptr_t>(&spirv)) + ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) return false;
			out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
			if (!out) return false;
		}
		std::filesystem::rename(temporary, file, ec);
		if (ec) std::filesystem::remove(temporary, ec);
		return true;
	}

	bool Read(const std::filesystem::path& directory, uint64_t key, std::vector<uint8_t>& spirv)
	{
		std::ifstream in(FileFor(directory, key), std::ios::binary);
		if (!in) return false;
		spirv.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return !spirv.empty() && spirv.size() % 4 == 0;
	}

	std::string Describe(const ShaderDesc& desc)
	{
		std::string text = RelativeName(desc) + " " + desc.entryPoint;
		for (const std::string& define : desc.defines) text += " -D" + define;
		return text;
	}
}
