#include "Core/Assets.h"
#include "Core/AssetImport.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "GeometryGenerator.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
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

	MaterialDesc ToDesc(const MaterialFile& file, TextureHandle texture, SurfaceShaderHandle shader)
	{
		MaterialDesc desc;
		desc.baseColor = file.baseColor;
		desc.roughness = std::clamp(file.roughness, 0.02f, 1.0f);
		desc.metallic = std::clamp(file.metallic, 0.0f, 1.0f);
		desc.albedoTexture = texture;
		desc.shader = shader;
		desc.params = file.params;
		return desc;
	}

	bool IsGraphPath(const std::string& path) { return Lower(fs::u8path(path).extension().u8string()) == ".shadergraph"; }
}

struct AssetManager::Impl
{
	Renderer* renderer = nullptr;
	fs::path contentRoot;

	std::map<std::string, MeshHandle> meshes;
	std::map<std::string, MaterialHandle> materials;
	std::map<std::string, TextureHandle> textures;
	std::map<std::string, TextureHandle> linearTextures;   // the same files loaded without sRGB decoding

	// A surface shader asset (.hlsl or .shadergraph).
	struct ShaderEntry
	{
		SurfaceShaderHandle handle;
		bool graph = false;
		std::vector<sg::GraphParameter> parameters;   // graph: layout of the code that's running
		std::vector<sg::GraphError> errors;           // graph: last compile
		std::vector<std::string> dependencies;        // graph: subgraphs it uses
	};
	std::map<std::string, ShaderEntry> shaders;
	std::map<std::string, std::unique_ptr<sg::Graph>> graphs;   // graph files (nullptr: can't load)
	std::map<std::string, sg::Graph> previewGraphs;             // being edited, not saved
	std::map<std::string, MaterialFile> materialFiles;   // loaded materials' source data (for texture reloads)
	std::map<std::string, std::string> prefabs;          // prefab path -> JSON text
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

