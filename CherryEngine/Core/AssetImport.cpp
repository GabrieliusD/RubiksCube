#include "Core/AssetImport.h"
#include <cmath>
#include <fstream>
#include <iterator>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // files are read through std::filesystem (wide paths)
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include <stb_image.h>

using namespace DirectX;
namespace fs = std::filesystem;

namespace asset_import
{
	bool LoadImageMemory(const uint8_t* data, size_t size, Image& out, std::string& error)
	{
		int width = 0, height = 0, channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
		if (!pixels)
		{
			error = stbi_failure_reason() ? stbi_failure_reason() : "unknown image format";
			return false;
		}
		out.width = static_cast<uint32_t>(width);
		out.height = static_cast<uint32_t>(height);
		out.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
		stbi_image_free(pixels);
		return true;
	}

	bool LoadImageFile(const fs::path& file, Image& out, std::string& error)
	{
		std::ifstream stream(file, std::ios::binary);
		if (!stream)
		{
			error = "can't open " + file.string();
			return false;
		}
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		return LoadImageMemory(bytes.data(), bytes.size(), out, error);
	}

	namespace
	{
		void AppendPrimitive(const cgltf_primitive& primitive, const float* world, MeshData& mesh)
		{
			if (primitive.type != cgltf_primitive_type_triangles) return;
			const cgltf_accessor* positions = nullptr;
			const cgltf_accessor* normals = nullptr;
			const cgltf_accessor* uvs = nullptr;
			const cgltf_accessor* colors = nullptr;
			for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
			{
				const cgltf_attribute& attribute = primitive.attributes[i];
				if (attribute.type == cgltf_attribute_type_position) positions = attribute.data;
				else if (attribute.type == cgltf_attribute_type_normal) normals = attribute.data;
				else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) uvs = attribute.data;
				else if (attribute.type == cgltf_attribute_type_color && attribute.index == 0) colors = attribute.data;
			}
			if (!positions) return;

