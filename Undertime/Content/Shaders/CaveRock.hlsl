// CaveRock.hlsl
// The cave's rock (Materials/CaveRock.mat, on the scene's CaveWorld): the palette colour with
// a mottled variation, plus
//   ore that glints: metal facets that catch the lantern (each tiny facet tilted its own way)
//   Time crystal glowing cyan, the strange watch pink
//   cracks: dark lines across a damaged block's faces, more and wider as it weakens
//   damp rock deeper down: a little darker, with a sheen in the lantern light
// The palette layout comes from the game (VoxelWorld::PaletteLayout, ShaderGlobal(3)):
//   x = stone bands, y = first ore entry (one per block type), z = first cracked entry,
//   w = entries per cracked set (stone bands, then one per block type).
#include "ShaderGraph.hlsli"

static const uint kCopper = 3, kSilver = 4, kGold = 5, kTimeCrystal = 6, kStrangeWatch = 7;

float Hash13(float3 p)
{
	p = frac(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return frac((p.x + p.y) * p.z);
}

float2 Hash22(float2 p)
{
	float3 a = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
	a += dot(a, a.yzx + 33.33);
	return frac((a.xx + a.yz) * a.zy);
}

// Distance to the nearest edge between Voronoi cells on a face (0..1 coordinates): the cracks.
float CrackLines(float2 uv, float3 cellSeed, float cells)
{
	const float2 p = uv * cells;
	const float2 base = floor(p);
	float first = 9.0, second = 9.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			const float2 c = base + float2(x, y);
			const float2 site = c + Hash22(c + cellSeed.xy * 7.13 + cellSeed.z * 3.71);
			const float d = distance(p, site);
			if (d < first) { second = first; first = d; }
			else if (d < second) second = d;
		}
	return (second - first) / cells;
}

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	const float4 layout = ShaderGlobal(3);
	const float stoneBands = layout.x, oreFirst = layout.y, crackFirst = layout.z, crackSet = layout.w;
	const float id = round(input.blockId);

	// Which block, and how cracked.
	int crack = 0;
	float local = id;
	if (crackSet > 0.0 && id >= crackFirst)
	{
		crack = id >= crackFirst + crackSet ? 2 : 1;
		local = id - crackFirst - (crack - 1) * crackSet;
		local = local >= stoneBands ? oreFirst + (local - stoneBands) : 1.0;
	}
	const bool ore = oreFirst > 0.0 && local >= oreFirst;
	const uint type = ore ? (uint)round(local - oreFirst) : 1;

	SurfaceOutput s;
	const float3 colour = input.vertexColor.rgb;
	s.baseColor = colour * lerp(0.82, 1.15, CherryGraphFbm(input.positionL * 2.5, 3));
	s.metallic = 0.0;
	s.roughness = 0.85;
	s.specular = 0.25;
	s.normalW = input.normalW;
	s.emissive = 0.0;
	s.alpha = 1.0;
	s.opacityMask = 1.0;

	// The block this pixel is on, and a tiny facet inside it.
	const float3 cell = floor(input.positionL * 4.0 - input.normalW * 0.5);
	const float3 facet = floor(input.positionL * 12.0);   // three per block

	// Damp deeper down: darker, with a sheen.
	const float damp = smoothstep(40.0, 110.0, input.positionL.z);
	s.baseColor *= 1.0 - 0.15 * damp;
	s.roughness = lerp(0.85, 0.42, damp);
	s.specular = lerp(0.25, 0.45, damp);

	if (ore)
	{
		if (type == kCopper || type == kSilver || type == kGold)
		{
			// Metal flecks: tilted facets that flash in the lantern light as you move.
			// (half metal: fully metallic flecks only mirror the dark cave and read as black)
			const float fleck = step(0.7, Hash13(facet));
			const float3 tilt = float3(Hash13(facet + 1.7), Hash13(facet + 3.1), Hash13(facet + 5.9)) - 0.5;
			s.normalW = normalize(input.normalW + tilt * 0.9 * fleck);
			s.metallic = 0.45 * fleck;
			s.roughness = lerp(0.7, 0.22, fleck);
			s.specular = lerp(0.25, 0.9, fleck);
			s.baseColor = lerp(s.baseColor, colour * 1.35, fleck);
			// ... and a faint twinkle of their own, so they read in the dark too.
			const float twinkle = step(0.97, Hash13(facet + 9.3)) * (0.5 + 0.5 * sin(input.time * 3.0 + Hash13(facet) * 40.0));
			s.emissive = colour * twinkle * 0.6;
		}
		else if (type == kTimeCrystal)
		{
			const float pulse = 0.85 + 0.15 * sin(input.time * 1.7 + dot(cell, float3(0.7, 1.3, 0.9)));
			s.emissive = float3(0.35, 1.6, 1.9) * pulse * lerp(0.6, 1.2, Hash13(facet));
			s.roughness = 0.15;
			s.specular = 0.6;
		}
		else if (type == kStrangeWatch)
		{
			s.emissive = float3(1.6, 0.35, 1.2) * (0.7 + 0.3 * sin(input.time * 4.0));
		}
	}

	// Cracks across a damaged block's faces.
	if (crack > 0)
	{
		const float width = crack == 1 ? 0.025 : 0.05;
		const float lines = CrackLines(input.uv, cell, crack == 1 ? 2.0 : 3.0);
		const float onCrack = 1.0 - smoothstep(width * 0.5, width, lines);
		s.baseColor *= 1.0 - 0.75 * onCrack;
		s.emissive *= 1.0 - onCrack;
	}
	return s;
}