	TextureHandle LoadTexture(const std::string& path, bool srgb = true)
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
			return renderer->LoadTexture(Full(file).wstring(), srgb);
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
		return renderer->CreateTexture(image.width, image.height, image.rgba.data(), srgb);
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
		out.shader = json.value("shader", std::string());
		if (json.contains("params") && json["params"].is_array())
		{
			for (const auto& value : json["params"]) out.params.push_back(value.is_number() ? value.get<float>() : 0.0f);
		}
		if (json.contains("parameters") && json["parameters"].is_object())
		{
			for (const auto& [name, value] : json["parameters"].items())
			{
				MaterialParameter parameter;
				if (value.is_string()) parameter.texture = value.get<std::string>();
				else if (value.is_number()) parameter.value[0] = value.get<float>();
				else if (value.is_array())
				{
					for (size_t i = 0; i < 4 && i < value.size(); ++i) parameter.value[i] = value[i].is_number() ? value[i].get<float>() : 0.0f;
				}
				else continue;
				out.parameters[name] = parameter;
			}
		}
		return true;
	}

	void ApplyMaterial(const std::string& path, MaterialHandle handle, const MaterialFile& file)
	{
		const TextureHandle texture = file.albedoTexture.empty() ? TextureHandle{} : GetTextureCached(file.albedoTexture);
		const SurfaceShaderHandle shader = file.shader.empty() ? SurfaceShaderHandle{} : GetShaderCached(file.shader);
		MaterialDesc desc = ToDesc(file, texture, shader);

		// A shader graph's parameter block: the graph's defaults, overridden by the material.
		auto entry = shaders.find(file.shader);
		if (entry != shaders.end() && entry->second.graph)
		{
			desc.params.clear();
			desc.textures.clear();
			for (const sg::GraphParameter& param : entry->second.parameters)
			{
				const auto set = file.parameters.find(param.name);
				if (param.kind == sg::ParamKind::Texture)
				{
					if (desc.textures.size() <= param.offset) desc.textures.resize(param.offset + 1);
					const std::string& asset = set != file.parameters.end() && !set->second.texture.empty() ? set->second.texture : param.defaultTexture;
					if (!asset.empty()) desc.textures[param.offset] = GetTextureCached(asset, !param.linearTexture);
					continue;
				}
				const uint32_t count = param.kind == sg::ParamKind::Scalar ? 1 : 4;
				if (desc.params.size() < param.offset + count) desc.params.resize(param.offset + count, 0.0f);
				const sg::Float4& value = set != file.parameters.end() ? set->second.value : param.defaultValue;
				for (uint32_t i = 0; i < count; ++i) desc.params[param.offset + i] = value[i];
			}
		}
		renderer->SetMaterial(handle, desc);
		materialFiles[path] = file;
	}

	// Re-applies every loaded material that `uses` (after its shader or a texture changed).
	void ReapplyMaterials(const std::function<bool(const MaterialFile&)>& uses)
	{
		for (auto& [path, material] : materials)
		{
			auto file = materialFiles.find(path);
			if (material.IsValid() && file != materialFiles.end() && uses(file->second))
			{
				const MaterialFile data = file->second;   // ApplyMaterial writes materialFiles
				ApplyMaterial(path, material, data);
			}
		}
	}

	TextureHandle GetTextureCached(const std::string& path, bool srgb = true)
	{
		std::map<std::string, TextureHandle>& cache = srgb ? textures : linearTextures;
		auto it = cache.find(path);
		if (it != cache.end()) return it->second;
		const TextureHandle texture = LoadTexture(path, srgb);
		cache[path] = texture;
		return texture;
	}

	// ---- Shader graphs -------------------------------------------------------------------------

	const sg::Graph* LoadGraph(const std::string& path)
	{
		auto preview = previewGraphs.find(path);
		if (preview != previewGraphs.end()) return &preview->second;
		auto it = graphs.find(path);
		if (it != graphs.end()) return it->second.get();
		std::unique_ptr<sg::Graph> graph;
		std::string text, error;
		if (!contentRoot.empty() && ReadText(path, text))
		{
			graph = std::make_unique<sg::Graph>();
			if (!sg::GraphFromJson(text, *graph, error))
			{
				ReportMissing("shader graph", path, error);
				graph.reset();
			}
			Watch(path);
		}
		else
		{
			ReportMissing("shader graph", path);
		}
		return (graphs[path] = std::move(graph)).get();
	}

	sg::GraphLoader Loader()
	{
		return [this](const std::string& path) { return LoadGraph(path); };
	}

	// (Re)compiles a shader graph shader from its current graph (the preview, else the file).
	sg::CompiledGraph BuildGraphShader(const std::string& path, ShaderEntry& entry)
	{
		sg::CompiledGraph compiled;
		const sg::Graph* graph = LoadGraph(path);
		if (!graph)
		{
			compiled.errors.push_back({ 0, "Can't read the shader graph file" });
		}
		else
		{
			compiled = sg::CompileGraph(*graph, Loader());
		}
		entry.errors = compiled.errors;
		entry.dependencies = compiled.dependencies;

		SurfaceShaderOptions options;
		options.translucent = compiled.blend == sg::BlendMode::Translucent;
		options.boundsPadding = compiled.boundsPadding;
		if (compiled.ok)
		{
			// Keep the generated code next to the project for debugging.
			std::error_code ec;
			const fs::path cache = contentRoot.parent_path() / "Saved" / "ShaderCache" / fs::u8path(path + ".hlsl");
			fs::create_directories(cache.parent_path(), ec);
			std::ofstream(cache, std::ios::binary) << compiled.hlsl;

			// The parameter layout goes with the code: materials switch to it once the build of
			// this version is done (until then the previous version runs, with its layout).
			auto adopt = [this, path, parameters = compiled.parameters](bool ok)
			{
				auto built = shaders.find(path);
				if (!ok || built == shaders.end()) return;
				built->second.parameters = parameters;
				ReapplyMaterials([&](const MaterialFile& file) { return file.shader == path; });
			};
			if (entry.handle.IsValid()) renderer->SetSurfaceShaderSource(entry.handle, compiled.hlsl, options, adopt);
			else entry.handle = renderer->CreateSurfaceShader(path, compiled.hlsl, options, adopt);
		}
		else if (!entry.handle.IsValid())
		{
			// Never compiled: a shader that fails, so meshes using it show the error checker.
			entry.handle = renderer->CreateSurfaceShader(path, "#error This shader graph has errors: open it in the graph editor\n", options);
		}
		return compiled;
	}

	// Recompiles the graph shaders that use `file` as a subgraph (directly or not).
	void RebuildDependents(const std::string& file)
	{
		for (auto& [path, entry] : shaders)
		{
			if (entry.graph && path != file && std::find(entry.dependencies.begin(), entry.dependencies.end(), file) != entry.dependencies.end())
			{
				BuildGraphShader(path, entry);
			}
		}
	}

	bool ReadText(const std::string& path, std::string& out)
	{
		std::ifstream stream(Full(path), std::ios::binary);
		if (!stream) return false;
		out.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		return true;
	}

	SurfaceShaderHandle GetShaderCached(const std::string& path)
	{
		auto it = shaders.find(path);
		if (it != shaders.end()) return it->second.handle;
		ShaderEntry& entry = shaders[path];   // misses too, so a bad path doesn't retry every frame
		std::string source;
		if (contentRoot.empty())
		{
		}
		else if (TypeOf(path) == "shadergraph")
		{
			entry.graph = true;
			BuildGraphShader(path, entry);
		}
		else if (TypeOf(path) == "shader" && ReadText(path, source))
		{
			entry.handle = renderer->CreateSurfaceShader(path, source);
			Watch(path);
		}
		else
		{
			ReportMissing("shader", path);
		}
		return entry.handle;
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
	for (auto& [path, texture] : a.linearTextures) if (texture.IsValid()) a.renderer->DestroyTexture(texture);
	for (auto& [path, shader] : a.shaders) if (shader.handle.IsValid()) a.renderer->DestroySurfaceShader(shader.handle);
	a.meshes.clear();
	a.materials.clear();
	a.textures.clear();
	a.linearTextures.clear();
	a.shaders.clear();
	a.graphs.clear();
	a.previewGraphs.clear();
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

TextureHandle AssetManager::GetTexture(const std::string& path, bool srgb)
{
	Impl& a = *mImpl;
	if (!a.renderer || path.empty()) return {};
	return a.GetTextureCached(path, srgb);
}

bool AssetManager::ReadShaderGraph(const std::string& path, sg::Graph& out)
{
	Impl& a = *mImpl;
	std::string text, error;
	if (a.contentRoot.empty() || !a.ReadText(path, text)) return false;
	return sg::GraphFromJson(text, out, error);
}

bool AssetManager::WriteShaderGraph(const std::string& path, const sg::Graph& graph)
{
	Impl& a = *mImpl;
	if (a.contentRoot.empty() || TypeOf(path) != "shadergraph") return false;
	std::error_code ec;
	fs::create_directories(a.Full(path).parent_path(), ec);
	std::ofstream stream(a.Full(path), std::ios::binary | std::ios::trunc);
	stream << sg::GraphToJson(graph) << "\n";
	if (!stream) return false;
	stream.close();

	// Apply now, and don't reload it again for this write.
	a.previewGraphs.erase(path);
	a.graphs[path] = std::make_unique<sg::Graph>(graph);
	a.Watch(path);
	auto entry = a.shaders.find(path);
	if (entry != a.shaders.end() && entry->second.graph && a.renderer) a.BuildGraphShader(path, entry->second);
	if (a.renderer) a.RebuildDependents(path);
	if (a.scanned && std::find(a.listing["shadergraph"].begin(), a.listing["shadergraph"].end(), path) == a.listing["shadergraph"].end()) a.Scan();
	return true;
}

sg::CompiledGraph AssetManager::PreviewShaderGraph(const std::string& path, const sg::Graph& graph)
{
	Impl& a = *mImpl;
	if (!a.renderer) return sg::CompileGraph(graph, a.Loader());
	a.previewGraphs[path] = graph;
	a.GetShaderCached(path);   // makes the entry (compiling the preview)
	sg::CompiledGraph compiled = a.BuildGraphShader(path, a.shaders[path]);
	a.RebuildDependents(path);
	return compiled;
}

void AssetManager::RevertShaderGraph(const std::string& path)
{
	Impl& a = *mImpl;
	if (a.previewGraphs.erase(path) == 0 || !a.renderer) return;
	auto entry = a.shaders.find(path);
	if (entry != a.shaders.end() && entry->second.graph) a.BuildGraphShader(path, entry->second);
	a.RebuildDependents(path);
}

const std::vector<sg::GraphParameter>* AssetManager::ShaderGraphParameters(const std::string& path)
{
	Impl& a = *mImpl;
	if (!a.renderer || TypeOf(path) != "shadergraph") return nullptr;
	a.GetShaderCached(path);
	auto entry = a.shaders.find(path);
	return entry != a.shaders.end() ? &entry->second.parameters : nullptr;
}

std::vector<sg::GraphError> AssetManager::ShaderGraphErrors(const std::string& path)
{
	Impl& a = *mImpl;
	auto entry = a.shaders.find(path);
	return entry != a.shaders.end() ? entry->second.errors : std::vector<sg::GraphError>{};
}

sg::GraphLoader AssetManager::GraphLoader() { return mImpl->Loader(); }

MaterialHandle AssetManager::ShaderPreviewMaterial(const std::string& shaderPath)
{
	Impl& a = *mImpl;
	if (!a.renderer || shaderPath.empty()) return {};
	// Kept with the other materials under a key no file has ("?"), so ReapplyMaterials
	// updates it when the shader recompiles.
	const std::string key = shaderPath + "?preview";
	auto it = a.materials.find(key);
	if (it != a.materials.end()) return it->second;
	MaterialFile file;
	file.shader = shaderPath;
	const MaterialHandle material = a.renderer->CreateMaterial(MaterialDesc{});
	a.materials[key] = material;
	a.ApplyMaterial(key, material, file);
	return material;
}

SurfaceShaderHandle AssetManager::GetSurfaceShader(const std::string& path)
{
	Impl& a = *mImpl;
	if (!a.renderer || path.empty()) return {};
	return a.GetShaderCached(path);
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
	if (extension == ".prefab") return "prefab";
	if (extension == ".hlsl") return "shader";
	if (extension == ".shadergraph") return "shadergraph";
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

bool AssetManager::ReadPrefab(const std::string& path, std::string& out)
{
	Impl& a = *mImpl;
	auto it = a.prefabs.find(path);
	if (it == a.prefabs.end())
	{
		std::ifstream stream(a.Full(path), std::ios::binary);
		if (!stream)
		{
			a.ReportMissing("prefab", path);
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		it = a.prefabs.emplace(path, std::move(text)).first;
		a.Watch(path);
	}
	out = it->second;
	return true;
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
	if (!material.shader.empty()) json["shader"] = material.shader;
	if (!material.params.empty()) json["params"] = material.params;
	if (!material.parameters.empty())
	{
		nlohmann::ordered_json parameters = nlohmann::ordered_json::object();
		for (const auto& [name, parameter] : material.parameters)
		{
			if (!parameter.texture.empty()) parameters[name] = parameter.texture;
			else parameters[name] = { parameter.value[0], parameter.value[1], parameter.value[2], parameter.value[3] };
		}
		json["parameters"] = parameters;
	}

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
		a.prefabs.erase(file);   // read again on the next spawn

		// Textures from this file (the file itself, or images inside a glTF), sRGB and linear.
		bool texturesReloaded = false;
		for (bool srgb : { true, false })
		{
			for (auto& [path, texture] : srgb ? a.textures : a.linearTextures)
			{
				if (SplitSubAsset(path).first != file) continue;
				if (texture.IsValid()) a.renderer->DestroyTexture(texture);
				texture = a.LoadTexture(path, srgb);
				texturesReloaded = true;
			}
		}
		// Surface shaders: recompiled in place (same handle). A failed compile keeps the
		// previous version running, and the error stays in Renderer::SurfaceShaderError
		// (and ShaderGraphErrors for graphs).
		auto graph = a.graphs.find(file);
		if (graph != a.graphs.end())
		{
			a.graphs.erase(graph);   // read again
			auto entry = a.shaders.find(file);
			if (entry != a.shaders.end() && entry->second.graph && !a.previewGraphs.count(file))
			{
				if (a.BuildGraphShader(file, entry->second).ok) CHERRY_LOG("Shader graph %s reloaded", file.c_str());
			}
			a.RebuildDependents(file);
		}
		auto shader = a.shaders.find(file);
		if (shader != a.shaders.end() && !shader->second.graph && shader->second.handle.IsValid())
		{
			std::string source;
			if (a.ReadText(file, source))
			{
				a.renderer->SetSurfaceShaderSource(shader->second.handle, source, {}, [file](bool ok)
				{
					if (ok) CHERRY_LOG("Surface shader %s compiled", file.c_str());
				});
			}
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
		// Materials: in place (same handle), from the file or from the glTF. A reloaded texture
		// has a new handle, so then every material is re-applied.
		for (auto& [path, material] : a.materials)
		{
			if (!material.IsValid() || SplitSubAsset(path).first != file) continue;
			MaterialFile data;
			std::string error;
			if (a.ReadMaterialFile(path, data, error)) a.ApplyMaterial(path, material, data);
		}
		if (texturesReloaded) a.ReapplyMaterials([](const MaterialFile&) { return true; });
	}
}
