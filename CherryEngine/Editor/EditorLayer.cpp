#include "Editor/EditorLayer.h"
#include "Core/Engine.h"
#include "Core/Project.h"
#include "Game/GameModule.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <commdlg.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace
{
	const char* kViewportWindow = "Viewport";
	const char* kStatsWindow = "Stats";
	const char* kGameWindow = "Game";
	const char* kOutputWindow = "Output";
	constexpr size_t kMaxLogLines = 4000;

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

void EditorLayer::Draw(Engine& engine, IGameModule* game, bool editorMode, bool gameOverlayWindow, float frameMs)
{
	mFrameMsSmoothed = mFrameMsSmoothed * 0.95f + frameMs * 0.05f;

	// Keep the log history even while the Output panel is hidden.
	std::vector<cherry::LogEntry> fresh;
	cherry::ReadLog(mLogSequence, fresh);
	for (cherry::LogEntry& entry : fresh)
	{
		mLogSequence = entry.sequence;
		mLog.push_back(std::move(entry));
		mLogScrollToBottom = true;
	}
	while (mLog.size() > kMaxLogLines) mLog.pop_front();

	HandleHotkeys(engine);
	if (mPhysicsDebug)
	{
		engine.GetPhysics().DrawBounds(engine.GetRenderer().Debug());
	}

	if (!editorMode)
	{
		viewportHovered = viewportFocused = false;
		if (!game) return;
		if (!gameOverlayWindow)
		{
			game->OnImGui(engine);   // the game draws its own HUD
			return;
		}
		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.85f);
		// A separate window from the editor's docked "Game" panel, so switching modes
		// doesn't undock that panel.
		ImGui::Begin("Game##Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
		game->OnImGui(engine);
		ImGui::TextDisabled("F1: editor  F2: voxel test  F3: bounds");
		ImGui::End();
		return;
	}

	HandleSceneHotkeys(engine);
	UpdateEditorCamera(engine);
	DrawMenuBar(engine);
	DrawDockspace();
	DrawViewport(engine);
	DrawHierarchy(engine);
	DrawInspector(engine);
	DrawStats(engine, frameMs);
	DrawOutput();
	DrawContentBrowser(engine);
	mGraphEditor.Draw(engine);
	DrawSelection(engine);
	UpdateUndo(engine);

	ImGui::Begin(kGameWindow);
	if (game)
	{
		game->OnImGui(engine);
	}
	else
	{
		const ModuleStatus status = engine.GetModuleStatus();
		if (!status.hasProject) ImGui::TextDisabled("No project open.");
		else if (!status.loaded) ImGui::TextDisabled("The game module isn't loaded. Compile it (Ctrl+Shift+B).");
		else ImGui::TextDisabled("Press Play (F5) to run the game.");
	}
	ImGui::End();

	if (!engine.Project().IsValid())
	{
		DrawProjectPicker(engine);
	}
}

void EditorLayer::OnWorldChanging(Engine& engine)
{
	mSelected = INVALID_ENTITY;   // entity ids belong to the world being left
	mEulerEntity = INVALID_ENTITY;
	// The test volume belongs to the world it was made in.
	if (mVoxelTest)
	{
		SetVoxelTest(engine, false);
	}
}

void EditorLayer::HandleHotkeys(Engine& engine)
{
	// Engine debug hotkeys (work in both modes).
	if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
	{
		SetVoxelTest(engine, !mVoxelTest);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F3, false))
	{
		engine.GetRenderer().Settings().drawBounds ^= true;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F4, false))
	{
		mPhysicsDebug = !mPhysicsDebug;
	}

	// Play controls.
	const ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
	{
		if (engine.GetPlayState() == PlayState::Edit) engine.Play();
		else engine.Stop();
	}
	if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B, false))
	{
		engine.CompileModule();
	}
}

