#pragma once
#include "Scene/ECS.h"
#include "Core/Log.h"
#include "Editor/Gizmo.h"
#include "Core/Assets.h"
#include <DirectXMath.h>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class Engine;
class IGameModule;
class Scene;
struct ComponentInfo;
struct FieldInfo;
struct Transform;
struct Camera;

// The engine's ImGui UI. In editor mode: a menu bar (project, scene, play controls)
// and a dockspace with the Viewport, Hierarchy, Inspector, Stats, Output and Game
// panels (the module's OnImGui). In game mode: the game's UI as a small overlay.
// Also owns the editor fly camera, the selection, engine debug tools (voxel test)
// and the project picker shown when no project is open.
class EditorLayer
{
public:
	// `game` is the running game, or null while editing (its UI only runs while playing).
	// `gameOverlayWindow`: wrap the game's UI in a small window in game mode (EngineConfig).
	void Draw(Engine& engine, IGameModule* game, bool editorMode, bool gameOverlayWindow, float frameMs);
	// The active world is about to be destroyed or swapped (play / stop).
	void OnWorldChanging(Engine& engine);
	// The edit scene is about to be replaced (new / open scene).
	void OnSceneReplaced(Engine& engine);
	void Shutdown(Engine& engine);

	// The editor's fly camera (used as the view while editing).
	void GetEditorCamera(Transform& transform, Camera& camera) const;
	void LoadSettings(const std::filesystem::path& file);
	void SaveSettings(const std::filesystem::path& file) const;

	// Results of the last Draw(), used for input routing and view sizing.
	bool viewportHovered = false;
	bool viewportFocused = false;
	DirectX::XMFLOAT2 viewportPosition{ 0, 0 };   // window client coordinates
	DirectX::XMFLOAT2 viewportSize{ 0, 0 };

private:
	void HandleHotkeys(Engine& engine);
	void DrawMenuBar(Engine& engine);
	void DrawDockspace();
	void DrawViewport(Engine& engine);
	void DrawStats(Engine& engine, float frameMs);
	void DrawOutput();
	void DrawProjectPicker(Engine& engine);
	void BrowseForProject(Engine& engine);
	void SetVoxelTest(Engine& engine, bool enabled);

	// Scene editing (ScenePanels.cpp)
	void UpdateEditorCamera(Engine& engine);
	void DrawHierarchy(Engine& engine);
	void DrawHierarchyNode(Engine& engine, Scene& scene, Entity entity, const std::unordered_map<Entity, std::vector<Entity>>& children);
	void DrawCreateMenu(Engine& engine, Scene& scene, Entity parent);
	void DrawInspector(Engine& engine);
	bool DrawField(Engine& engine, Scene& scene, Entity entity, const FieldInfo& field, void* data);
	void DrawSceneMenu(Engine& engine);
	void DrawSelection(Engine& engine);
	void FocusSelection(Engine& engine);
	void HandleSceneHotkeys(Engine& engine);
	void SaveSceneAs(Engine& engine);
	void OpenSceneDialog(Engine& engine);
	void DeleteSelection(Engine& engine);
	void DuplicateSelection(Engine& engine);
	// Marks the edit scene changed (edits to the play world are thrown away on Stop).
	void Edited(Engine& engine);
	Entity ValidSelection(Engine& engine);

	// Viewport tools (ScenePanels.cpp): toolbar, icons, gizmo, click to select
	void DrawViewportOverlay(Engine& engine);
	DirectX::XMMATRIX EditorViewProjection() const;
	Entity PickEntity(Engine& engine, const DirectX::XMFLOAT2& screenPoint);

	// Undo (ScenePanels.cpp): whole-scene snapshots of the edit scene
	void UpdateUndo(Engine& engine);
	void Undo(Engine& engine);
	void Redo(Engine& engine);
	void RestoreSnapshot(Engine& engine, const std::string& json);
	void DrawEditMenu(Engine& engine);

	// Content browser and asset inspector (ContentBrowser.cpp)
	void DrawContentBrowser(Engine& engine);
	void DrawAssetInspector(Engine& engine);
	void HandleViewportDrop(Engine& engine);
	void SelectAsset(const std::string& path);
	void PlaceMesh(Engine& engine, const std::string& path, const DirectX::XMFLOAT3& position);

	bool mLayoutBuilt = false;
	bool mVoxelTest = false;
	bool mPhysicsDebug = false;
	Entity mVoxelEntity = INVALID_ENTITY;
	float mFrameMsSmoothed = 16.0f;

	// Editor fly camera
	struct EditorCamera
	{
		DirectX::XMFLOAT3 position{ 0.0f, 4.0f, -10.0f };
		float yaw = 0.0f;
		float pitch = 0.35f;
		float speed = 6.0f;      // m/s
		float fovY = DirectX::XM_PIDIV4;
	};
	EditorCamera mCamera;
	bool mCameraLooking = false;

	// Selection and inspector state
	Entity mSelected = INVALID_ENTITY;
	Entity mEulerEntity = INVALID_ENTITY;            // rotation shown as Euler angles for this entity...
	DirectX::XMFLOAT4 mEulerQuaternion{ 0, 0, 0, 1 }; // ...while its quaternion is still this one
	DirectX::XMFLOAT3 mEulerDegrees{ 0, 0, 0 };
	bool mHierarchyFocused = false;

	// Gizmo
	Gizmo mGizmo;
	Gizmo::Mode mGizmoMode = Gizmo::Mode::Translate;
	bool mGizmoLocal = false;
	Gizmo::Snap mSnap;

	// Undo: snapshots (scene JSON) of the edit scene before each change. A change is
	// committed once no widget or gizmo is being dragged, so one drag = one undo step.
	std::vector<std::string> mUndo;
	std::vector<std::string> mRedo;
	std::string mBaseline;            // the scene as of the last commit
	bool mBaselineValid = false;
	bool mPendingChange = false;

	// Content browser
	struct ContentEntry
	{
		std::string path;    // relative to Content, '/' separated
		std::string name;
		int depth = 0;
		bool folder = false;
	};
	std::vector<ContentEntry> mContent;
	float mContentScanTimer = 0.0f;
	std::string mSelectedAsset;            // asset shown in the inspector (no entity selected)
	MaterialFile mMaterialEdit;            // material being edited, and its path
	std::string mMaterialEditPath;
	bool mMaterialChanged = false;

	// Output panel
	std::deque<cherry::LogEntry> mLog;
	uint64_t mLogSequence = 0;
	bool mLogScrollToBottom = true;
	bool mLogShowInfo = true;
	bool mLogShowBuild = true;
};
