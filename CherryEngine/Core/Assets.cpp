#include "Core/Assets.h"
#include "Core/AssetImport.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "GeometryGenerator.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace
{
	MeshData Convert(const GeometryGenerator::MeshData& source)
	{
		MeshData mesh;
		mesh.vertices.reserve(source.Vertices.size());
		for (const GeometryGenerator::Vertex& v : source.Vertices)
		{
			Vertex vertex;
			vertex.position = v.Position;
			vertex.normal = v.Normal;
			vertex.uv = v.TexC;
			mesh.vertices.push_back(vertex);
		}
		mesh.indices = source.Indices32;
		return mesh;
	}

	struct BuiltinMaterial
	{
		const char* name;
		DirectX::XMFLOAT4 color;
		float roughness;
		float metallic;
	};

	const BuiltinMaterial kBuiltinMaterials[] =
	{
		{ "engine/Default", { 0.70f, 0.70f, 0.70f, 1 }, 0.6f, 0.0f },
		{ "engine/White",   { 0.92f, 0.92f, 0.92f, 1 }, 0.5f, 0.0f },
		{ "engine/Dark",    { 0.08f, 0.08f, 0.09f, 1 }, 0.6f, 0.0f },
		{ "engine/Red",     { 0.80f, 0.12f, 0.10f, 1 }, 0.5f, 0.0f },
		{ "engine/Green",   { 0.20f, 0.65f, 0.25f, 1 }, 0.5f, 0.0f },
		{ "engine/Blue",    { 0.15f, 0.35f, 0.85f, 1 }, 0.5f, 0.0f },
		{ "engine/Yellow",  { 0.95f, 0.78f, 0.15f, 1 }, 0.5f, 0.0f },
		{ "engine/Gold",    { 1.00f, 0.77f, 0.34f, 1 }, 0.25f, 1.0f },
		{ "engine/Stone",   { 0.42f, 0.40f, 0.38f, 1 }, 0.9f, 0.0f },
	};
	const char* kBuiltinMeshes[] = { "engine/Cube", "engine/Sphere", "engine/Plane", "engine/Cylinder" };

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	bool IsBuiltin(const std::string& path) { return path.rfind("engine/", 0) == 0; }

	// "Meshes/rock.glb#2" -> ("Meshes/rock.glb", "2")
	std::pair<std::string, std::string> SplitSubAsset(const std::string& path)
	{
		const size_t hash = path.find('#');
		if (hash == std::string::npos) return { path, std::string() };
		return { path.substr(0, hash), path.substr(hash + 1) };
	}

	MaterialDesc ToDesc(const MaterialFile& file, TextureHandle texture)
	{
		MaterialDesc desc;
		desc.baseColor = file.baseColor;
		desc.roughness = std::clamp(file.roughness, 0.02f, 1.0f);
		const float m = std::clamp(file.metallic, 0.0f, 1.0f);
		desc.fresnelR0 = { 0.04f + (file.baseColor.x - 0.04f) * m, 0.04f + (file.baseColor.y - 0.04f) * m, 0.04f + (file.baseColor.z - 0.04f) * m };
		if (m > 0.0f)
		{
			// Metals have no diffuse colour.
			desc.baseColor = { file.baseColor.x * (1.0f - m), file.baseColor.y * (1.0f - m), file.baseColor.z * (1.0f - m), file.baseColor.w };
		}
		desc.albedoTexture = texture;
		return desc;
	}
}

struct AssetManager::Impl
{
	Renderer* renderer = nullptr;
	fs::path contentRoot;

	std::map<std::string, MeshHandle> meshes;
	std::map<std::string, MaterialHandle> materials;
	std::map<std::string, TextureHandle> textures;
	std::map<std::string, MaterialFile> materialFiles;   // loaded materials' source data (for texture reloads)
	std::set<std::string> reportedMissing;

	// Change tracking: file -> last write time seen.
	std::map<std::string, fs::file_time_type> watched;

	bool scanned = false;
	std::map<std::string, std::vector<std::string>> listing;   // type -> paths

	fs::path Full(const std::string& path) const { return contentRoot / fs::u8path(path); }

	void Watch(const std::string& file)
	{
		std::error_code ec;
		const fs::file_time_type time = fs::last_write_time(Full(file), ec);
		if (!ec) watched[file] = time;
	}

	void ReportMissing(const std::string& kind, const std::string& path, const std::string& why = {})
	{
		if (reportedMissing.insert(kind + ":" + path).second)
		{
			if (why.empty()) CHERRY_WARN("No %s asset \"%s\"", kind.c_str(), path.c_str());
			else CHERRY_WARN("Can't load %s \"%s\": %s", kind.c_str(), path.c_str(), why.c_str());
		}
	}

