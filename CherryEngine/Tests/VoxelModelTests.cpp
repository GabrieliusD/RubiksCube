// Tests for voxel models: .vox reading and writing, mesh baking and collision boxes.
#include <Voxel/VoxelModel.h>
#include <Renderer/Renderer.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace DirectX;

namespace
{
	int gVoxFailures = 0;

	void Check(bool condition, const std::string& what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what.c_str());
		if (!condition) ++gVoxFailures;
	}

	// ---- Building .vox bytes by hand (independent of WriteVox) ----------------------------
	void Int(std::vector<uint8_t>& out, int32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); out.insert(out.end(), b, b + 4); }
	void Str(std::vector<uint8_t>& out, const std::string& s) { Int(out, static_cast<int32_t>(s.size())); out.insert(out.end(), s.begin(), s.end()); }
	void Chunk(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& content)
	{
		out.insert(out.end(), id, id + 4);
		Int(out, static_cast<int32_t>(content.size()));
		Int(out, 0);
		out.insert(out.end(), content.begin(), content.end());
	}
	std::vector<uint8_t> File(const std::vector<uint8_t>& children)
	{
		std::vector<uint8_t> out = { 'V', 'O', 'X', ' ' };
		Int(out, 200);
		out.insert(out.end(), { 'M', 'A', 'I', 'N' });
		Int(out, 0);
		Int(out, static_cast<int32_t>(children.size()));
		out.insert(out.end(), children.begin(), children.end());
		return out;
	}
	void Size(std::vector<uint8_t>& out, int x, int y, int z) { std::vector<uint8_t> c; Int(c, x); Int(c, y); Int(c, z); Chunk(out, "SIZE", c); }
	void Xyzi(std::vector<uint8_t>& out, const std::vector<uint8_t>& voxels)
	{
		std::vector<uint8_t> c;
		Int(c, static_cast<int32_t>(voxels.size() / 4));
		c.insert(c.end(), voxels.begin(), voxels.end());
		Chunk(out, "XYZI", c);
	}

	VoxelModel Make(uint32_t x, uint32_t y, uint32_t z)
	{
		VoxelModel m;
		m.Resize({ x, y, z });
		for (int i = 1; i < 256; ++i) m.palette[i] = { (i % 7) / 6.0f, (i % 5) / 4.0f, (i % 3) / 2.0f, 1.0f };
		return m;
	}

	float Linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
	bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

	// Every triangle faces the way its vertex normal says (the engine's winding convention).
	bool WindingMatchesNormals(const MeshData& mesh)
	{
		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
		{
			const Vertex& a = mesh.vertices[mesh.indices[i]];
			const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
			const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
			const XMVECTOR pa = XMLoadFloat3(&a.position), pb = XMLoadFloat3(&b.position), pc = XMLoadFloat3(&c.position);
			const float d = XMVectorGetX(XMVector3Dot(XMVector3Cross(pb - pa, pc - pa), XMLoadFloat3(&a.normal)));
			if (!(d > 0.0f)) return false;
		}
		return true;
	}
}