void EditorLayer::DrawMenuBar(Engine& engine)
{
	if (!ImGui::BeginMainMenuBar()) return;

	const ModuleStatus status = engine.GetModuleStatus();
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Open Project...")) BrowseForProject(engine);
		if (ImGui::BeginMenu("Recent Projects"))
		{
			const std::vector<std::filesystem::path> recent = cherry::LoadRecentProjects();
			if (recent.empty()) ImGui::TextDisabled("(none)");
			for (const std::filesystem::path& file : recent)
			{
				if (ImGui::MenuItem(file.string().c_str())) engine.OpenProject(file);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Projects in Source Tree"))
		{
			const std::vector<ProjectDesc> known = cherry::FindKnownProjects();
			if (known.empty()) ImGui::TextDisabled("(none)");
			for (const ProjectDesc& project : known)
			{
				if (ImGui::MenuItem(project.name.c_str(), project.file.filename().string().c_str())) engine.OpenProject(project.file);
			}
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::BeginMenu("Export", status.hasProject))
		{
			const bool busy = engine.IsExportingAndroid();
			if (ImGui::MenuItem("Android APK (debug)", nullptr, false, !busy)) engine.ExportAndroid(false);
			ImGui::SetItemTooltip("Tools/Android/Package.ps1: builds the game for arm64 and writes out/android/<Module>-Debug.apk (see the Output panel)");
			if (ImGui::MenuItem("Android APK (release)", nullptr, false, !busy)) engine.ExportAndroid(true);
			ImGui::SetItemTooltip("Signed with the keystore in CHERRY_ANDROID_KEYSTORE (and its passwords), else the debug key");
			if (busy) ImGui::TextDisabled("Exporting...");
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Exit")) engine.RequestQuit();
		ImGui::EndMenu();
	}
	DrawSceneMenu(engine);
	DrawEditMenu(engine);

	if (status.hasProject)
	{
		ImGui::Separator();
		ImGui::TextUnformatted(engine.Project().name.c_str());
		ImGui::Separator();

		// Play controls.
		const PlayState state = engine.GetPlayState();
		ImGui::BeginDisabled(!status.loaded && state == PlayState::Edit);
		if (state == PlayState::Edit)
		{
			if (ImGui::Button("Play")) engine.Play();
			ImGui::SetItemTooltip("Run the game in a fresh play world (F5)");
		}
		else
		{
			if (ImGui::Button("Stop")) engine.Stop();
			ImGui::SetItemTooltip("Stop and return to the editor (F5)");
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(state == PlayState::Edit);
		bool paused = state == PlayState::Paused;
		if (ImGui::Checkbox("Pause", &paused)) engine.SetPaused(paused);
		ImGui::EndDisabled();

		// Module controls.
		if (!status.isStatic)
		{
			ImGui::Separator();
			ImGui::BeginDisabled(status.compiling);
			if (ImGui::Button(status.compiling ? "Compiling..." : "Compile")) engine.CompileModule();
			ImGui::SetItemTooltip("Build %s with CMake; it reloads when the build finishes (Ctrl+Shift+B)", status.moduleFile.c_str());
			if (ImGui::Button("Reload")) engine.ReloadModule();
			ImGui::SetItemTooltip("Load %s again from disk", status.moduleFile.c_str());
			ImGui::EndDisabled();

			if (status.compiling)            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "building %s", status.moduleFile.c_str());
			else if (status.lastBuildFailed) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1), "build failed (see Output)");
			else if (!status.loaded)         ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1), "%s", status.error.c_str());
			else                             ImGui::TextDisabled("%s loaded%s", status.moduleFile.c_str(), status.reloads ? " (reloaded)" : "");
		}

		const char* stateText = state == PlayState::Playing ? "PLAYING" : state == PlayState::Paused ? "PAUSED" : "";
		if (*stateText)
		{
			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", stateText);
		}
	}
	ImGui::EndMainMenuBar();
}

void EditorLayer::DrawProjectPicker(Engine& engine)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32.0f, 0), ImGuiCond_Always);
	ImGui::Begin("Open a project", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

	ImGui::SeparatorText("Projects in the source tree");
	const std::vector<ProjectDesc> known = cherry::FindKnownProjects();
	if (known.empty()) ImGui::TextDisabled("(none found)");
	for (const ProjectDesc& project : known)
	{
		ImGui::PushID(project.file.string().c_str());
		if (ImGui::Button(project.name.c_str(), ImVec2(-FLT_MIN, 0))) engine.OpenProject(project.file);
		ImGui::SetItemTooltip("%s", project.file.string().c_str());
		ImGui::PopID();
	}

	const std::vector<std::filesystem::path> recent = cherry::LoadRecentProjects();
	if (!recent.empty())
	{
		ImGui::SeparatorText("Recent");
		for (const std::filesystem::path& file : recent)
		{
			if (ImGui::Selectable(file.string().c_str())) engine.OpenProject(file);
		}
	}

	ImGui::Separator();
	if (ImGui::Button("Browse...")) BrowseForProject(engine);
	ImGui::End();
}

void EditorLayer::BrowseForProject(Engine& engine)
{
	wchar_t file[MAX_PATH] = L"";
	OPENFILENAMEW dialog{};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = engine.GetWindow().Handle();
	dialog.lpstrFilter = L"Cherry project (*.cherryproject)\0*.cherryproject\0";
	dialog.lpstrFile = file;
	dialog.nMaxFile = MAX_PATH;
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&dialog))
	{
		engine.OpenProject(file);
	}
}