	MeshHandle LoadMesh(const std::string& path)
	{
		GeometryGenerator generator;
		if (path == "engine/Cube")     return renderer->CreateMesh(Convert(generator.CreateBox(1.0f, 1.0f, 1.0f, 0)));
		if (path == "engine/Sphere")   return renderer->CreateMesh(Convert(generator.CreateSphere(0.5f, 32, 16)));
		if (path == "engine/Plane")    return renderer->CreateMesh(Convert(generator.CreateGrid(1.0f, 1.0f, 2, 2)));
		if (path == "engine/Cylinder") return renderer->CreateMesh(Convert(generator.CreateCylinder(0.5f, 0.5f, 1.0f, 32, 1)));
		if (IsBuiltin(path) || contentRoot.empty()) return {};

		const std::string extension = Lower(fs::u8path(path).extension().u8string());
		if (extension != ".gltf" && extension != ".glb") return {};
		asset_import::GltfFile gltf;
		std::string error;
		if (!asset_import::LoadGltf(Full(path), true, gltf, error))
		{
			ReportMissing("mesh", path, error);
			return {};
		}
		Watch(path);
		return renderer->CreateMesh(gltf.mesh);
	}

	TextureHandle LoadTexture(const std::string& path)
	{
		if (contentRoot.empty() || path.empty()) return {};
		const auto [file, sub] = SplitSubAsset(path);
		asset_import::Image image;
		std::string error;
		bool ok = false;
		if (!sub.empty())
		{
			// An image inside a glTF file: "Meshes/rock.glb#image0".
			asset_import::GltfFile gltf;
			const int index = sub.rfind("image", 0) == 0 ? std::atoi(sub.c_str() + 5) : -1;
			if (asset_import::LoadGltf(Full(file), false, gltf, error) && index >= 0 && index < static_cast<int>(gltf.images.size()))
			{
				const asset_import::GltfImage& source = gltf.images[index];
				if (!source.encoded.empty()) ok = asset_import::LoadImageMemory(source.encoded.data(), source.encoded.size(), image, error);
				else ok = asset_import::LoadImageFile(Full(file).parent_path() / fs::u8path(source.uri), image, error);
			}
			else if (error.empty())
			{
				error = "no such image";
			}
		}
		else if (Lower(fs::u8path(file).extension().u8string()) == ".dds")
		{
			Watch(file);
			return renderer->LoadTexture(Full(file).wstring(), true);
		}
		else
		{
			ok = asset_import::LoadImageFile(Full(file), image, error);
		}
		if (!ok)
		{
			ReportMissing("texture", path, error);
			return {};
		}
		Watch(file);
		return renderer->CreateTexture(image.width, image.height, image.rgba.data(), true);
	}

	bool ReadMaterialFile(const std::string& path, MaterialFile& out, std::string& error)
	{
		for (const BuiltinMaterial& builtin : kBuiltinMaterials)
		{
			if (path != builtin.name) continue;
			out = MaterialFile{};
			out.baseColor = builtin.color;
			out.roughness = builtin.roughness;
			out.metallic = builtin.metallic;
			return true;
		}
		if (contentRoot.empty()) return false;

		const auto [file, sub] = SplitSubAsset(path);
		if (!sub.empty())
		{
			// A material inside a glTF file: "Meshes/rock.glb#0".
			asset_import::GltfFile gltf;
			if (!asset_import::LoadGltf(Full(file), false, gltf, error)) return false;
			const int index = std::atoi(sub.c_str());
			if (index < 0 || index >= static_cast<int>(gltf.materials.size()))
			{
				error = "no such material";
				return false;
			}
			out = gltf.materials[index];
			const int image = gltf.materialImages[index];
			if (image >= 0 && image < static_cast<int>(gltf.images.size()))
			{
				if (!gltf.images[image].encoded.empty())
				{
					out.albedoTexture = file + "#image" + std::to_string(image);
				}
				else
				{
					// A texture file next to the glTF: as a Content path.
					const fs::path texture = (Full(file).parent_path() / fs::u8path(gltf.images[image].uri)).lexically_normal();
					out.albedoTexture = texture.lexically_relative(contentRoot).generic_u8string();
				}
			}
			return true;
		}

		std::ifstream stream(Full(file));
		if (!stream)
		{
			error = "can't open the file";
			return false;
		}
		const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false);
		if (json.is_discarded() || !json.is_object())
		{
			error = "invalid JSON";
			return false;
		}
		out = MaterialFile{};
		if (json.contains("baseColor") && json["baseColor"].is_array() && json["baseColor"].size() >= 3)
		{
			const auto& c = json["baseColor"];
			out.baseColor = { c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c.size() > 3 ? c[3].get<float>() : 1.0f };
		}
		out.roughness = json.value("roughness", out.roughness);
		out.metallic = json.value("metallic", out.metallic);
		out.albedoTexture = json.value("albedoTexture", std::string());
		return true;
	}

	void ApplyMaterial(const std::string& path, MaterialHandle handle, const MaterialFile& file)
	{
		const TextureHandle texture = file.albedoTexture.empty() ? TextureHandle{} : GetTextureCached(file.albedoTexture);
		renderer->SetMaterial(handle, ToDesc(file, texture));
		materialFiles[path] = file;
	}

	TextureHandle GetTextureCached(const std::string& path)
	{
		auto it = textures.find(path);
		if (it != textures.end()) return it->second;
		const TextureHandle texture = LoadTexture(path);
		textures[path] = texture;
		return texture;
	}

	void Scan()
	{
		listing.clear();
		for (const char* mesh : kBuiltinMeshes) listing["mesh"].push_back(mesh);
		for (const BuiltinMaterial& material : kBuiltinMaterials) listing["material"].push_back(material.name);
		scanned = true;
		std::error_code ec;
		if (contentRoot.empty() || !fs::exists(contentRoot, ec)) return;

		for (fs::recursive_directory_iterator it(contentRoot, ec), end; it != end && !ec; it.increment(ec))
		{
			if (!it->is_regular_file(ec)) continue;
			const std::string path = it->path().lexically_relative(contentRoot).generic_u8string();
			const std::string type = AssetManager::TypeOf(path);
			if (type.empty()) continue;
			listing[type].push_back(path);
			if (type == "mesh")
			{
				// Its materials are assets too.
				asset_import::GltfFile gltf;
				std::string error;
				if (asset_import::LoadGltf(it->path(), false, gltf, error))
				{
					for (size_t i = 0; i < gltf.materials.size(); ++i) listing["material"].push_back(path + "#" + std::to_string(i));
				}
			}
		}
		for (auto& [type, paths] : listing)
		{
			auto firstFile = std::find_if(paths.begin(), paths.end(), [](const std::string& p) { return !IsBuiltin(p); });
			std::sort(firstFile, paths.end());
		}
	}
};

