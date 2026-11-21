#include "SceneComponents.h"
#include "ValleyScenery.h"
#include "ValleyProps.h"
#include "Config.h"
#include <Core/Engine.h>
#include <imgui.h>
#include <algorithm>
#include <memory>

struct ValleyPreviewSystem::State
{
	Valley valley;
	ValleyScenery scenery;
	ValleyProps props;
};

ValleyPreviewSystem::ValleyPreviewSystem() = default;
ValleyPreviewSystem::~ValleyPreviewSystem() = default;

void ValleyPreviewSystem::OnEditUpdate(Engine& engine, float)
{
	Scene& scene = engine.GetEditScene();
	int previewEra = 0;
	if (scene.IsRegistered<ValleyWorld>())
	{
		scene.Each<ValleyWorld>([&](Entity, ValleyWorld& world) { if (previewEra == 0) previewEra = std::clamp(world.previewEra, era::kFirst, era::kLast); });
	}
	if (previewEra == 0)
	{
		// No ValleyWorld (any more): no preview.
		if (mState) mState->scenery.Destroy(engine, scene, nullptr);
		mState.reset();
		return;
	}
	if (!mState) mState = std::make_unique<State>();
	State& s = *mState;
	bool spawned = false;
	if (!s.scenery.Built())
	{
		s.valley.SetPads(s.props.Pads(engine, scene, s.valley));
		s.scenery.Build(engine, scene, s.valley, nullptr);
		spawned = true;
	}
	else
	{
		// Undo and scene reloads rebuild the edit scene without the preview: put it back.
		spawned = s.scenery.EnsureEntities(scene);
	}
	if (spawned)
	{
		scene.Add(s.scenery.Root(), ValleyPreview{});
		for (Entity entity : s.scenery.Entities()) scene.Add(entity, ValleyPreview{});
	}
	ValleyProps::SnapToGround(scene, s.valley);
	s.props.ApplyEra(engine, scene, previewEra, nullptr);
}

void ValleyPreviewSystem::OnPlayBegin(Engine& engine)
{
	// The play world is a copy of the edit scene, preview and all: drop the preview (the
	// game builds the valley itself).
	Scene& scene = engine.GetScene();
	if (!scene.IsRegistered<ValleyPreview>()) return;
	std::vector<Entity> preview;
	scene.Each<ValleyPreview>([&](Entity entity, ValleyPreview&) { preview.push_back(entity); });
	for (Entity entity : preview) scene.DestroyEntity(entity);   // (it skips ones already gone)
}

void ValleyPreviewSystem::OnInspectorGui(Engine& engine, Entity entity)
{
	Scene& scene = engine.GetScene();
	if (engine.GetPlayState() != PlayState::Edit || !scene.IsRegistered<ValleyWorld>() || !scene.Has<ValleyWorld>(entity)) return;
	ValleyWorld& world = scene.Get<ValleyWorld>(entity);

	ImGui::SeparatorText("Valley preview");
	int previewEra = std::clamp(world.previewEra, era::kFirst, era::kLast);
	if (ImGui::BeginCombo("Show era", era::Name(previewEra)))
	{
		for (int e = era::kFirst; e <= era::kLast; ++e)
		{
			if (ImGui::Selectable(era::Name(e), e == previewEra))
			{
				world.previewEra = e;
				engine.MarkSceneDirty();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Only ValleyProps that exist in this era are shown");
	if (mState && mState->scenery.Built())
	{
		if (ImGui::Button("Rebuild valley")) mState->scenery.Destroy(engine, engine.GetEditScene(), nullptr);
		ImGui::SameLine();
		ImGui::TextDisabled("Built in %.0f ms. Not saved.", mState->scenery.BuildMs());
	}
}

void ValleyPreviewSystem::OnUnload(Engine& engine)
{
	if (mState) mState->scenery.Destroy(engine, engine.GetEditScene(), nullptr);
	mState.reset();
}