void EditorLayer::DrawOutput()
{
	ImGui::Begin(kOutputWindow);
	if (ImGui::SmallButton("Clear")) mLog.clear();
	ImGui::SameLine();
	ImGui::Checkbox("Info", &mLogShowInfo);
	ImGui::SameLine();
	ImGui::Checkbox("Build", &mLogShowBuild);
	ImGui::Separator();

	ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
	for (const cherry::LogEntry& entry : mLog)
	{
		const bool isBuild = entry.text.rfind("[build]", 0) == 0;
		if (isBuild && !mLogShowBuild) continue;
		if (!isBuild && entry.level == cherry::LogLevel::Info && !mLogShowInfo) continue;
		if (entry.level == cherry::LogLevel::Error)        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", entry.text.c_str());
		else if (entry.level == cherry::LogLevel::Warning) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "%s", entry.text.c_str());
		else                                               ImGui::TextUnformatted(entry.text.c_str());
	}
	if (mLogScrollToBottom && atBottom) ImGui::SetScrollHereY(1.0f);
	mLogScrollToBottom = false;
	ImGui::EndChild();
	ImGui::End();
}

void EditorLayer::DrawDockspace()
{
	const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

	// First run (no imgui.ini layout yet, or one from before the newest panel existed):
	// hierarchy left, viewport over the output log in the middle, inspector/stats and game right.
	if (!mLayoutBuilt)
	{
		mLayoutBuilt = true;
		ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
		// A layout saved before the newest panel existed is rebuilt (bump this name when adding panels).
		const bool oldLayout = ImGui::FindWindowSettingsByID(ImHashStr("Content")) == nullptr;
		if (!node || (node->IsLeafNode() && node->Windows.Size == 0) || oldLayout)
		{
			ImGui::DockBuilderRemoveNode(dockspace);
			ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);
			ImGuiID center = 0;
			ImGuiID right = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.26f, nullptr, &center);
			ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
			ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
			// The viewport is the central node, so it absorbs window resizes and the side panels keep their width.
			if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(center))
			{
				centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
			}
			ImGuiID rightBottom = 0;
			ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.6f, nullptr, &rightBottom);
			ImGui::DockBuilderDockWindow(kOutputWindow, bottom);
			ImGui::DockBuilderDockWindow("Content", bottom);
			ImGui::DockBuilderDockWindow(kViewportWindow, center);
			ImGui::DockBuilderDockWindow("Hierarchy", left);
			ImGui::DockBuilderDockWindow(kStatsWindow, rightTop);
			ImGui::DockBuilderDockWindow("Inspector", rightTop);
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
	DrawViewportOverlay(engine);

	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorLayer::DrawStats(Engine& engine, float frameMs)
{
	Renderer& renderer = engine.GetRenderer();
	const RenderStats& stats = renderer.Stats();
	RenderSettings& settings = renderer.Settings();

	ImGui::Begin(kStatsWindow);
	ImGui::Text("%s  (%s)", renderer.AdapterName().c_str(), GraphicsApiName(renderer.GetGraphicsApi()));
	ImGui::Text("%.1f fps  (%.2f ms)", 1000.0f / std::max(mFrameMsSmoothed, 0.001f), mFrameMsSmoothed);
	ImGui::Text("View: %u x %u", stats.viewWidth, stats.viewHeight);
	ImGui::Text("GPU: %.2f ms", stats.gpuFrameMs);
	ImGui::TextDisabled("  mesh %.2f  voxel %.2f  tonemap %.2f", stats.gpuMeshMs, stats.gpuVoxelMs, stats.gpuTonemapMs);
	ImGui::TextDisabled("  debug %.2f  game ui %.2f  ui %.2f", stats.gpuDebugMs, stats.gpuGameUiMs, stats.gpuUiMs);
	if (stats.gameUiDraws > 0)
		ImGui::TextDisabled("  game ui: %u draws, %u vertices, %.2f ms CPU", stats.gameUiDraws, stats.gameUiVertices, stats.gameUiCpuMs);
	ImGui::Separator();
	ImGui::Text("Draw calls:       %u", stats.drawCalls);
	ImGui::Text("Instances:        %u  (culled %u)", stats.instancesSubmitted, stats.instancesCulled);
	ImGui::Text("Voxel volumes:    %u", stats.voxelVolumes);
	ImGui::Text("Debug lines:      %u", stats.debugLines);
	ImGui::Text("Descriptors used: %u", stats.descriptorsInUse);
	ImGui::Text("Entities:         %u", engine.GetScene().GetCoordinator().LivingCount());
	const PhysicsStats& physics = engine.GetPhysics().Stats();
	ImGui::Text("Physics:          %u bodies (%u active)  %.2f ms  %u steps", physics.bodies, physics.activeBodies, physics.stepMs, physics.stepsLastFrame);
	const char* api = GraphicsApiName(renderer.GetGraphicsApi());
	if (stats.d3dErrors > 0)
		ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "%s errors: %u  warnings: %u", api, stats.d3dErrors, stats.d3dWarnings);
	else
		ImGui::Text("%s errors: 0  warnings: %u", api, stats.d3dWarnings);

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
	ImGui::Checkbox("Physics bounds (F4)", &mPhysicsDebug);
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
	mGraphEditor.Shutdown(engine);
	if (mVoxelTest)
	{
		SetVoxelTest(engine, false);
	}
}
