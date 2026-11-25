#pragma once
// What the dice and the board look like (PROTOTYPE_EXPORT.md B2, B3), as plain data: the
// face atlas and grid textures (RGBA8, rows top to bottom), the meshes, and the instance
// matrices. No device calls, so the tests check the face mapping on the same data.
//
// Handedness (B13.6): the simulation keeps the reference's right-handed coordinates. The
// engine is left-handed, so everything drawn goes through one mirror, z -> -z (ToEngine):
// meshes, instance matrices, the camera, the sun and the mouse ray. A mirrored scene seen by
// a mirrored camera in a left-handed renderer is the reference's picture, pixel for pixel,
// and every formula of the simulation (tumble axis and direction included) stays as written.
#include "DiceMath.h"
#include <Renderer/RenderTypes.h>
#include <cstdint>
#include <vector>

namespace dice
{
	struct Image
	{
		int width = 0, height = 0;
		std::vector<uint8_t> rgba;
	};

	constexpr int kAtlasFace = 256;   // pixels per face cell (3 x 2 cells)

	Image MakeAtlas();
	Image MakeGridTexture();          // 64 x 64 felt cell with its grid line, repeated N times

	// The atlas cell (column, row from the top of the image) that box face k (+x, -x, +y,
	// -y, +z, -z) shows.
	void AtlasCell(int boxFace, int& column, int& rowFromTop);

	// The die: three.js BoxGeometry(dieSize) with each face's UVs moved into its atlas cell,
	// in the reference's coordinates (mirror it with ToEngine before drawing).
	MeshData MakeDieMesh(float dieSize);
	// The felt (N x N at y = 0, uv in cells) and the wood base ((N+0.8) x 0.5 x (N+0.8), top at y = -0.001).
	MeshData MakeFeltMesh(int N);
	MeshData MakeBaseMesh(int N);
	// three.js CircleGeometry(radius, segments, thetaStart).rotateX(-pi/2): a flat fan on y = 0
	// (the slot markers: square layout (0.62, 4, pi/4), hex (0.52, 6, pi/2), scatter (0.38, 18, 0)).
	MeshData MakeDiscMesh(double radius, int segments, double thetaStart);
	// Mirrors a mesh made in the reference's coordinates into the engine's (z -> -z).
	void ToEngine(MeshData& mesh);
	inline Vec3 ToEngine(const Vec3& v) { return { v.x, v.y, -v.z }; }

	// The instance matrix of a die (B9 "Rendering"): rotation q, then the squash in world
	// space (scaleXZ, scaleY: always vertical however the die is tilted, B13.7), then the
	// translation (x, y, z), all in the reference's coordinates; returned mirrored for the
	// engine, in its row-vector convention.
	void DieMatrix(const float q[4], float scaleXZ, float scaleY, float x, float y, float z, DirectX::XMFLOAT4X4& out);

	// sRGB colour (0xRRGGBB) to linear.
	DirectX::XMFLOAT4 SrgbToLinear(uint32_t rgb);
}