AssetManager::AssetManager() : mImpl(std::make_unique<Impl>()) {}
AssetManager::~AssetManager() = default;

void AssetManager::Initialize(Renderer& renderer) { mImpl->renderer = &renderer; }

void AssetManager::SetContentRoot(const fs::path& root)
{
	mImpl->contentRoot = root;
	mImpl->scanned = false;
}

const fs::path& AssetManager::ContentRoot() const { return mImpl->contentRoot; }

void AssetManager::Shutdown()
{
	Impl& a = *mImpl;
	if (!a.renderer) return;
	for (auto& [path, mesh] : a.meshes) if (mesh.IsValid()) a.renderer->DestroyMesh(mesh);
	for (auto& [path, material] : a.materials) if (material.IsValid()) a.renderer->DestroyMaterial(material);
	for (auto& [path, texture] : a.textures) if (texture.IsValid()) a.renderer->DestroyTexture(texture);
	a.meshes.clear();
	a.materials.clear();
	a.textures.clear();
	a.renderer = nullptr;
}

MeshHandle AssetManager::GetMesh(const std::string& path)
{
	Impl& a = *mImpl;
	if (!a.renderer || path.empty()) return {};
	auto it = a.meshes.find(path);
	if (it != a.meshes.end()) return it->second;
	const MeshHandle mesh = a.LoadMesh(path);
	if (!mesh.IsValid()) a.ReportMissing("mesh", path);
	a.meshes[path] = mesh;   // misses too, so a bad path doesn't retry every frame
	return mesh;
}

MaterialHandle AssetManager::GetMaterial(const std::string& path)
{
	Impl& a = *mImpl;
	if (!a.renderer || path.empty()) return {};
	auto it = a.materials.find(path);
	if (it != a.materials.end()) return it->second;

	MaterialFile file;
	std::string error;
	MaterialHandle material;
	if (a.ReadMaterialFile(path, file, error))
	{
		material = a.renderer->CreateMaterial(MaterialDesc{});
		a.ApplyMaterial(path, material, file);
		if (!IsBuiltin(path)) a.Watch(SplitSubAsset(path).first);
	}
	else
	{
		a.ReportMissing("material", path, error);
	}
	a.materials[path] = material;
	return material;
}

TextureHandle AssetManager::GetTexture(const std::string& path)
{
	Impl& a = *mImpl;
	if (!a.renderer || path.empty()) return {};
	return a.GetTextureCached(path);
}

std::vector<std::string> AssetManager::List(const std::string& type)
{
	if (!mImpl->scanned) mImpl->Scan();
	auto it = mImpl->listing.find(type);
	return it != mImpl->listing.end() ? it->second : std::vector<std::string>{};
}

void AssetManager::Refresh() { mImpl->Scan(); }

