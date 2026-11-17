#include "Editor/EditorLayer.h"
#include "Core/Engine.h"
#include "Game/GameModule.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace
{
	const char* kViewportWindow = "Viewport";
	const char* kStatsWindow = "Stats";
	const char* kGameWindow = "Game";

	// Procedural 64^3 test volume: a hollow, banded sphere with a few cut-outs,
	// so the DDA, normals and depth intersection with meshes are easy to inspect.
	VoxelVolumeDesc MakeTestVolume()
	{
		constexpr uint32_t N = 64;
		VoxelVolumeDesc desc;
		desc.size = { N, N, N };
		desc.voxels.resize(N * N * N, 0);
		desc.palette =
		{
			{ 0, 0, 0, 0 },
			{ 0.85f, 0.25f, 0.20f, 1 },
			{ 0.95f, 0.75f, 0.20f, 1 },
			{ 0.25f, 0.70f, 0.35f, 1 },
			{ 0.20f, 0.45f, 0.85f, 1 },
			{ 0.90f, 0.90f, 0.92f, 1 },
		};
		const float c = (N - 1) * 0.5f;
		for (uint32_t z = 0; z < N; ++z)
		for (uint32_t y = 0; y < N; ++y)
		for (uint32_t x = 0; x < N; ++x)
		{
			const float dx = x - c, dy = y - c, dz = z - c;
			const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
			uint8_t value = 0;
			if (r < 30.0f && r > 24.0f)
			{
				value = static_cast<uint8_t>(1 + (y / 8) % 4);          // horizontal bands
				if (std::fabs(dx) < 7.0f && dz < 0.0f) value = 0;        // window into the shell
			}
			else if (r < 9.0f)
			{
				value = 5;                                               // inner core
			}
			desc.voxels[x + y * N + z * N * N] = value;
		}
		return desc;
	}
}

void EditorLayer::Draw(Engine& engine, IGameModule& game, bool editorMode, bool gameOverlayWindow, float frameMs)
{
	mFrameMsSmoothed = mFrameMsSmoothed * 0.95f + frameMs * 0.05f;

	// Engine debug hotkeys (work in both modes).
	if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
	{
		SetVoxelTest(engine, !mVoxelTest);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F3, false))
	{
		engine.GetRenderer().Settings().drawBounds ^= true;
	}

	if (!editorMode)
	{
		viewportHovered = viewportFocused = false;
		if (!gameOverlayWindow)
		{
			game.OnImGui(engine);   // the game draws its own HUD
			return;
		}
		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.85f);
		// A separate window from the editor's docked "Game" panel, so switching modes
		// doesn't undock that panel.
		ImGui::Begin("Game##Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
		game.OnImGui(engine);
		ImGui::TextDisabled("F1: editor  F2: voxel test  F3: bounds");
		ImGui::End();
		return;
	}

	DrawDockspace();
	DrawViewport(engine);
	DrawStats(engine, frameMs);

	ImGui::Begin(kGameWindow);
	game.OnImGui(engine);
	ImGui::End();
}

void EditorLayer::DrawDockspace()
{
	const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

	// First run (no imgui.ini layout yet): viewport left, stats + game panels right.
	if (!mLayoutBuilt)
	{
		mLayoutBuilt = true;
		ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
		if (!node || node->IsLeafNode() && node->Windows.Size == 0)
		{
			ImGui::DockBuilderRemoveNode(dockspace);
			ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
			ImGuiID center = 0;
			ImGuiID right = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.26f, nullptr, &center);
			// The viewport is the central node, so it absorbs window resizes and the side panels keep their width.
			if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(center))
			{
				centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
			}
			ImGuiID rightBottom = 0;
			ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.6f, nullptr, &rightBottom);
			ImGui::DockBuilderDockWindow(kViewportWindow, center);
			ImGui::DockBuilderDockWindow(kStatsWindow, rightTop);
			ImGui::DockBuilderDockWindow(kGameWindow, rightBottom);
			ImGui::DockBuilderFinish(dockspace);
		}
	}
}

