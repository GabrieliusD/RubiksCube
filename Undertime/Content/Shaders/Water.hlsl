// Water.hlsl
// The river and the lake (ValleyScenery makes the material): translucent water that flows.
//   texture 0: the flow map over the valley (ValleyScenery::BuildFlowMap), one texel per metre:
//     rg = flow velocity (x, z) as 0.5 + v / (2 * max speed), b = depth / 2 m, a = foam below falls
//   params: 0, 1 = the map's world origin (x, z); 2, 3 = its size in metres;
//           4..6 = deep colour, 7..9 = shallow colour (linear); 10 = max speed (m/s)
// Ripples are carried along the flow (flow mapping: two layers half a cycle apart, each
// fading out before it jumps back, so the water is seen to run without ever resetting).
// Foam is blocky (0.25 m cells, like everything else) and streams off the falls.
#define CHERRY_SURFACE_TRANSLUCENT
#define CHERRY_SURFACE_DEPTH_WRITE   // only the nearest layer shows (no flicker where falls overlap)

// Height of a few crossing waves at p, and its slope (d/dx, d/dz). `footprint`: metres one
// pixel covers; waves too fine to show there fade out (they'd only shimmer as streaks).
float3 Ripples(float2 p, float time, float footprint)
{
	const float2 dirs[4] = { float2(0.8, 0.6), float2(-0.5, 0.86), float2(0.96, -0.28), float2(-0.2, -0.98) };
	const float lengths[4] = { 1.9, 1.3, 0.8, 0.55 };
	float3 h = 0.0;
	[unroll] for (int i = 0; i < 4; ++i)
	{
		const float k = 6.2831853 / lengths[i];
		const float a = 0.04 * lengths[i] * saturate(1.0 - footprint * 4.0 / lengths[i]);
		const float phase = dot(dirs[i], p) * k + time * sqrt(9.8 * k) * 0.35;
		h += float3(a * sin(phase), a * k * cos(phase) * dirs[i]);
	}
	return h;
}

float FoamNoise(float2 p)
{
	return CloudValueNoise(p) * 0.6 + CloudValueNoise(p * 2.1 + 7.3) * 0.4;
}

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s = DefaultSurfaceOutput(input);
	const float2 origin = ParamFloat2(input, 0);
	const float2 size = ParamFloat2(input, 2);
	const float3 deep = ParamFloat3(input, 4);
	const float3 shallow = ParamFloat3(input, 7);
	const float maxSpeed = ParamFloat(input, 10);

	const float2 p = input.positionW.xz;
	const float4 map = SampleParamTexture(input, 0, gLinearClamp, (p - origin) / size, float4(0.5, 0.5, 0.5, 0.0));
	const float2 flow = (map.xy * 2.0 - 1.0) * maxSpeed;
	const float speed = length(flow);
	const float depth = map.z * 2.0;
	const bool side = input.normalW.y < 0.5;   // a face where the water steps down
	const float drop = (1.0 - input.vertexColor.g) * 4.0;   // metres this face falls (TerrainMesh)
	const bool fall = side && drop > 0.45;     // ... a real fall (not one of the river's small steps)
	const float footprint = length(fwidth(p)) + 0.002 * distance(input.positionW, gView.eyePosW);

	// Ripples carried by the flow: two layers, half a cycle apart.
	const float cycle = 2.0;
	const float t = input.time / cycle;
	const float phaseA = frac(t), phaseB = frac(t + 0.5);
	const float blend = abs(1.0 - 2.0 * phaseA);   // 0 when A is mid-cycle, 1 when it jumps
	// Carried at most about 1 m/s: faster water would stretch them into streaks (rapids show foam instead).
	const float2 carry = flow * min(1.0, 0.9 / max(speed, 1e-3));
	const float3 rippleA = Ripples(p - carry * phaseA * cycle, input.time, footprint);
	const float3 rippleB = Ripples(p - carry * phaseB * cycle + 3.7, input.time, footprint);
	float2 slope = lerp(rippleA.yz, rippleB.yz, blend);
	// Patchy, not regular: some stretches choppier than others (drifting with the flow).
	const float patches = CloudValueNoise((p - flow * input.time * 0.5) * 0.35);
	slope *= (0.25 + 0.55 * saturate(speed / 1.2)) * (0.4 + 1.2 * patches);   // the lake is calmer
	float3 n = normalize(float3(-slope.x, 1.0, -slope.y));
	if (side)
		n = input.normalW;

	// Colour: shallow at the edges, deep in the middle.
	const float deepness = saturate(depth / 1.6);
	float3 colour = lerp(shallow, deep, deepness);
	float alpha = lerp(0.45, 0.88, deepness);

	// The sky, reflected more at grazing angles (Fresnel).
	const float3 V = input.viewDirW;
	const float facing = saturate(dot(n, V));
	const float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
	const float3 R = reflect(-V, n);
	const float3 sky = lerp(gFrame.skyHorizon, gFrame.skyZenith, pow(saturate(R.y), max(gFrame.skyGradientPower, 0.05)));
	alpha = lerp(alpha, 1.0, fresnel);

	// Foam: pouring down the falls, churning on the ledges around them (the mesh's churn,
	// red = 1 - churn) and trailing off downstream (the flow map), as soft streaks carried by
	// the current; a faint broken line where it laps the shore. Smooth, not on the block grid.
	float foam = 0.0;
	if (fall)
	{
		const float stream = FoamNoise(float2((input.positionW.x + input.positionW.z) * 2.0, input.positionW.y * 1.5 + input.time * 3.0));
		foam = saturate(0.6 + stream);
	}
	else
	{
		const float churn = saturate((1.0 - input.vertexColor.r) / 0.98);
		const float amount = saturate(max(churn, map.a * 0.85));
		// Streaks: stretched along the flow, drifting with it.
		const float2 along = speed > 0.05 ? flow / speed : float2(1.0, 0.0);
		const float2 q = float2(dot(p, along), dot(p, float2(-along.y, along.x)));
		const float drift = input.time * max(speed, 0.3);
		const float streaks = FoamNoise(float2((q.x - drift) * 0.9, q.y * 2.6)) * 0.65 + FoamNoise(float2((q.x - drift * 1.3) * 2.2, q.y * 4.0) + 5.1) * 0.35;
		foam = amount * smoothstep(1.0 - amount * 0.9, 1.15 - amount * 0.9, streaks + 0.25 * amount);
		const float shore = 1.0 - smoothstep(0.05, 0.2, depth);
		foam = max(foam, 0.35 * shore * smoothstep(0.55, 0.75, FoamNoise(p * 1.6 + input.time * 0.12)));
	}

	s.baseColor = lerp(colour, 0.92, foam);
	s.normalW = n;
	s.roughness = lerp(0.05, 0.85, foam);   // sharp sun glints on the ripples
	s.metallic = 0.0;
	s.specular = 0.5;
	s.emissive = sky * fresnel * 0.35 * (1.0 - foam);
	s.alpha = lerp(alpha, 0.97, foam);
	return s;
}
