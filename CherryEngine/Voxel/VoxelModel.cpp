#include "Voxel/VoxelModel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>

using namespace DirectX;

// =============================================================================
// .vox files
// =============================================================================
// Layout (MagicaVoxel, little-endian): "VOX " + int version, then a MAIN chunk whose
// children are the data chunks. Every chunk: 4-char id, int content bytes, int children
// bytes, content, children. Strings are int length + bytes; a DICT is int count + pairs.
namespace
{
	struct Reader
	{
		const uint8_t* data;
		size_t length;
		size_t pos = 0;
		bool ok = true;

		bool Has(size_t n) const { return ok && pos + n <= length; }
		int32_t Int()
		{
			if (!Has(4)) { ok = false; return 0; }
			int32_t v;
			std::memcpy(&v, data + pos, 4);
			pos += 4;
			return v;
		}
		uint8_t Byte()
		{
			if (!Has(1)) { ok = false; return 0; }
			return data[pos++];
		}
		std::string String()
		{
			const int32_t n = Int();
			if (n < 0 || !Has(static_cast<size_t>(n))) { ok = false; return {}; }
			std::string s(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(n));
			pos += n;
			return s;
		}
		std::map<std::string, std::string> Dict()
		{
			std::map<std::string, std::string> dict;
			const int32_t n = Int();
			for (int32_t i = 0; i < n && ok; ++i)
			{
				std::string key = String();
				dict[key] = String();
			}
			return dict;
		}
	};

	struct VoxModel
	{
		int sx = 0, sy = 0, sz = 0;   // MagicaVoxel axes (z up)
		std::vector<uint8_t> xyzi;    // 4 bytes per voxel
	};

	struct VoxNode
	{
		enum Kind { Transform, Group, Shape } kind = Transform;
		int translation[3] = { 0, 0, 0 };
		bool rotated = false;
		std::vector<int> children;   // node ids (Transform: one child; Group: many)
		std::vector<int> models;     // Shape: model ids
	};

	void WriteInt(std::vector<uint8_t>& out, int32_t v)
	{
		uint8_t b[4];
		std::memcpy(b, &v, 4);
		out.insert(out.end(), b, b + 4);
	}

	void WriteString(std::vector<uint8_t>& out, const std::string& s)
	{
		WriteInt(out, static_cast<int32_t>(s.size()));
		out.insert(out.end(), s.begin(), s.end());
	}

	void WriteChunk(std::vector<uint8_t>& out, const char id[4], const std::vector<uint8_t>& content)
	{
		out.insert(out.end(), id, id + 4);
		WriteInt(out, static_cast<int32_t>(content.size()));
		WriteInt(out, 0);
		out.insert(out.end(), content.begin(), content.end());
	}

	std::string FormatFloat(float v)
	{
		std::ostringstream s;
		s << v;
		return s.str();
	}
}

