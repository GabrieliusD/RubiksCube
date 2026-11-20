// ShaderGraph.hlsli
// Helper functions for code generated from shader graphs (included after Surface.hlsli).
#ifndef CHERRY_SHADER_GRAPH_HLSLI
#define CHERRY_SHADER_GRAPH_HLSLI

// Hash of a lattice point to 0..1 (Dave Hoskins, "Hash without Sine").
float CherryGraphHash13(float3 p)
{
	p = frac(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return frac((p.x + p.y) * p.z);
}

// Smooth value noise in 0..1.
float CherryGraphNoise(float3 p)
{
	float3 i = floor(p);
	float3 f = frac(p);
	float3 u = f * f * (3.0 - 2.0 * f);
	float n000 = CherryGraphHash13(i);
	float n100 = CherryGraphHash13(i + float3(1, 0, 0));
	float n010 = CherryGraphHash13(i + float3(0, 1, 0));
	float n110 = CherryGraphHash13(i + float3(1, 1, 0));
	float n001 = CherryGraphHash13(i + float3(0, 0, 1));
	float n101 = CherryGraphHash13(i + float3(1, 0, 1));
	float n011 = CherryGraphHash13(i + float3(0, 1, 1));
	float n111 = CherryGraphHash13(i + float3(1, 1, 1));
	return lerp(lerp(lerp(n000, n100, u.x), lerp(n010, n110, u.x), u.y),
	            lerp(lerp(n001, n101, u.x), lerp(n011, n111, u.x), u.y), u.z);
}

// Value noise summed over octaves (each twice the frequency, half the amplitude), 0..1.
float CherryGraphFbm(float3 p, int octaves)
{
	float sum = 0.0;
	float amplitude = 0.5;
	float total = 0.0;
	for (int i = 0; i < octaves; ++i)
	{
		sum += amplitude * CherryGraphNoise(p);
		total += amplitude;
		p = p * 2.0 + 17.13;
		amplitude *= 0.5;
	}
	return sum / total;
}

// Tangent-space normal (-1..1, +Z out of the surface) to a world normal, with a tangent frame
// built from screen-space derivatives (Schueler, "Normal Mapping Without Precomputed
// Tangents"), so meshes need no tangents. Strength scales the bump.
float3 CherryPerturbNormal(SurfaceInput input, float3 tangentNormal, float strength)
{
	float3 N = input.normalW;
	float3 dp1 = ddx(input.positionW);
	float3 dp2 = ddy(input.positionW);
	float2 duv1 = ddx(input.uv);
	float2 duv2 = ddy(input.uv);
	float3 dp2perp = cross(dp2, N);
	float3 dp1perp = cross(N, dp1);
	float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float lengthSq = max(dot(T, T), dot(B, B));
	if (lengthSq < 1e-20)
		return N;   // no UVs: nothing to perturb along
	float invMax = rsqrt(lengthSq);
	tangentNormal.xy *= strength;
	// UV v grows downwards in D3D textures, so the bitangent points against +v.
	float3 n = tangentNormal.x * T * invMax - tangentNormal.y * B * invMax + tangentNormal.z * N;
	return normalize(n);
}

#endif
