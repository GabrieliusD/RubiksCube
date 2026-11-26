// Mesh.hlsl
// The built-in surface shader, used by materials without one of their own: base colour x
// vertex colour x albedo texture, with the material's roughness and metallic.
// With CHERRY_ERROR_SURFACE it is the stand-in for surface shaders that don't compile.
#include "Surface.hlsli"

SurfaceOutput EvaluateSurface(SurfaceInput input)
{
	SurfaceOutput s = DefaultSurfaceOutput(input);
#ifdef CHERRY_ERROR_SURFACE
	// Magenta / black checker, 0.25 m squares, glowing so it shows up in the dark.
	int3 cell = int3(floor(input.positionW * 4.0));
	// lerp, not ?: (DXC folds that into a vector select that SPIR-V 1.3 can't express).
	float check = lerp(0.15, 1.0, float((cell.x + cell.y + cell.z) & 1));
	s.baseColor = 0.0;
	s.metallic = 0.0;
	s.roughness = 1.0;
	s.emissive = float3(1.0, 0.0, 1.0) * check;
	s.alpha = 1.0;
#endif
	return s;
}

#include "MeshPass.hlsli"