bool ParseVox(const uint8_t* data, size_t length, VoxelModel& out, std::string& error)
{
	error.clear();
	Reader r{ data, length };
	if (length < 8 || std::memcmp(data, "VOX ", 4) != 0)
	{
		error = "not a MagicaVoxel file";
		return false;
	}
	r.pos = 4;
	r.Int();   // version (150 and 200 share the chunks read here)
	if (!r.Has(12) || std::memcmp(data + r.pos, "MAIN", 4) != 0)
	{
		error = "no MAIN chunk";
		return false;
	}
	r.pos += 4;
	const int32_t mainContent = r.Int(), mainChildren = r.Int();
	r.pos += std::max(mainContent, 0);
	const size_t end = std::min(length, r.pos + static_cast<size_t>(std::max(mainChildren, 0)));

	std::vector<VoxModel> models;
	std::map<int, VoxNode> nodes;
	bool hasPalette = false;
	std::array<XMFLOAT4, 256> palette{};
	std::array<VoxelMaterial, 256> materials{};

	while (r.ok && r.pos + 12 <= end)
	{
		char id[5] = {};
		std::memcpy(id, data + r.pos, 4);
		r.pos += 4;
		const int32_t contentBytes = r.Int(), childBytes = r.Int();
		if (contentBytes < 0 || childBytes < 0 || r.pos + contentBytes > end)
		{
			error = std::string("damaged chunk ") + id;
			return false;
		}
		Reader c{ data, r.pos + contentBytes, r.pos };
		const std::string chunk = id;
		if (chunk == "SIZE")
		{
			VoxModel model;
			model.sx = c.Int(); model.sy = c.Int(); model.sz = c.Int();
			models.push_back(model);
		}
		else if (chunk == "XYZI" && !models.empty())
		{
			const int32_t count = c.Int();
			if (count > 0 && c.Has(static_cast<size_t>(count) * 4))
			{
				models.back().xyzi.assign(data + c.pos, data + c.pos + static_cast<size_t>(count) * 4);
			}
		}
		else if (chunk == "RGBA")
		{
			// Colour i of the chunk is palette index i + 1 (index 0 means empty).
			for (int i = 0; i < 255 && c.Has(4); ++i)
			{
				const float rgba[4] = { c.Byte() / 255.0f, c.Byte() / 255.0f, c.Byte() / 255.0f, c.Byte() / 255.0f };
				palette[i + 1] = { rgba[0], rgba[1], rgba[2], rgba[3] };
			}
			hasPalette = true;
		}
		else if (chunk == "MATL")
		{
			const int32_t index = c.Int();
			auto dict = c.Dict();
			if (c.ok && index > 0 && index < 256 && dict["_type"] == "_glass")
			{
				// MagicaVoxel's glass: "_alpha" (older files: "_trans") is how see-through it is.
				const char* key = dict.count("_alpha") ? "_alpha" : "_trans";
				const float alpha = dict.count(key) ? std::strtof(dict[key].c_str(), nullptr) : 0.5f;
				materials[index].glass = std::clamp(alpha, 0.05f, 1.0f);
			}
			if (c.ok && index > 0 && index < 256 && dict["_type"] == "_emit")
			{
				// MagicaVoxel's emission (0..1) and power ("flux", 0..4): strength = emit x (1 + flux).
				const float emit = dict.count("_emit") ? std::strtof(dict["_emit"].c_str(), nullptr) : 0.0f;
				const float flux = dict.count("_flux") ? std::strtof(dict["_flux"].c_str(), nullptr) : 0.0f;
				materials[index].emissive = std::max(0.0f, emit * (1.0f + flux));
			}
		}
		else if (chunk == "nTRN")
		{
			const int nodeId = c.Int();
			c.Dict();
			VoxNode node;
			node.kind = VoxNode::Transform;
			node.children.push_back(c.Int());
			c.Int();   // reserved
			c.Int();   // layer
			const int32_t frames = c.Int();
			for (int32_t f = 0; f < frames && c.ok; ++f)
			{
				auto frame = c.Dict();
				if (f > 0) continue;   // animation frames: the first one only
				if (frame.count("_t"))
				{
					std::istringstream t(frame["_t"]);
					t >> node.translation[0] >> node.translation[1] >> node.translation[2];
				}
				if (frame.count("_r") && std::atoi(frame["_r"].c_str()) != 4) node.rotated = true;   // 4 = identity
			}
			nodes[nodeId] = node;
		}
		else if (chunk == "nGRP")
		{
			const int nodeId = c.Int();
			c.Dict();
			VoxNode node;
			node.kind = VoxNode::Group;
			const int32_t count = c.Int();
			for (int32_t i = 0; i < count && c.ok; ++i) node.children.push_back(c.Int());
			nodes[nodeId] = node;
		}
		else if (chunk == "nSHP")
		{
			const int nodeId = c.Int();
			c.Dict();
			VoxNode node;
			node.kind = VoxNode::Shape;
			const int32_t count = c.Int();
			for (int32_t i = 0; i < count && c.ok; ++i)
			{
				node.models.push_back(c.Int());
				c.Dict();
			}
			nodes[nodeId] = node;
		}
		r.pos += contentBytes + childBytes;
	}

	if (models.empty())
	{
		error = "no voxel model in the file";
		return false;
	}

	// Place every model (MagicaVoxel axes). A model's centre sits at its node's translation.
	struct Placed { int model; int offset[3]; };
	std::vector<Placed> placed;
	bool rotated = false;
	if (nodes.count(0))
	{
		std::function<void(int, const int*, int)> visit = [&](int id, const int* t, int depth)
		{
			auto it = nodes.find(id);
			if (it == nodes.end() || depth > 64) return;
			const VoxNode& node = it->second;
			int here[3] = { t[0], t[1], t[2] };
			if (node.kind == VoxNode::Transform)
			{
				for (int k = 0; k < 3; ++k) here[k] += node.translation[k];
				rotated |= node.rotated;
			}
			for (int child : node.children) visit(child, here, depth + 1);
			for (int m : node.models)
			{
				if (m < 0 || m >= static_cast<int>(models.size())) continue;
				const VoxModel& model = models[m];
				placed.push_back({ m, { here[0] - model.sx / 2, here[1] - model.sy / 2, here[2] - model.sz / 2 } });
			}
		};
		const int origin[3] = { 0, 0, 0 };
		visit(0, origin, 0);
	}
	if (placed.empty()) placed.push_back({ 0, { 0, 0, 0 } });

	int lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX }, hi[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
	for (const Placed& p : placed)
	{
		const VoxModel& m = models[p.model];
		const int s[3] = { m.sx, m.sy, m.sz };
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = std::min(lo[k], p.offset[k]);
			hi[k] = std::max(hi[k], p.offset[k] + s[k]);
		}
	}
	const int vox[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
	if (vox[0] <= 0 || vox[1] <= 0 || vox[2] <= 0 || vox[0] > 2048 || vox[1] > 2048 || vox[2] > 2048)
	{
		error = "bad model size";
		return false;
	}

	VoxelModel model;
	model.Resize({ static_cast<uint32_t>(vox[0]), static_cast<uint32_t>(vox[2]), static_cast<uint32_t>(vox[1]) });   // z-up -> y-up
	for (const Placed& p : placed)
	{
		const std::vector<uint8_t>& xyzi = models[p.model].xyzi;
		for (size_t i = 0; i + 3 < xyzi.size(); i += 4)
		{
			const int x = xyzi[i] + p.offset[0] - lo[0], y = xyzi[i + 1] + p.offset[1] - lo[1], z = xyzi[i + 2] + p.offset[2] - lo[2];
			model.Set(x, z, y, xyzi[i + 3]);
		}
	}
	if (hasPalette)
	{
		model.palette = palette;
	}
	else
	{
		// MagicaVoxel always writes a palette; a file without one gets a grey ramp.
		for (int i = 1; i < 256; ++i) model.palette[i] = { i / 255.0f, i / 255.0f, i / 255.0f, 1.0f };
		error = "no palette in the file (grey used)";
	}
	model.materials = materials;
	if (rotated) error = "rotated models aren't supported yet (rotation ignored)";
	out = std::move(model);
	return true;
}

