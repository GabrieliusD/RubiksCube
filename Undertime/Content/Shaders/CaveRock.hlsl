// CaveRock.hlsl
// The cave's rock (Materials/CaveRock.mat, on the scene's CaveWorld): the palette colour with
// a mottled variation, plus
//   ore in the rock: an ore block is the surrounding rock with metal in it, in pixel clusters
//     (8 x 8 pixels per block face) from a noise in world space, so the metal runs on as seams
//     across blocks and round corners. Only the metal is glossy and catches the lantern.
//     Copper has green verdigris, silver dark tarnish and bright pixels, gold sparse flecks in
//     pale quartz.
//   Time crystal glowing cyan (bevelled the same way), the strange watch pink
//   cracks: pixel-art cracks on the same 8 x 8 grid, one from an edge when lightly cracked,
//     three meeting in the middle when badly cracked (like a block-breaking overlay)
//   damp rock deeper down: a little darker, with a sheen in the lantern light
// Material parameters (filled by UndertimeModule from Config.h, sRGB colours):
//   0..2 stone at the entrance, 3..5 stone at the far wall, 6..8 copper, 9..11 silver,
//   12..14 gold, 15..17 the quartz around gold, 18..20 verdigris,
//   21 ore richness, 22 gold richness, 23 silver tarnish, 24 the grid's depth (m)
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

float SegmentDistance(float2 p, float2 a, float2 b)
{
	const float2 ab = b - a;
	const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-5));
	return length(p - a - ab * t);
}

// A crack's way across one block face, on the face's 8 x 8 pixel grid. Each edge of the face
// is crossed by a crack (or not) at a point picked from the edge's place in the world, so the
// neighbouring block, which shares that edge, continues the crack from the same point. The
// crossings join at a point near the middle with a kink on the way: jagged 1-pixel lines.
//   face: integer coordinates of the face's block in the face's plane (u, v) and its depth w
//   chance: how likely each edge is crossed (more when badly cracked)
float CrackDistance(float2 pixelCentre, float3 face, float chance, float3 axisSeed)
{
	const float3 keys[4] = {
		float3(face.x, face.y, face.z) + axisSeed,            // the u = 0 edge
		float3(face.x + 1.0, face.y, face.z) + axisSeed,      // the u = 8 edge (the next block's u = 0)
		float3(face.x, face.y, face.z) + axisSeed + 50.5,     // the v = 0 edge
		float3(face.x, face.y + 1.0, face.z) + axisSeed + 50.5 };
	const float2 middle = float2(4.0, 4.0) + (float2(Hash13(face * 1.7 + axisSeed), Hash13(face * 2.3 + axisSeed)) - 0.5) * 3.0;
	float d = 99.0;
	int crossings = 0;
	[unroll] for (int e = 0; e < 4; ++e)
	{
		if (Hash13(keys[e] * 0.731) >= chance)
			continue;
		++crossings;
		const float t = 1.5 + Hash13(keys[e] * 1.913) * 5.0;
		const float2 start = e == 0 ? float2(0.0, t) : e == 1 ? float2(8.0, t) : e == 2 ? float2(t, 0.0) : float2(t, 8.0);
		const float2 kink = lerp(start, middle, 0.5) + (float2(Hash13(keys[e] * 3.1), Hash13(keys[e] * 4.7)) - 0.5) * 2.5;
		d = min(d, min(SegmentDistance(pixelCentre, start, kink), SegmentDistance(pixelCentre, kink, middle)));
	}
	return crossings > 0 ? d : 99.0;
}

// The pixel (0..7, 0..7) this point is on, on its block face.
float2 FacePixel(float2 uv)
{
	return clamp(floor(uv * 8.0), 0.0, 7.0);
}

// A pixel-art bevel: the top-left rim of a face lighter, the bottom-right darker.
float Bevel(float2 pixel)
{
	if (pixel.x < 0.5 || pixel.y > 6.5) return 1.18;
	if (pixel.x > 6.5 || pixel.y < 0.5) return 0.8;
	return 1.0;
}

// Value noise on a lattice (0..1), for the ore's pixel clusters.
float LatticeNoise(float3 p)
{
	const float3 i = floor(p), f = p - i;
	const float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(Hash13(i), Hash13(i + float3(1, 0, 0)), u.x), lerp(Hash13(i + float3(0, 1, 0)), Hash13(i + float3(1, 1, 0)), u.x), u.y),
		lerp(lerp(Hash13(i + float3(0, 0, 1)), Hash13(i + float3(1, 0, 1)), u.x), lerp(Hash13(i + float3(0, 1, 1)), Hash13(i + float3(1, 1, 1)), u.x), u.y), u.z);
}

