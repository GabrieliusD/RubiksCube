#include "DiceVisuals.h"
#include "DiceSim.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dice
{
	namespace
	{
		struct Rgb { double r, g, b; };
		Rgb Hex(uint32_t c) { return { ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0 }; }

		// Canvas-style anti-aliasing: coverage from a signed distance in pixels.
		double Coverage(double signedDistance) { return std::clamp(0.5 - signedDistance, 0.0, 1.0); }

		double RoundedRectDistance(double px, double py, double x0, double y0, double w, double h, double r)
		{
			const double cx = x0 + w / 2, cy = y0 + h / 2;
			const double qx = std::fabs(px - cx) - (w / 2 - r), qy = std::fabs(py - cy) - (h / 2 - r);
			const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
			return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
		}

		void Put(Image& image, int x, int y, const Rgb& c)
		{
			uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
			p[0] = static_cast<uint8_t>(std::lround(std::clamp(c.r, 0.0, 1.0) * 255));
			p[1] = static_cast<uint8_t>(std::lround(std::clamp(c.g, 0.0, 1.0) * 255));
			p[2] = static_cast<uint8_t>(std::lround(std::clamp(c.b, 0.0, 1.0) * 255));
			p[3] = 255;
		}

		Rgb Mix(const Rgb& a, const Rgb& b, double t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

		struct Pip { double x, y; };
		std::vector<Pip> PipsOf(int value)
		{
			const double a = .26, b = .74, m = .5;
			switch (value)
			{
			case 1: return { { m, m } };
			case 2: return { { a, a }, { b, b } };
			case 3: return { { a, a }, { m, m }, { b, b } };
			case 4: return { { a, a }, { b, a }, { a, b }, { b, b } };
			case 5: return { { a, a }, { b, a }, { a, b }, { b, b }, { m, m } };
			default: return { { a, .24 }, { a, m }, { a, .76 }, { b, .24 }, { b, m }, { b, .76 } };
			}
		}

		// three.js BoxGeometry's buildPlane with one segment: axes u, v, w (0 = x, 1 = y, 2 = z).
		void BuildPlane(MeshData& mesh, int u, int v, int w, double udir, double vdir, double width, double height, double depth)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			for (int iy = 0; iy < 2; ++iy)
			{
				const double y = iy * height - height / 2;
				for (int ix = 0; ix < 2; ++ix)
				{
					const double x = ix * width - width / 2;
					double p[3] = {}, n[3] = {};
					p[u] = x * udir; p[v] = y * vdir; p[w] = depth / 2;
					n[w] = depth > 0 ? 1 : -1;
					Vertex vertex;
					vertex.position = { static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]) };
					vertex.normal = { static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]) };
					vertex.uv = { static_cast<float>(ix), static_cast<float>(1 - iy) };   // three.js uv (v up)
					mesh.vertices.push_back(vertex);
				}
			}
			// a = 0, b = 2, c = 3, d = 1: faces (a, b, d), (b, c, d)
			mesh.indices.insert(mesh.indices.end(), { base + 0, base + 2, base + 1, base + 2, base + 3, base + 1 });
		}

		void AddBox(MeshData& mesh, double sx, double sy, double sz)
		{
			BuildPlane(mesh, 2, 1, 0, -1, -1, sz, sy, sx);    // +x
			BuildPlane(mesh, 2, 1, 0, 1, -1, sz, sy, -sx);    // -x
			BuildPlane(mesh, 0, 2, 1, 1, 1, sx, sz, sy);      // +y
			BuildPlane(mesh, 0, 2, 1, 1, -1, sx, sz, -sy);    // -y
			BuildPlane(mesh, 0, 1, 2, 1, -1, sx, sy, sz);     // +z
			BuildPlane(mesh, 0, 1, 2, -1, -1, sx, sy, -sz);   // -z
		}
	}

	void AtlasCell(int boxFace, int& column, int& rowFromTop)
	{
		// The reference draws box face k at canvas x = (k % 3) * S, y = (1 - floor(k / 3)) * S.
		column = boxFace % 3;
		rowFromTop = 1 - boxFace / 3;
	}

	Image MakeAtlas()
	{
		const int S = kAtlasFace;
		Image image;
		image.width = S * 3;
		image.height = S * 2;
		image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 255);
		const Rgb border = Hex(0xb9b2a4), faceColor = Hex(0xf5f2ea), pipColor = Hex(0x1c1e23), red = Hex(0xc8372d);
		for (int k = 0; k < 6; ++k)
		{
			const int value = kFaceOrder[k];
			int column = 0, row = 0;
			AtlasCell(k, column, row);
			const int ox = column * S, oy = row * S;
			const std::vector<Pip> pips = PipsOf(value);
			const double pr = value == 1 ? S * 0.13 : S * 0.085;
			const Rgb& pc = value == 1 ? red : pipColor;
			for (int y = 0; y < S; ++y)
				for (int x = 0; x < S; ++x)
				{
					const double px = x + 0.5, py = y + 0.5;
					Rgb c = Mix(border, faceColor, Coverage(RoundedRectDistance(px, py, 10, 10, S - 20, S - 20, 38)));
					for (const Pip& pip : pips)
						c = Mix(c, pc, Coverage(std::hypot(px - pip.x * S, py - pip.y * S) - pr));
					Put(image, ox + x, oy + y, c);
				}
		}
		return image;
	}

	Image MakeGridTexture()
	{
		Image image;
		image.width = image.height = 64;
		image.rgba.assign(64 * 64 * 4, 255);
		const Rgb felt = Hex(0x2d5b4e);
		const Rgb line = Mix(felt, { 0, 0, 0 }, 0.28);   // rgba(0,0,0,.28) over the felt
		for (int y = 0; y < 64; ++y)
			for (int x = 0; x < 64; ++x) Put(image, x, y, (y < 2 || x < 2) ? line : felt);
		return image;
	}

	MeshData MakeDieMesh(float dieSize)
	{
		MeshData mesh;
		AddBox(mesh, dieSize, dieSize, dieSize);
		// Each face's UVs into its atlas cell (as the reference), then to D3D's v (down).
		for (int k = 0; k < 6; ++k)
		{
			const double col = k % 3, row = k / 3;
			for (int v = 0; v < 4; ++v)
			{
				Vertex& vertex = mesh.vertices[k * 4 + v];
				const double u3 = (col + 0.01 + vertex.uv.x * 0.98) / 3, v3 = (row + 0.01 + vertex.uv.y * 0.98) / 2;
				vertex.uv = { static_cast<float>(u3), static_cast<float>(1.0 - v3) };
			}
		}
		return mesh;
	}

	MeshData MakeFeltMesh(int N)
	{
		MeshData mesh;
		const float h = N / 2.0f;
		// uv in cells: u = x + N/2, v = z + N/2 (D3D v), so the grid lines sit where the reference's do.
		const float xs[4] = { -h, h, -h, h }, zs[4] = { -h, -h, h, h };
		for (int k = 0; k < 4; ++k)
		{
			Vertex v;
			v.position = { xs[k], 0.0f, zs[k] };
			v.normal = { 0, 1, 0 };
			v.uv = { xs[k] + h, zs[k] + h };
			mesh.vertices.push_back(v);
		}
		mesh.indices = { 0, 2, 1, 2, 3, 1 };
		return mesh;
	}

	MeshData MakeBaseMesh(int N)
	{
		MeshData mesh;
		AddBox(mesh, N + 0.8, 0.5, N + 0.8);
		for (Vertex& v : mesh.vertices) v.position.y -= 0.251f;
		return mesh;
	}

	MeshData MakeDiscMesh(double radius, int segments, double thetaStart)
	{
		MeshData mesh;
		Vertex centre;
		centre.position = { 0, 0, 0 };
		centre.normal = { 0, 1, 0 };
		mesh.vertices.push_back(centre);
		for (int s = 0; s <= segments; ++s)
		{
			const double a = thetaStart + static_cast<double>(s) / segments * 2 * kPi;
			Vertex v;
			// (r cos a, r sin a, 0) turned by rotateX(-pi/2): (x, y, 0) -> (x, 0, -y).
			v.position = { static_cast<float>(radius * std::cos(a)), 0.0f, static_cast<float>(-radius * std::sin(a)) };
			v.normal = { 0, 1, 0 };
			mesh.vertices.push_back(v);
		}
		for (uint32_t s = 1; s <= static_cast<uint32_t>(segments); ++s) mesh.indices.insert(mesh.indices.end(), { s, s + 1, 0u });
		return mesh;
	}

	void ToEngine(MeshData& mesh)
	{
		for (Vertex& v : mesh.vertices)
		{
			v.position.z = -v.position.z;
			v.normal.z = -v.normal.z;
		}
		for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
	}

	void DieMatrix(const float q[4], float scaleXZ, float scaleY, float x, float y, float z, XMFLOAT4X4& out)
	{
		const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
		const double x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
		const double xx = qx * x2, xy = qx * y2, xz = qx * z2, yy = qy * y2, yz = qy * z2, zz = qz * z2;
		const double wx = qw * x2, wy = qw * y2, wz = qw * z2;
		// R (column vectors), rows scaled in world space: M = diag(sxz, sy, sxz) R.
		const double s[3] = { scaleXZ, scaleY, scaleXZ };
		const double R[3][3] = {
			{ 1 - (yy + zz), xy - wz, xz + wy },
			{ xy + wz, 1 - (xx + zz), yz - wx },
			{ xz - wy, yz + wx, 1 - (xx + yy) },
		};
		// Mirror: M_engine = S M S with S = diag(1, 1, -1); row-vector convention: out[j][i] = M_engine[i][j].
		const double S[3] = { 1, 1, -1 };
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) out.m[j][i] = static_cast<float>(S[i] * S[j] * s[i] * R[i][j]);
		out.m[0][3] = out.m[1][3] = out.m[2][3] = 0.0f;
		out.m[3][0] = x;
		out.m[3][1] = y;
		out.m[3][2] = -z;
		out.m[3][3] = 1.0f;
	}

	XMFLOAT4 SrgbToLinear(uint32_t rgb)
	{
		auto channel = [](uint32_t c)
		{
			const double v = c / 255.0;
			return static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
		};
		return { channel((rgb >> 16) & 255), channel((rgb >> 8) & 255), channel(rgb & 255), 1.0f };
	}
}