bool LoadVox(const std::filesystem::path& file, VoxelModel& out, std::string& error)
{
	std::ifstream stream(file, std::ios::binary);
	if (!stream)
	{
		error = "can't open the file";
		return false;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	return ParseVox(bytes.data(), bytes.size(), out, error);
}

std::vector<uint8_t> WriteVox(const VoxelModel& model)
{
	if (model.size.x == 0 || model.size.y == 0 || model.size.z == 0 || model.size.x > 256 || model.size.y > 256 || model.size.z > 256) return {};

	std::vector<uint8_t> children;
	// SIZE and XYZI in MagicaVoxel axes: its y is our z, its z our y.
	{
		std::vector<uint8_t> size;
		WriteInt(size, static_cast<int32_t>(model.size.x));
		WriteInt(size, static_cast<int32_t>(model.size.z));
		WriteInt(size, static_cast<int32_t>(model.size.y));
		WriteChunk(children, "SIZE", size);
	}
	{
		std::vector<uint8_t> xyzi;
		WriteInt(xyzi, static_cast<int32_t>(model.SolidCount()));
		for (uint32_t z = 0; z < model.size.z; ++z)
			for (uint32_t y = 0; y < model.size.y; ++y)
				for (uint32_t x = 0; x < model.size.x; ++x)
				{
					const uint8_t index = model.At(x, y, z);
					if (!index) continue;
					xyzi.insert(xyzi.end(), { static_cast<uint8_t>(x), static_cast<uint8_t>(z), static_cast<uint8_t>(y), index });
				}
		WriteChunk(children, "XYZI", xyzi);
	}
	{
		std::vector<uint8_t> rgba;
		auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
		for (int i = 0; i < 256; ++i)
		{
			const XMFLOAT4 c = i < 255 ? model.palette[i + 1] : XMFLOAT4(0, 0, 0, 1);
			rgba.insert(rgba.end(), { byte(c.x), byte(c.y), byte(c.z), byte(c.w) });
		}
		WriteChunk(children, "RGBA", rgba);
	}
	for (int index = 1; index < 256; ++index)
	{
		if (model.materials[index].glass > 0.0f)
		{
			// One MagicaVoxel material per colour: glass wins over glow.
			std::vector<uint8_t> matl;
			WriteInt(matl, index);
			WriteInt(matl, 2);
			WriteString(matl, "_type"); WriteString(matl, "_glass");
			WriteString(matl, "_alpha"); WriteString(matl, FormatFloat(model.materials[index].glass));
			WriteChunk(children, "MATL", matl);
			continue;
		}
		const float strength = model.materials[index].emissive;
		if (strength <= 0.0f) continue;
		// Back to MagicaVoxel's emission (0..1) and power: strength = emit x (1 + flux).
		const float emit = std::min(1.0f, strength), flux = std::max(0.0f, strength - 1.0f);
		std::vector<uint8_t> matl;
		WriteInt(matl, index);
		WriteInt(matl, 3);
		WriteString(matl, "_type"); WriteString(matl, "_emit");
		WriteString(matl, "_emit"); WriteString(matl, FormatFloat(emit));
		WriteString(matl, "_flux"); WriteString(matl, FormatFloat(flux));
		WriteChunk(children, "MATL", matl);
	}

	std::vector<uint8_t> out = { 'V', 'O', 'X', ' ' };
	WriteInt(out, 150);
	out.insert(out.end(), { 'M', 'A', 'I', 'N' });
	WriteInt(out, 0);
	WriteInt(out, static_cast<int32_t>(children.size()));
	out.insert(out.end(), children.begin(), children.end());
	return out;
}

bool SaveVox(const std::filesystem::path& file, const VoxelModel& model, std::string& error)
{
	const std::vector<uint8_t> bytes = WriteVox(model);
	if (bytes.empty())
	{
		error = "a .vox model must be 1 to 256 voxels along each axis";
		return false;
	}
	std::error_code ec;
	if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!stream)
	{
		error = "can't write the file";
		return false;
	}
	return true;
}

