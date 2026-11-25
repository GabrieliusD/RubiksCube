#pragma once
// Builds without the editor (CHERRY_EDITOR=0, e.g. Android): what Engine.cpp needs from the
// editor, the module host and the build process, reduced to a packaged game's behaviour. The
// game is linked in (monolithic), so there is no module to load, build or reload, and the
// window always shows the game view.
#include "Core/Engine.h"
#include "Game/GameModule.h"
#include "Scene/Components.h"
#include "imgui.h"
#include <DirectXMath.h>
#include <filesystem>
#include <string>

class EditorLayer
{
public:
	bool viewportHovered = false;
	bool viewportFocused = false;
	DirectX::XMFLOAT2 viewportPosition{ 0, 0 };
	DirectX::XMFLOAT2 viewportSize{ 0, 0 };

	// Game view only: what EditorLayer::Draw does with editorMode false.
	void Draw(Engine& engine, IGameModule* game, bool /*editorMode*/, bool gameOverlayWindow, float /*frameMs*/)
	{
		if (!game) return;
		if (!gameOverlayWindow)
		{
			game->OnImGui(engine);
			return;
		}
		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.85f);
		ImGui::Begin("Game##Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
		game->OnImGui(engine);
		ImGui::End();
	}
	void OnWorldChanging(Engine&) {}
	void OnSceneReplaced(Engine&) {}
	void GetEditorCamera(Transform&, Camera&) const {}
	void LoadSettings(const std::filesystem::path&) {}
	void SaveSettings(const std::filesystem::path&) const {}
	void Shutdown(Engine&) {}
};

class ModuleHost
{
public:
	bool Load(const std::filesystem::path&, const std::filesystem::path&, std::string& error)
	{
		error = "game modules can't be loaded in this build (the game is linked in)";
		return false;
	}
	IGameModule* Game() const { return nullptr; }
	void Unload() {}
	bool HasNewBuild() const { return false; }
	static void CleanShadowRoot(const std::filesystem::path&) {}
};

class BuildProcess
{
public:
	bool Start(const std::wstring&, std::string& error)
	{
		error = "building modules isn't supported in this build";
		return false;
	}
	bool Poll(int&) { return false; }
	bool IsRunning() const { return false; }
};
