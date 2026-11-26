// Tests for renderer pieces that need no GPU: point light selection and that the engine's
// own shaders compile with DXC.
#include <Renderer/Renderer.h>
#include <Particles/ParticleSystem.h>
#include <Scene/Scene.h>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int gRendererFailures = 0;

	void Check(bool condition, const std::string& what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what.c_str());
		if (!condition) ++gRendererFailures;
	}

	PointLight Light(float x, float range)
	{
		PointLight light;
		light.position = { x, 0.0f, 0.0f };
		light.range = range;
		return light;
	}
}

int RunRendererTests()
{
	// ---- Point light selection -----------------------------------------------------------------
	{
		std::vector<PointLight> few = { Light(50, 1), Light(1, 1) };
		const std::vector<uint32_t> all = SelectPointLights(few, { 0, 0, 0 }, 8);
		Check(all.size() == 2 && all[0] == 0 && all[1] == 1, "up to the limit, every light is used in order");

		// 10 lights in a row along x; light 9 is far but huge, so it reaches the camera.
		std::vector<PointLight> many;
		for (int i = 0; i < 9; ++i) many.push_back(Light(10.0f * (i + 1), 5.0f));
		many.push_back(Light(500.0f, 600.0f));
		const std::vector<uint32_t> chosen = SelectPointLights(many, { 0, 0, 0 }, 8);
		bool sorted = true;
		for (size_t i = 1; i < chosen.size(); ++i) sorted &= chosen[i - 1] < chosen[i];
		bool hasBig = false, hasFar = false;
		for (uint32_t i : chosen) { hasBig |= i == 9; hasFar |= i == 7 || i == 8; }
		Check(chosen.size() == 8 && sorted, "over the limit, 8 lights are picked, in their original order");
		Check(hasBig && !hasFar, "the picked lights are the ones closest to reaching the camera");
	}

	// ---- Particles (CPU side) ------------------------------------------------------------------
	{
		ParticleSystem system;
		ParticleEmitter dust;
		dust.lifetime = 1.0f;
		dust.lifetimeJitter = 0.0f;
		dust.velocity = { 0, 0, 0 };
		dust.velocityJitter = { 0, 0, 0 };
		dust.gravity = 10.0f;
		system.Burst(dust, { 0, 5, 0 }, 50);
		Check(system.Count() == 50, "a burst adds its particles");
		Scene scene;
		system.Update(scene, 0.05f);
		Check(system.Count() == 50, "particles live out their lifetime");
		for (int i = 0; i < 30; ++i) system.Update(scene, 0.05f);
		Check(system.Count() == 0, "... and then they're gone");

		uint32_t rng = 7;
		std::vector<Particle> falling = { ParticleSystem::Spawn(dust, { 0, 5, 0 }, rng) };
		ParticleSystem::Step(falling, 0.5f, rng);
		Check(falling.size() == 1 && falling[0].position.y < 5.0f && falling[0].velocity.y < 0.0f, "gravity pulls particles down");
	}

	// ---- Engine shaders compile, for D3D12 (DXIL) and Vulkan (SPIR-V) ----------------------------
	for (bool spirv : { false, true })
	{
		const std::string target = spirv ? " (SPIR-V)" : " (DXIL)";
		for (const char* file : { "SkyPass.hlsl", "Mesh.hlsl", "ShadowPass.hlsl", "VoxelRaymarch.hlsl", "VoxelShadow.hlsl", "Particles.hlsl",
			"DebugDraw.hlsl", "Blit.hlsl", "ImGui.hlsl" })
		{
			std::string errors;
			const bool ok = CompileShaderFileOffline(file, CHERRY_TEST_SHADER_DIR, errors, spirv);
			if (!ok) std::printf("%s\n", errors.c_str());
			Check(ok, std::string(file) + " compiles" + target);
		}
		for (const auto& [file, entry] : { std::pair<const char*, const char*>{ "Bloom.hlsl", "CSDownsample" }, { "Bloom.hlsl", "CSUpsample" },
			{ "Tonemap.hlsl", "CSMain" }, { "Fxaa.hlsl", "CSMain" }, { "Ssao.hlsl", "CSMain" }, { "Ssao.hlsl", "CSBlur" }, { "Edges.hlsl", "CSMain" },
			{ "LightShafts.hlsl", "CSMain" }, { "LightShafts.hlsl", "CSBlur" }, { "EyeAdaptation.hlsl", "CSReduce" }, { "EyeAdaptation.hlsl", "CSAdapt" } })
		{
			std::string errors;
			const bool ok = CompileComputeShaderOffline(file, entry, CHERRY_TEST_SHADER_DIR, errors, spirv);
			if (!ok) std::printf("%s\n", errors.c_str());
			Check(ok, std::string(file) + " " + entry + " compiles" + target);
		}
		std::string errors;
		const bool ok = CompileSurfaceShaderOffline("globals", "SurfaceOutput EvaluateSurface(SurfaceInput input)\n{\n"
			"\tSurfaceOutput s = DefaultSurfaceOutput(input);\n\ts.emissive = ShaderGlobal(2).rgb;\n\treturn s;\n}\n", CHERRY_TEST_SHADER_DIR, errors, false, spirv);
		if (!ok) std::printf("%s\n", errors.c_str());
		Check(ok, "surface shaders can read ShaderGlobal()" + target);
	}

	return gRendererFailures;
}
