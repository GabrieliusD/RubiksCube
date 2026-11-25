#include "ModelKit.h"
#include <algorithm>

using namespace DirectX;

namespace kit
{
	uint8_t Builder::Colour(uint32_t srgb, float glow, float glass)
	{
		const XMFLOAT4 c{ ((srgb >> 16) & 0xff) / 255.0f, ((srgb >> 8) & 0xff) / 255.0f, (srgb & 0xff) / 255.0f, 1.0f };
		for (int i = 1; i <= mUsed; ++i)
		{
			const XMFLOAT4& p = model.palette[i];
			if (p.x == c.x && p.y == c.y && p.z == c.z && model.materials[i].emissive == glow && model.materials[i].glass == glass) return static_cast<uint8_t>(i);
		}
		if (mUsed >= 255) return 255;
		++mUsed;
		model.palette[mUsed] = c;
		model.materials[mUsed].emissive = glow;
		model.materials[mUsed].glass = glass;
		return static_cast<uint8_t>(mUsed);
	}

	void Builder::Box(int x0, int y0, int z0, int x1, int y1, int z1, uint8_t colour)
	{
		x0 = std::max(x0, 0); y0 = std::max(y0, 0); z0 = std::max(z0, 0);
		x1 = std::min(x1, static_cast<int>(model.size.x)); y1 = std::min(y1, static_cast<int>(model.size.y)); z1 = std::min(z1, static_cast<int>(model.size.z));
		for (int z = z0; z < z1; ++z)
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x) model.Set(x, y, z, colour);
	}

	void Builder::Speckle(int x0, int y0, int z0, int x1, int y1, int z1, const std::vector<uint8_t>& colours, uint32_t salt)
	{
		if (colours.empty()) return;
		x0 = std::max(x0, 0); y0 = std::max(y0, 0); z0 = std::max(z0, 0);
		x1 = std::min(x1, static_cast<int>(model.size.x)); y1 = std::min(y1, static_cast<int>(model.size.y)); z1 = std::min(z1, static_cast<int>(model.size.z));
		for (int z = z0; z < z1; ++z)
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
				{
					uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u ^ salt * 2654435761u;
					h ^= h >> 13;
					h *= 0x5bd1e995u;
					h ^= h >> 15;
					model.Set(x, y, z, colours[h % colours.size()]);
				}
	}

	MeshHandle Bake(Engine& engine, const VoxelModel& model, VoxelPivot pivot, float voxelSize)
	{
		VoxelBakeOptions options;
		options.pivot = pivot;
		options.voxelSize = voxelSize;
		return engine.GetRenderer().CreateMesh(BakeVoxelMesh(model, options));
	}

	Entity Spawn(Engine& engine, MeshHandle mesh, MaterialHandle material, const XMFLOAT3& position, float yaw)
	{
		Scene& scene = engine.GetScene();
		const Entity e = scene.CreateEntity();
		Transform t;
		t.position = position;
		XMStoreFloat4(&t.rotation, XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
		scene.Add(e, t);
		scene.Add(e, MeshRenderer{ mesh, material });
		return e;
	}
}
