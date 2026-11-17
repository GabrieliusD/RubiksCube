#pragma once
#include <DirectXMath.h>
#include <cmath>
#include <cstdint>

// sRGB hex colours (as in Config.h) to the linear colours the renderer shades with.
namespace color
{
	inline float SrgbToLinear(float c)
	{
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	}

	inline DirectX::XMFLOAT3 Srgb(uint32_t hex)
	{
		return { ((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f };
	}

	inline DirectX::XMFLOAT3 Linear(uint32_t hex)
	{
		const DirectX::XMFLOAT3 c = Srgb(hex);
		return { SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z) };
	}

	inline DirectX::XMFLOAT3 ToLinear(const DirectX::XMFLOAT3& srgb)
	{
		return { SrgbToLinear(srgb.x), SrgbToLinear(srgb.y), SrgbToLinear(srgb.z) };
	}

	inline DirectX::XMFLOAT3 Lerp(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t)
	{
		return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
	}

	inline DirectX::XMFLOAT3 Scale(const DirectX::XMFLOAT3& c, float s)
	{
		return { c.x * s, c.y * s, c.z * s };
	}

	inline DirectX::XMFLOAT4 WithAlpha(const DirectX::XMFLOAT3& c, float a = 1.0f)
	{
		return { c.x, c.y, c.z, a };
	}

	// Small, stable integer hash -> [0, 1).
	inline float Hash01(int x, int y, int z, uint32_t salt = 0)
	{
		uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u ^ salt * 2654435761u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return (h & 0xFFFFFF) / static_cast<float>(0x1000000);
	}
}
