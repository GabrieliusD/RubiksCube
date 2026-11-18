#include "SceneComponents.h"
#include "CaveGenerator.h"
#include "Config.h"
#include "VoxelWorld.h"
#include <Core/Engine.h>
#include <Core/Log.h>
#include <imgui.h>
#include <chrono>
#include <memory>
#include <random>

void CavePreviewSystem::Generate(Engine& engine, uint32_t seed)
{
	Clear(engine);
	const auto start = std::chrono::steady_clock::now();
	auto world = std::make_unique<VoxelWorld>();   // big: keep it off the stack
	GenerateCaves(*world, seed);

	VoxelVolumeDesc desc;
	desc.size = { static_cast<uint32_t>(cfg::kVoxX), static_cast<uint32_t>(cfg::kVoxY), static_cast<uint32_t>(cfg::kVoxZ) };
	desc.voxels = world->PaletteVoxels();
	desc.palette = VoxelWorld::BuildPalette();
	mVolume = engine.GetRenderer().CreateVoxelVolume(desc);
	mSeed = seed;
	mGenerationMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	CHERRY_LOG("UNDERTIME: cave preview for seed %u (%.0f ms)", seed, mGenerationMs);
}

void CavePreviewSystem::Clear(Engine& engine)
{
	Scene& scene = engine.GetEditScene();
	if (scene.IsAlive(mEntity)) scene.DestroyEntity(mEntity);
	mEntity = INVALID_ENTITY;
	if (mVolume.IsValid()) engine.GetRenderer().DestroyVoxelVolume(mVolume);
	mVolume = {};
}

void CavePreviewSystem::OnEditUpdate(Engine& engine, float)
{
	if (!mVolume.IsValid()) return;
	// The preview entity is runtime-only: undo and scene reloads rebuild the scene without
	// it, so put it back whenever it's gone.
	Scene& scene = engine.GetEditScene();
	if (!scene.IsAlive(mEntity) || !scene.Has<VoxelVolumeRenderer>(mEntity))
	{
		mEntity = scene.CreateEntity();   // no EntityInfo: never saved
		scene.Add(mEntity, Transform{});
		VoxelVolumeRenderer renderer;
		renderer.volume = mVolume;
		renderer.voxelSize = cfg::kVoxelSize;
		renderer.colorJitter = cfg::kStoneJitter;
		renderer.aoStrength = cfg::kVoxelAoStrength;
		scene.Add(mEntity, renderer);
	}
}

void CavePreviewSystem::OnInspectorGui(Engine& engine, Entity entity)
{
	Scene& scene = engine.GetScene();
	if (engine.GetPlayState() != PlayState::Edit || !scene.IsRegistered<CaveWorld>() || !scene.Has<CaveWorld>(entity)) return;
	const uint32_t seed = scene.Get<CaveWorld>(entity).seed;

	ImGui::SeparatorText("Cave preview");
	if (ImGui::Button(mVolume.IsValid() ? "Regenerate preview" : "Generate preview"))
	{
		// Seed 0 means "random every play": preview a random one and say which.
		const uint32_t previewSeed = seed != 0 ? seed : std::random_device{}();
		Generate(engine, previewSeed);
	}
	if (mVolume.IsValid())
	{
		ImGui::SameLine();
		if (ImGui::Button("Clear preview")) Clear(engine);
		ImGui::TextDisabled("Showing seed %u (%.0f ms). Not saved.", mSeed, mGenerationMs);
		if (seed == 0)
		{
			if (ImGui::Button("Keep this seed"))
			{
				scene.Get<CaveWorld>(entity).seed = mSeed;
				engine.MarkSceneDirty();
			}
			ImGui::SetItemTooltip("Set the CaveWorld seed to %u, so every play uses this mountain", mSeed);
		}
		else if (seed != mSeed)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "The seed changed: regenerate to see it.");
		}
	}
}

void CavePreviewSystem::OnUnload(Engine& engine)
{
	Clear(engine);
}