			// glTF matrices are column-major for column vectors; read row by row they are already
			// the row-vector matrix DirectXMath uses. Normals go through its inverse transpose.
			const XMMATRIX m = XMMATRIX(world);
			const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, m));
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			for (cgltf_size i = 0; i < positions->count; ++i)
			{
				Vertex vertex;
				if (!normals) vertex.normal = { 0, 0, 0 };   // summed from the faces below
				float p[3] = {};
				cgltf_accessor_read_float(positions, i, p, 3);
				XMFLOAT3 position;
				XMStoreFloat3(&position, XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1), m));
				// glTF is right-handed with +Z towards the viewer; the engine is left-handed.
				vertex.position = { position.x, position.y, -position.z };
				if (normals)
				{
					float n[3] = {};
					cgltf_accessor_read_float(normals, i, n, 3);
					XMFLOAT3 normal;
					XMStoreFloat3(&normal, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(n[0], n[1], n[2], 0), normalMatrix)));
					vertex.normal = { normal.x, normal.y, -normal.z };
				}
				if (uvs)
				{
					float uv[2] = {};
					cgltf_accessor_read_float(uvs, i, uv, 2);
					vertex.uv = { uv[0], uv[1] };
				}
				if (colors)
				{
					float c[4] = { 1, 1, 1, 1 };
					cgltf_accessor_read_float(colors, i, c, colors->type == cgltf_type_vec3 ? 3 : 4);
					vertex.color = { c[0], c[1], c[2], colors->type == cgltf_type_vec3 ? 1.0f : c[3] };
				}
				mesh.vertices.push_back(vertex);
			}

			const size_t firstIndex = mesh.indices.size();
			if (primitive.indices)
			{
				for (cgltf_size i = 0; i + 2 < primitive.indices->count; i += 3)
				{
					// Mirroring Z flips the winding: swap two corners.
					mesh.indices.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i)));
					mesh.indices.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 2)));
					mesh.indices.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 1)));
				}
			}
			else
			{
				for (uint32_t i = 0; i + 2 < positions->count; i += 3)
				{
					mesh.indices.push_back(base + i);
					mesh.indices.push_back(base + i + 2);
					mesh.indices.push_back(base + i + 1);
				}
			}

			if (!normals)
			{
				// Face normals summed per vertex.
				for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3)
				{
					Vertex& a = mesh.vertices[mesh.indices[i]];
					Vertex& b = mesh.vertices[mesh.indices[i + 1]];
					Vertex& c = mesh.vertices[mesh.indices[i + 2]];
					const XMVECTOR pa = XMLoadFloat3(&a.position), pb = XMLoadFloat3(&b.position), pc = XMLoadFloat3(&c.position);
					const XMVECTOR face = XMVector3Cross(pb - pa, pc - pa);
					for (Vertex* v : { &a, &b, &c })
					{
						XMStoreFloat3(&v->normal, XMLoadFloat3(&v->normal) + face);
					}
				}
				for (uint32_t i = base; i < mesh.vertices.size(); ++i)
				{
					XMStoreFloat3(&mesh.vertices[i].normal, XMVector3Normalize(XMLoadFloat3(&mesh.vertices[i].normal)));
				}
			}
		}

		void AppendNode(const cgltf_node& node, MeshData& mesh)
		{
			if (node.mesh)
			{
				float world[16];
				cgltf_node_transform_world(&node, world);
				for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
				{
					AppendPrimitive(node.mesh->primitives[i], world, mesh);
				}
			}
			for (cgltf_size i = 0; i < node.children_count; ++i)
			{
				AppendNode(*node.children[i], mesh);
			}
		}
	}

	bool LoadGltf(const fs::path& file, bool loadGeometry, GltfFile& out, std::string& error)
	{
		cgltf_options options{};
		cgltf_data* data = nullptr;
		const std::string path = file.u8string();
		if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
		{
			error = "not a readable glTF file";
			return false;
		}
		const bool needBuffers = loadGeometry || data->images_count > 0;
		if (needBuffers && cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
		{
			cgltf_free(data);
			error = "glTF buffers are missing";
			return false;
		}

		for (cgltf_size i = 0; i < data->images_count; ++i)
		{
			const cgltf_image& image = data->images[i];
			GltfImage result;
			if (image.buffer_view && image.buffer_view->buffer && image.buffer_view->buffer->data)
			{
				const uint8_t* bytes = static_cast<const uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
				result.encoded.assign(bytes, bytes + image.buffer_view->size);
			}
			else if (image.uri && std::string(image.uri).rfind("data:", 0) != 0)
			{
				std::string uri = image.uri;
				cgltf_decode_uri(uri.data());
				result.uri = uri.c_str();
			}
			out.images.push_back(std::move(result));
		}

		for (cgltf_size i = 0; i < data->materials_count; ++i)
		{
			const cgltf_material& material = data->materials[i];
			MaterialFile result;
			int imageIndex = -1;
			if (material.has_pbr_metallic_roughness)
			{
				const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
				result.baseColor = { pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3] };
				result.metallic = pbr.metallic_factor;
				result.roughness = pbr.roughness_factor;
				if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
				{
					imageIndex = static_cast<int>(pbr.base_color_texture.texture->image - data->images);
				}
			}
			out.materials.push_back(result);
			out.materialImages.push_back(imageIndex);
		}

		if (loadGeometry)
		{
			const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
			if (scene)
			{
				for (cgltf_size i = 0; i < scene->nodes_count; ++i) AppendNode(*scene->nodes[i], out.mesh);
			}
			else
			{
				const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
				for (cgltf_size m = 0; m < data->meshes_count; ++m)
				for (cgltf_size p = 0; p < data->meshes[m].primitives_count; ++p)
					AppendPrimitive(data->meshes[m].primitives[p], identity, out.mesh);
			}
			if (out.mesh.indices.empty())
			{
				cgltf_free(data);
				error = "the glTF file has no triangles";
				return false;
			}
		}
		cgltf_free(data);
		return true;
	}
}