std::string AssetManager::TypeOf(const std::string& path)
{
	const auto [file, sub] = SplitSubAsset(path);
	const std::string extension = Lower(fs::u8path(file).extension().u8string());
	if (!sub.empty()) return sub.rfind("image", 0) == 0 ? "texture" : "material";
	if (extension == ".gltf" || extension == ".glb") return "mesh";
	if (extension == ".mat") return "material";
	if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp" || extension == ".dds") return "texture";
	if (extension == ".scene") return "scene";
	if (IsBuiltin(path))
	{
		for (const char* mesh : kBuiltinMeshes) if (path == mesh) return "mesh";
		return "material";
	}
	return {};
}

fs::path AssetManager::FullPath(const std::string& path) const { return mImpl->Full(SplitSubAsset(path).first); }

std::string AssetManager::AssetPathOf(const fs::path& file) const
{
	const fs::path relative = file.lexically_normal().lexically_relative(mImpl->contentRoot);
	if (relative.empty() || relative.native().rfind(L"..", 0) == 0) return {};
	return relative.generic_u8string();
}

bool AssetManager::ReadMaterial(const std::string& path, MaterialFile& out)
{
	std::string error;
	return mImpl->ReadMaterialFile(path, out, error);
}

bool AssetManager::WriteMaterial(const std::string& path, const MaterialFile& material)
{
	Impl& a = *mImpl;
	if (IsBuiltin(path) || path.find('#') != std::string::npos || a.contentRoot.empty()) return false;
	nlohmann::ordered_json json = nlohmann::ordered_json::object();
	json["baseColor"] = { material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w };
	json["roughness"] = material.roughness;
	json["metallic"] = material.metallic;
	json["albedoTexture"] = material.albedoTexture;

	std::error_code ec;
	fs::create_directories(a.Full(path).parent_path(), ec);
	std::ofstream stream(a.Full(path), std::ios::trunc);
	stream << json.dump(2) << "\n";
	if (!stream) return false;
	stream.close();

	// Apply now (not on the next change poll), and don't reload it again for this write.
	auto it = a.materials.find(path);
	if (it != a.materials.end() && it->second.IsValid()) a.ApplyMaterial(path, it->second, material);
	a.Watch(path);
	if (a.scanned && std::find(a.listing["material"].begin(), a.listing["material"].end(), path) == a.listing["material"].end()) a.Scan();
	return true;
}

void AssetManager::PreviewMaterial(const std::string& path, const MaterialFile& material)
{
	Impl& a = *mImpl;
	auto it = a.materials.find(path);
	if (it != a.materials.end() && it->second.IsValid()) a.ApplyMaterial(path, it->second, material);
}

void AssetManager::PollChanges(std::vector<std::pair<MeshHandle, MeshHandle>>& replacedMeshes)
{
	Impl& a = *mImpl;
	if (!a.renderer) return;
	std::vector<std::string> changed;
	for (auto& [file, time] : a.watched)
	{
		std::error_code ec;
		const fs::file_time_type now = fs::last_write_time(a.Full(file), ec);
		if (!ec && now != time)
		{
			time = now;
			changed.push_back(file);
		}
	}
	for (const std::string& file : changed)
	{
		CHERRY_LOG("Reloading %s", file.c_str());
		a.reportedMissing.clear();

		// Textures from this file (the file itself, or images inside a glTF).
		std::set<std::string> reloadedTextures;
		for (auto& [path, texture] : a.textures)
		{
			if (SplitSubAsset(path).first != file) continue;
			if (texture.IsValid()) a.renderer->DestroyTexture(texture);
			texture = a.LoadTexture(path);
			reloadedTextures.insert(path);
		}
		// Meshes: new handles.
		auto mesh = a.meshes.find(file);
		if (mesh != a.meshes.end())
		{
			const MeshHandle old = mesh->second;
			const MeshHandle fresh = a.LoadMesh(file);
			if (fresh.IsValid())
			{
				if (old.IsValid()) a.renderer->DestroyMesh(old);
				mesh->second = fresh;
				replacedMeshes.emplace_back(old, fresh);
			}
		}
		// Materials: in place (same handle), from the file or from the glTF, and every
		// material that uses a reloaded texture.
		for (auto& [path, material] : a.materials)
		{
			if (!material.IsValid()) continue;
			const bool fromFile = SplitSubAsset(path).first == file;
			const auto previous = a.materialFiles.find(path);
			const bool usesTexture = previous != a.materialFiles.end() && reloadedTextures.count(previous->second.albedoTexture) > 0;
			if (!fromFile && !usesTexture) continue;
			MaterialFile data;
			std::string error;
			if (fromFile ? a.ReadMaterialFile(path, data, error) : (data = previous->second, true)) a.ApplyMaterial(path, material, data);
		}
	}
}