// =============================================================================
// Baking
// =============================================================================
namespace
{
	float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
}

XMFLOAT3 VoxelPivotOffset(const VoxelModel& model, const VoxelBakeOptions& options)
{
	if (options.pivot == VoxelPivot::Corner) return { 0, 0, 0 };
	return { -0.5f * model.size.x * options.voxelSize, 0.0f, -0.5f * model.size.z * options.voxelSize };
}

bool HasGlass(const VoxelModel& model)
{
	bool used[256] = {};
	for (uint8_t v : model.voxels) used[v] = true;
	for (int i = 1; i < 256; ++i)
		if (used[i] && model.materials[i].glass > 0.0f) return true;
	return false;
}

MeshData BakeVoxelMesh(const VoxelModel& model, const VoxelBakeOptions& options)
{
	MeshData mesh;
	const int dims[3] = { static_cast<int>(model.size.x), static_cast<int>(model.size.y), static_cast<int>(model.size.z) };
	if (model.voxels.empty()) return mesh;
	const XMFLOAT3 pivot = VoxelPivotOffset(model, options);
	const float s = options.voxelSize;

	// Corner shading: how much of the light reaches a corner with 0..3 of its neighbours open.
	const float kOpen[4] = { 0.0f, 0.5f, 0.78f, 1.0f };
	float shade[4];
	for (int i = 0; i < 4; ++i) shade[i] = 1.0f - std::clamp(options.aoStrength, 0.0f, 1.0f) * (1.0f - kOpen[i]);

	std::array<XMFLOAT3, 256> linear{};
	for (int i = 1; i < 256; ++i)
	{
		linear[i] = { SrgbToLinear(model.palette[i].x), SrgbToLinear(model.palette[i].y), SrgbToLinear(model.palette[i].z) };
	}
	// What's baked, and what hides a face: the opaque part ignores glass (its faces show
	// through it); glass hides behind anything.
	auto isGlass = [&](uint8_t index) { return index != 0 && model.materials[index].glass > 0.0f; };
	auto included = [&](uint8_t index)
	{
		if (!index) return false;
		if (options.part == VoxelPart::Opaque) return !isGlass(index);
		if (options.part == VoxelPart::Glass) return isGlass(index);
		return true;
	};
	auto solid = [&](const int* c)
	{
		const uint8_t index = model.At(c[0], c[1], c[2]);
		return options.part == VoxelPart::Opaque ? index != 0 && !isGlass(index) : index != 0;
	};

	std::vector<uint32_t> mask;
	for (int axis = 0; axis < 3; ++axis)
	{
		const int u = (axis + 1) % 3, v = (axis + 2) % 3;
		const int du = dims[u], dv = dims[v];
		mask.assign(static_cast<size_t>(du) * dv, 0);
		for (int sign = -1; sign <= 1; sign += 2)
		{
			for (int slice = 0; slice < dims[axis]; ++slice)
			{
				// Faces of this slice that look into empty space, keyed by colour and corner shading.
				bool any = false;
				for (int j = 0; j < dv; ++j)
				{
					for (int i = 0; i < du; ++i)
					{
						int c[3];
						c[axis] = slice; c[u] = i; c[v] = j;
						const uint8_t index = model.At(c[0], c[1], c[2]);
						uint32_t key = 0;
						int n[3] = { c[0], c[1], c[2] };
						n[axis] += sign;
						if (included(index) && !solid(n))
						{
							uint32_t ao = 0xFF;   // all corners open
							if (model.materials[index].emissive <= 0.0f)
							{
								ao = 0;
								const int corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
								for (int k = 0; k < 4; ++k)
								{
									int a[3] = { n[0], n[1], n[2] }, b[3] = { n[0], n[1], n[2] }, d[3] = { n[0], n[1], n[2] };
									a[u] += corners[k][0];
									b[v] += corners[k][1];
									d[u] += corners[k][0]; d[v] += corners[k][1];
									const int side1 = solid(a), side2 = solid(b), corner = solid(d);
									const uint32_t open = (side1 && side2) ? 0u : static_cast<uint32_t>(3 - side1 - side2 - corner);
									ao |= open << (2 * k);
								}
							}
							key = index | (ao << 8);
							any = true;
						}
						mask[static_cast<size_t>(j) * du + i] = key;
					}
				}
				if (!any) continue;

				const float plane = static_cast<float>(slice + (sign > 0 ? 1 : 0));
				for (int j = 0; j < dv; ++j)
				{
					for (int i = 0; i < du; )
					{
						const uint32_t key = mask[static_cast<size_t>(j) * du + i];
						if (!key) { ++i; continue; }
						int w = 1;
						while (i + w < du && mask[static_cast<size_t>(j) * du + i + w] == key) ++w;
						int h = 1;
						for (bool grow = true; grow && j + h < dv; )
						{
							for (int k = 0; k < w; ++k)
							{
								if (mask[static_cast<size_t>(j + h) * du + i + k] != key) { grow = false; break; }
							}
							if (grow) ++h;
						}
						for (int jj = 0; jj < h; ++jj)
							for (int ii = 0; ii < w; ++ii)
								mask[static_cast<size_t>(j + jj) * du + i + ii] = 0;

						const uint8_t index = static_cast<uint8_t>(key & 0xFF);
						const uint32_t ao = key >> 8;
						auto corner = [&](int cu, int cv)
						{
							float p[3];
							p[axis] = plane;
							p[u] = static_cast<float>(cu);
							p[v] = static_cast<float>(cv);
							return XMFLOAT3(p[0] * s + pivot.x, p[1] * s + pivot.y, p[2] * s + pivot.z);
						};
						XMFLOAT3 q[4] = { corner(i, j), corner(i + w, j), corner(i + w, j + h), corner(i, j + h) };
						int level[4] = { static_cast<int>(ao & 3), static_cast<int>((ao >> 2) & 3), static_cast<int>((ao >> 4) & 3), static_cast<int>((ao >> 6) & 3) };
						// (u x v) points along +axis; flip the winding for faces looking down -axis.
						if (sign < 0)
						{
							std::swap(q[1], q[3]);
							std::swap(level[1], level[3]);
						}

						XMFLOAT3 normal{ 0, 0, 0 };
						(&normal.x)[axis] = static_cast<float>(sign);
						const float emissive = model.materials[index].emissive;
						const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
						for (int k = 0; k < 4; ++k)
						{
							Vertex vertex;
							vertex.position = q[k];
							vertex.normal = normal;
							vertex.uv = { emissive, s };   // glow, block size (per-block shading)
							const float f = shade[level[k]];
							// Glass: alpha is how opaque it is (engine/VoxelGlass blends by it).
							const float alpha = isGlass(index) ? 1.0f - model.materials[index].glass : 1.0f;
							vertex.color = { linear[index].x * f, linear[index].y * f, linear[index].z * f, alpha };
							mesh.vertices.push_back(vertex);
						}
						// Split along the diagonal whose corners are more alike, so shading stays smooth.
						if (level[0] + level[2] < level[1] + level[3])
							mesh.indices.insert(mesh.indices.end(), { base + 1, base + 2, base + 3, base + 1, base + 3, base });
						else
							mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
						i += w;
					}
				}
			}
		}
	}
	return mesh;
}