void EditorLayer::DrawViewport(Engine& engine)
{
	Renderer& renderer = engine.GetRenderer();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin(kViewportWindow, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	const ImVec2 available = ImGui::GetContentRegionAvail();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	viewportSize = { std::max(available.x, 1.0f), std::max(available.y, 1.0f) };
	viewportPosition = { origin.x, origin.y };

	// Show the texture at its real size; while the panel is being resized the
	// texture catches up next frame instead of being stretched.
	const ImVec2 imageSize(static_cast<float>(renderer.ViewWidth()), static_cast<float>(renderer.ViewHeight()));
	ImGui::Image(static_cast<ImTextureID>(renderer.ViewTextureId()), imageSize);

	// InvisibleButton-like hover test over the whole panel content.
	viewportHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + available.x, origin.y + available.y));
	viewportFocused = ImGui::IsWindowFocused();
	if (viewportHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
	{
		ImGui::SetWindowFocus();
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorLayer::DrawStats(Engine& engine, float frameMs)
{
	Renderer& renderer = engine.GetRenderer();
	const RenderStats& stats = renderer.Stats();
	RenderSettings& settings = renderer.Settings();

	ImGui::Begin(kStatsWindow);
	ImGui::Text("%s", renderer.AdapterName().c_str());
	ImGui::Text("%.1f fps  (%.2f ms)", 1000.0f / std::max(mFrameMsSmoothed, 0.001f), mFrameMsSmoothed);
	ImGui::Text("View: %u x %u", stats.viewWidth, stats.viewHeight);
	ImGui::Text("GPU: %.2f ms", stats.gpuFrameMs);
	ImGui::TextDisabled("  mesh %.2f  voxel %.2f  tonemap %.2f", stats.gpuMeshMs, stats.gpuVoxelMs, stats.gpuTonemapMs);
	ImGui::TextDisabled("  debug %.2f  ui %.2f", stats.gpuDebugMs, stats.gpuUiMs);
	ImGui::Separator();
	ImGui::Text("Draw calls:       %u", stats.drawCalls);
	ImGui::Text("Instances:        %u  (culled %u)", stats.instancesSubmitted, stats.instancesCulled);
	ImGui::Text("Voxel volumes:    %u", stats.voxelVolumes);
	ImGui::Text("Debug lines:      %u", stats.debugLines);
	ImGui::Text("Descriptors used: %u", stats.descriptorsInUse);
	ImGui::Text("Entities:         %u", engine.GetScene().GetCoordinator().LivingCount());
	if (stats.d3dErrors > 0)
		ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "D3D12 errors: %u  warnings: %u", stats.d3dErrors, stats.d3dWarnings);
	else
		ImGui::Text("D3D12 errors: 0  warnings: %u", stats.d3dWarnings);

	ImGui::SeparatorText("Rendering");
	ImGui::Checkbox("VSync", &settings.vsync);
	ImGui::SameLine();
	ImGui::Checkbox("Culling", &settings.cullingEnabled);
	ImGui::SameLine();
	ImGui::Checkbox("Bounds (F3)", &settings.drawBounds);
	ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 4.0f);
	ImGui::SliderFloat3("Sun dir", &settings.sunDirection.x, -1.0f, 1.0f);
	ImGui::ColorEdit3("Sun", &settings.sunColor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
	ImGui::ColorEdit3("Ambient", &settings.ambient.x, ImGuiColorEditFlags_Float);
	ImGui::ColorEdit3("Sky", &settings.clearColor.x, ImGuiColorEditFlags_Float);

	ImGui::SeparatorText("Shaders");
	if (ImGui::Button("Reload shaders"))
	{
		renderer.ReloadShaders();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(also reloads on save)  reloads: %u", stats.shaderReloads);
	if (!renderer.ShaderError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
		ImGui::TextWrapped("%s", renderer.ShaderError().c_str());
		ImGui::PopStyleColor();
	}

	ImGui::SeparatorText("Debug");
	bool voxelTest = mVoxelTest;
	if (ImGui::Checkbox("Voxel test volume (F2)", &voxelTest))
	{
		SetVoxelTest(engine, voxelTest);
	}
	if (ImGui::Button("Game mode (F1)"))
	{
		engine.SetEditorMode(false);
	}
	ImGui::End();
}

void EditorLayer::SetVoxelTest(Engine& engine, bool enabled)
{
	Scene& scene = engine.GetScene();
	Renderer& renderer = engine.GetRenderer();
	if (enabled && !mVoxelTest)
	{
		VoxelVolumeRenderer volume;
		volume.volume = renderer.CreateVoxelVolume(MakeTestVolume());
		volume.voxelSize = 0.1f;   // 64 voxels -> 6.4 units

		Transform transform;
		transform.position = { 1.5f, -3.2f, -2.0f };
		mVoxelEntity = scene.CreateEntity();
		scene.Add(mVoxelEntity, transform);
		scene.Add(mVoxelEntity, volume);
	}
	else if (!enabled && mVoxelTest && scene.IsAlive(mVoxelEntity))
	{
		renderer.DestroyVoxelVolume(scene.Get<VoxelVolumeRenderer>(mVoxelEntity).volume);
		scene.DestroyEntity(mVoxelEntity);
		mVoxelEntity = INVALID_ENTITY;
	}
	mVoxelTest = enabled;
}

void EditorLayer::Shutdown(Engine& engine)
{
	if (mVoxelTest)
	{
		SetVoxelTest(engine, false);
	}
}