float3 ParamSrgb(SurfaceInput input, uint index)
{
	return SrgbToLinear(ParamFloat3(input, index));
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

	// The block this pixel is on, and where on its face (an 8 x 8 pixel grid).
	const float3 cell = floor(input.positionL * 4.0 - input.normalW * 0.5);
	const float2 pixel = FacePixel(input.uv);
	const float blockHash = Hash13(cell);

	// Damp deeper down: darker, with a sheen.
	const float damp = smoothstep(40.0, 110.0, input.positionL.z);
	s.baseColor *= 1.0 - 0.15 * damp;
	s.roughness = lerp(0.85, 0.42, damp);
	s.specular = lerp(0.25, 0.45, damp);

	if (ore)
	{
		if ((type == kCopper || type == kSilver || type == kGold) && input.material.paramCount >= 25)
		{
			// The block's own shade (palette jitter, crack darkening): its colour over the ore's.
			const float3 oreBase = ParamSrgb(input, type == kCopper ? 6 : type == kSilver ? 9 : 12);
			const float shade = dot(colour, 1.0) / max(dot(oreBase, 1.0), 1e-4);
			// The rock around the metal: the stone of this depth (gold sits in quartz).
			const float depth01 = saturate(input.positionL.z / max(ParamFloat(input, 24), 1.0));
			float3 rock = SrgbToLinear(lerp(ParamFloat3(input, 0), ParamFloat3(input, 3), depth01));
			// Metal: pixel clusters from a noise sampled at the middle of each face pixel in world
			// space (32 pixels a metre), stretched along a gentle dip so they line up as seams.
			const float3 pixelW = (floor(input.positionL * 32.0 - input.normalW * 0.5) + 0.5) / 32.0;
			const float3 seamSpace = float3(pixelW.x * 2.2 + pixelW.y * 0.8, pixelW.y * 5.5, pixelW.z * 2.2 - pixelW.y * 0.6);
			const float field = LatticeNoise(seamSpace) * 0.65 + LatticeNoise(pixelW * 9.0 + 7.3) * 0.35;
			const float richness = (type == kGold ? ParamFloat(input, 22) : ParamFloat(input, 21)) * lerp(0.75, 1.2, blockHash);
			const float metal = field > 1.0 - richness ? 1.0 : 0.0;
			const float pixelHash = Hash13(pixelW * 32.0);
			float3 metalColour = oreBase * lerp(0.8, 1.25, pixelHash);
			float metallic = 0.55, roughness = 0.24;
			if (type == kCopper)
			{
				// Verdigris: green patches where the copper has weathered (matte).
				const float green = LatticeNoise(pixelW * 6.0 + 3.1) > 0.62 ? 1.0 : 0.0;
				metalColour = lerp(metalColour, ParamSrgb(input, 18) * lerp(0.85, 1.1, pixelHash), green);
				metallic = lerp(metallic, 0.0, green);
				roughness = lerp(roughness, 0.8, green);
			}
			else if (type == kSilver)
			{
				// Tarnish: dark patches, and a few bright scratched pixels.
				const float tarnish = LatticeNoise(pixelW * 5.0 + 9.7);
				metalColour *= lerp(1.0, ParamFloat(input, 23), smoothstep(0.45, 0.65, tarnish));
				metalColour *= pixelHash > 0.93 ? 1.6 : 1.0;
			}
			else
			{
				// Gold: in a matrix of pale quartz where the seams run, a little glint of its own.
				const float quartz = field > 1.0 - richness * 1.6 ? 1.0 : 0.0;
				rock = lerp(rock, ParamSrgb(input, 15) * lerp(0.85, 1.05, pixelHash), quartz);
				metalColour *= 1.15;
				s.emissive = metal * oreBase * 0.08 * step(0.85, pixelHash);
			}
			s.baseColor = lerp(rock * lerp(0.85, 1.1, LatticeNoise(pixelW * 4.0)), metalColour, metal) * shade;
			s.metallic = metal * metallic;
			s.roughness = lerp(0.8, roughness, metal);
			s.specular = lerp(0.3, 0.85, metal);
			// Each metal pixel tilted a little its own way, so they flash in turn as the lantern moves.
			const float3 tilt = float3(Hash13(pixelW * 32.0 + 1.7), Hash13(pixelW * 32.0 + 3.1), Hash13(pixelW * 32.0 + 5.9)) - 0.5;
			s.normalW = normalize(input.normalW + tilt * 0.5 * metal);
		}
		else if (type == kTimeCrystal)
		{
			const float pulse = 0.85 + 0.15 * sin(input.time * 1.7 + dot(cell, float3(0.7, 1.3, 0.9)));
			s.baseColor = colour * Bevel(pixel);
			s.emissive = float3(0.35, 1.6, 1.9) * pulse * lerp(0.7, 1.15, blockHash) * Bevel(pixel);
			s.roughness = 0.15;
			s.specular = 0.6;
		}
		else if (type == kStrangeWatch)
		{
			s.emissive = float3(1.6, 0.35, 1.2) * (0.7 + 0.3 * sin(input.time * 4.0));
		}
	}

	// Cracks: 1-pixel lines on the face's 8 x 8 grid, running on across neighbouring blocks.
	if (crack > 0)
	{
		const float3 n = abs(input.normalW);
		const float3 q = input.positionL * 4.0;   // in blocks
		// The face's plane coordinates (u, v) and its depth (w, the boundary it lies on).
		const float3 uvw = n.x > 0.5 ? q.yzx : (n.y > 0.5 ? q.xzy : q.xyz);
		const float3 face = float3(floor(uvw.xy), round(uvw.z));
		const float2 pixelCentre = floor(frac(uvw.xy) * 8.0) + 0.5;
		const float3 axisSeed = n.x > 0.5 ? float3(0.0, 0.0, 0.0) : (n.y > 0.5 ? float3(71.0, 0.0, 0.0) : float3(0.0, 137.0, 0.0));
		const float d = CrackDistance(pixelCentre, face, crack == 1 ? 0.45 : 0.85, axisSeed);
		const float onCrack = d < 0.55 ? 1.0 : 0.0;
		s.baseColor *= 1.0 - 0.7 * onCrack;
		s.emissive *= 1.0 - onCrack;
	}
	return s;
}