std::vector<PhysicsBox> BakeVoxelBoxes(const VoxelModel& model, const VoxelBakeOptions& options)
{
	std::vector<PhysicsBox> boxes;
	if (model.voxels.empty()) return boxes;
	const int sx = static_cast<int>(model.size.x), sy = static_cast<int>(model.size.y), sz = static_cast<int>(model.size.z);
	std::vector<uint8_t> used(model.voxels.size(), 0);
	auto free = [&](int x, int y, int z) { return model.At(x, y, z) != 0 && !used[model.Index(x, y, z)]; };
	const XMFLOAT3 pivot = VoxelPivotOffset(model, options);
	const float s = options.voxelSize;

	for (int z = 0; z < sz; ++z)
		for (int y = 0; y < sy; ++y)
			for (int x = 0; x < sx; ++x)
			{
				if (!free(x, y, z)) continue;
				// Grow along x, then whole rows along y, then whole layers along z.
				int w = 1;
				while (x + w < sx && free(x + w, y, z)) ++w;
				int h = 1;
				for (bool grow = true; grow && y + h < sy; )
				{
					for (int i = 0; i < w && grow; ++i) grow = free(x + i, y + h, z);
					if (grow) ++h;
				}
				int d = 1;
				for (bool grow = true; grow && z + d < sz; )
				{
					for (int j = 0; j < h && grow; ++j)
						for (int i = 0; i < w && grow; ++i) grow = free(x + i, y + j, z + d);
					if (grow) ++d;
				}
				for (int k = 0; k < d; ++k)
					for (int j = 0; j < h; ++j)
						for (int i = 0; i < w; ++i) used[model.Index(x + i, y + j, z + k)] = 1;

				PhysicsBox box;
				box.halfExtents = { 0.5f * w * s, 0.5f * h * s, 0.5f * d * s };
				box.center = { (x + 0.5f * w) * s + pivot.x, (y + 0.5f * h) * s + pivot.y, (z + 0.5f * d) * s + pivot.z };
				boxes.push_back(box);
			}
	return boxes;
}