int RunVoxelModelTests()
{
	// ---- Reading: axes and palette ----------------------------------------------------------
	{
		// MagicaVoxel model 2 x 3 x 4 (z up) with one voxel at (1, 2, 3), colour 5 = red.
		std::vector<uint8_t> children;
		Size(children, 2, 3, 4);
		Xyzi(children, { 1, 2, 3, 5 });
		std::vector<uint8_t> rgba(256 * 4, 0);
		rgba[4 * 4 + 0] = 255; rgba[4 * 4 + 3] = 255;   // chunk colour 4 = palette index 5
		Chunk(children, "RGBA", rgba);
		const std::vector<uint8_t> bytes = File(children);
		VoxelModel model;
		std::string error;
		const bool ok = ParseVox(bytes.data(), bytes.size(), model, error);
		Check(ok && error.empty(), "a hand-made .vox parses");
		Check(model.size.x == 2 && model.size.y == 4 && model.size.z == 3, "MagicaVoxel's z-up size becomes y-up (2x3x4 -> 2x4x3)");
		Check(model.At(1, 3, 2) == 5 && model.SolidCount() == 1, "voxel (1,2,3) lands at (1,3,2)");
		Check(model.palette[5].x == 1.0f && model.palette[5].y == 0.0f && model.palette[5].w == 1.0f, "RGBA colour i is palette index i + 1");
	}
	{
		VoxelModel model;
		std::string error;
		const uint8_t junk[] = { 'n', 'o', 'p', 'e', 0, 0, 0, 0 };
		Check(!ParseVox(junk, sizeof(junk), model, error) && !error.empty(), "a file that isn't .vox is rejected with a reason");
	}

	// ---- Reading: a file written by another program (Tests/Data/LanternCrate.vox) ---------------
	{
		VoxelModel model;
		std::string error;
		const bool ok = LoadVox(std::filesystem::path(CHERRY_TEST_DATA_DIR) / "LanternCrate.vox", model, error);
		Check(ok && model.size.x == 6 && model.size.y == 10 && model.size.z == 6 && model.SolidCount() == 197, "LanternCrate.vox (written by a script) loads: 6x10x6, 197 voxels");
		Check(model.At(2, 6, 2) == 5 && model.materials[5].emissive == 3.0f && model.materials[1].emissive == 0.0f, "its lantern glass glows (emit 1, power 2)");
		const MeshData mesh = BakeVoxelMesh(model);
		Check(!mesh.vertices.empty() && WindingMatchesNormals(mesh), "and bakes into a mesh");
	}

	// ---- Reading: several models placed by the scene graph -----------------------------------
	{
		std::vector<uint8_t> children;
		Size(children, 1, 1, 1); Xyzi(children, { 0, 0, 0, 1 });
		Size(children, 1, 1, 1); Xyzi(children, { 0, 0, 0, 2 });
		auto trn = [&](int id, int child, const char* t)
		{
			std::vector<uint8_t> c;
			Int(c, id); Int(c, 0); Int(c, child); Int(c, -1); Int(c, 0); Int(c, 1);
			if (t) { Int(c, 1); Str(c, "_t"); Str(c, t); } else Int(c, 0);
			Chunk(children, "nTRN", c);
		};
		trn(0, 1, nullptr);
		{ std::vector<uint8_t> c; Int(c, 1); Int(c, 0); Int(c, 2); Int(c, 2); Int(c, 4); Chunk(children, "nGRP", c); }
		trn(2, 3, "0 0 0");
		{ std::vector<uint8_t> c; Int(c, 3); Int(c, 0); Int(c, 1); Int(c, 0); Int(c, 0); Chunk(children, "nSHP", c); }
		trn(4, 5, "3 0 2");
		{ std::vector<uint8_t> c; Int(c, 5); Int(c, 0); Int(c, 1); Int(c, 1); Int(c, 0); Chunk(children, "nSHP", c); }
		const std::vector<uint8_t> bytes = File(children);
		VoxelModel model;
		std::string error;
		const bool ok = ParseVox(bytes.data(), bytes.size(), model, error);
		Check(ok && model.size.x == 4 && model.size.y == 3 && model.size.z == 1, "two models are combined at their scene translations");
		Check(model.At(0, 0, 0) == 1 && model.At(3, 2, 0) == 2 && model.SolidCount() == 2, "each model's voxels keep their colour and place");
	}

	// ---- Writing: round trip -----------------------------------------------------------------
	{
		VoxelModel model = Make(3, 2, 4);
		model.palette[9] = { 51 / 255.0f, 102 / 255.0f, 204 / 255.0f, 1.0f };
		model.Set(0, 0, 0, 9); model.Set(2, 1, 3, 9); model.Set(1, 0, 2, 7);
		model.materials[7].emissive = 2.5f;
		const std::vector<uint8_t> bytes = WriteVox(model);
		VoxelModel back;
		std::string error;
		const bool ok = ParseVox(bytes.data(), bytes.size(), back, error);
		Check(ok && error.empty(), "WriteVox output parses");
		Check(back.size.x == 3 && back.size.y == 2 && back.size.z == 4 && back.voxels == model.voxels, "round trip keeps size and voxels");
		Check(Near(back.palette[9].x, model.palette[9].x) && Near(back.palette[9].y, model.palette[9].y) && Near(back.palette[9].z, model.palette[9].z), "round trip keeps the palette");
		Check(Near(back.materials[7].emissive, 2.5f) && back.materials[9].emissive == 0.0f, "round trip keeps emissive strength (via MagicaVoxel's emit and power)");

		const std::filesystem::path file = std::filesystem::temp_directory_path() / "cherry_vox_test" / "model.vox";
		VoxelModel loaded;
		Check(SaveVox(file, model, error) && LoadVox(file, loaded, error) && loaded.voxels == model.voxels, "SaveVox / LoadVox through a file");
		std::error_code ec;
		std::filesystem::remove_all(file.parent_path(), ec);

		VoxelModel huge;
		huge.Resize({ 300, 1, 1 });
		Check(WriteVox(huge).empty() && !SaveVox(file, huge, error), "models over 256 voxels along an axis can't be written as .vox");
	}

	// ---- Mesh baking -------------------------------------------------------------------------
	{
		VoxelModel one = Make(1, 1, 1);
		one.Set(0, 0, 0, 3);
		const MeshData mesh = BakeVoxelMesh(one);
		Check(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "one voxel bakes into 6 quads");
		Check(WindingMatchesNormals(mesh), "baked triangles face along their normals");

		VoxelModel bar = Make(4, 1, 1);
		for (int x = 0; x < 4; ++x) bar.Set(x, 0, 0, 3);
		Check(BakeVoxelMesh(bar).vertices.size() == 24, "a bar of one colour merges into 6 quads");
		bar.Set(2, 0, 0, 4);
		Check(BakeVoxelMesh(bar).vertices.size() > 24, "a different colour splits the faces");

		VoxelModel block = Make(4, 2, 6);
		for (int z = 0; z < 6; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 4; ++x) block.Set(x, y, z, 1);
		const MeshData blockMesh = BakeVoxelMesh(block);   // 0.25 m voxels, pivot at the bottom centre
		XMFLOAT3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
		for (const Vertex& v : blockMesh.vertices)
		{
			lo = { std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z) };
			hi = { std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z) };
		}
		Check(Near(lo.x, -0.5f) && Near(lo.y, 0.0f) && Near(lo.z, -0.75f) && Near(hi.x, 0.5f) && Near(hi.y, 0.5f) && Near(hi.z, 0.75f),
			"bottom-centre pivot: a 4x2x6 model spans (-0.5,0,-0.75)..(0.5,0.5,0.75)");
		VoxelBakeOptions corner;
		corner.pivot = VoxelPivot::Corner;
		corner.voxelSize = 1.0f;
		float minX = 1e9f;
		for (const Vertex& v : BakeVoxelMesh(block, corner).vertices) minX = std::min(minX, v.position.x);
		Check(Near(minX, 0.0f), "corner pivot and voxel size options");

		// A post on a floor: the floor's top face is darker next to the post.
		VoxelModel floor = Make(3, 2, 3);
		for (int z = 0; z < 3; ++z) for (int x = 0; x < 3; ++x) floor.Set(x, 0, z, 2);
		floor.Set(1, 1, 1, 2);
		const float pure = Linear(floor.palette[2].x);
		float darkest = 1e9f, brightest = 0.0f;
		for (const Vertex& v : BakeVoxelMesh(floor).vertices)
		{
			if (v.normal.y < 0.5f || v.position.y > 0.3f) continue;   // the floor's top faces
			darkest = std::min(darkest, v.color.x);
			brightest = std::max(brightest, v.color.x);
		}
		Check(darkest < pure * 0.95f && Near(brightest, pure), "corner shading darkens the floor beside the post only");

		// Glowing colours: uv.x carries the strength and corners aren't shaded.
		VoxelModel lamp = Make(3, 2, 3);
		for (int z = 0; z < 3; ++z) for (int x = 0; x < 3; ++x) lamp.Set(x, 0, z, 6);
		lamp.materials[6].emissive = 3.0f;
		lamp.Set(1, 1, 1, 2);
		bool glowOk = true;
		int glowing = 0;
		for (const Vertex& v : BakeVoxelMesh(lamp).vertices)
		{
			if (v.uv.x == 0.0f) continue;   // the post
			++glowing;
			if (!Near(v.uv.x, 3.0f) || !Near(v.color.x, Linear(lamp.palette[6].x))) glowOk = false;
		}
		Check(glowOk && glowing > 0, "glowing voxels bake their strength into uv.x, unshaded");
	}

	// ---- The built-in material's surface shader (engine/VoxelModel) --------------------------------
	{
		std::string errors;
		const bool ok = CompileSurfaceShaderOffline("engine/VoxelModelShader", "#include \"VoxelModelSurface.hlsli\"\n", CHERRY_TEST_SHADER_DIR, errors);
		if (!ok) std::printf("%s\n", errors.c_str());
		Check(ok, "the voxel model surface shader compiles");
	}

	// ---- Glass ---------------------------------------------------------------------------------
	{
		// A 3-long wall with a glass pane in the middle.
		VoxelModel wall = Make(3, 1, 1);
		wall.palette[1] = { 0.5f, 0.4f, 0.3f, 1 };
		wall.palette[2] = { 0.7f, 0.8f, 0.9f, 1 };
		wall.materials[2].glass = 0.6f;
		wall.Set(0, 0, 0, 1);
		wall.Set(1, 0, 0, 2);
		wall.Set(2, 0, 0, 1);
		VoxelModel back;
		std::string error;
		const std::vector<uint8_t> bytes = WriteVox(wall);
		Check(ParseVox(bytes.data(), bytes.size(), back, error) && Near(back.materials[2].glass, 0.6f) && HasGlass(back) && !HasGlass(Make(2, 2, 2)),
			"glass colours survive a .vox round trip (MagicaVoxel's _glass)");
		VoxelBakeOptions opaque, glass;
		opaque.part = VoxelPart::Opaque;
		glass.part = VoxelPart::Glass;
		const MeshData solidPart = BakeVoxelMesh(wall, opaque);
		const MeshData glassPart = BakeVoxelMesh(wall, glass);
		// Opaque: two separate cubes (their faces towards the glass show): 12 quads. Glass: the
		// pane's faces that aren't against the wall: 4 quads, translucent.
		bool alphaOk = !glassPart.vertices.empty();
		for (const Vertex& v : glassPart.vertices) alphaOk &= Near(v.color.w, 0.4f);
		Check(solidPart.vertices.size() == 12 * 4 && glassPart.vertices.size() == 4 * 4 && alphaOk,
			"a model with glass bakes an opaque part (faces show through) and a see-through glass part");
		std::string errors;
		const bool ok = CompileSurfaceShaderOffline("engine/VoxelGlassShader", "#include \"VoxelGlassSurface.hlsli\"\n", CHERRY_TEST_SHADER_DIR, errors);
		if (!ok) std::printf("%s\n", errors.c_str());
		Check(ok, "the voxel glass surface shader compiles");
	}

	// ---- Collision boxes -----------------------------------------------------------------------
	{
		VoxelModel cube = Make(3, 3, 3);
		for (int z = 0; z < 3; ++z) for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) cube.Set(x, y, z, 1);
		const std::vector<PhysicsBox> one = BakeVoxelBoxes(cube);
		Check(one.size() == 1 && Near(one[0].halfExtents.x, 0.375f) && Near(one[0].center.y, 0.375f) && Near(one[0].center.x, 0.0f), "a solid cube is one box");

		VoxelModel shape = Make(4, 3, 2);   // an L: a floor and a wall, with a hole
		for (int z = 0; z < 2; ++z) for (int x = 0; x < 4; ++x) shape.Set(x, 0, z, 1);
		for (int z = 0; z < 2; ++z) for (int y = 1; y < 3; ++y) shape.Set(0, y, z, 1);
		shape.Set(3, 0, 1, 0);
		const std::vector<PhysicsBox> boxes = BakeVoxelBoxes(shape);
		float volume = 0.0f;
		for (const PhysicsBox& b : boxes) volume += 8.0f * b.halfExtents.x * b.halfExtents.y * b.halfExtents.z;
		const float expected = shape.SolidCount() * 0.25f * 0.25f * 0.25f;
		Check(boxes.size() <= 4 && Near(volume, expected, 1e-5f), "boxes cover exactly the solid voxels, in few pieces");
	}

	return gVoxFailures;
}
