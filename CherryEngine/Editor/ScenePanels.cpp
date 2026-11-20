// The editor's scene tools: fly camera, Hierarchy, Inspector, the Scene menu and
// the selection outline.
#include "Editor/EditorLayer.h"
#include "Core/Engine.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Game/GameModule.h"
#include "Game/Script.h"
#include "imgui.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
	const char* kHierarchyWindow = "Hierarchy";
	const char* kInspectorWindow = "Inspector";
	const char* kEntityPayload = "CHERRY_ENTITY";

	constexpr float kRadToDeg = 57.29578f;
	constexpr float kDegToRad = 0.01745329f;

	// Euler angles (radians) matching Transform::SetRotationEuler (roll, then pitch, then yaw).
	XMFLOAT3 QuaternionToEuler(const XMFLOAT4& q)
	{
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(XMLoadFloat4(&q)));
		const float sinPitch = std::clamp(-m._32, -1.0f, 1.0f);
		const float pitch = std::asin(sinPitch);
		float yaw, roll;
		if (std::fabs(sinPitch) < 0.9999f)
		{
			yaw = std::atan2(m._31, m._33);
			roll = std::atan2(m._12, m._22);
		}
		else
		{
			yaw = std::atan2(-m._13, m._11);
			roll = 0.0f;
		}
		return { pitch, yaw, roll };
	}

	bool InputString(const char* label, std::string& value, ImGuiInputTextFlags flags = 0)
	{
		char buffer[512];
		std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
		if (ImGui::InputText(label, buffer, sizeof(buffer), flags))
		{
			value = buffer;
			return true;
		}
		return false;
	}

	// `filter`: e.g. L"Cherry scene (*.scene)\0*.scene\0", `extension`: L"scene".
	std::wstring FileDialog(HWND owner, bool save, const fs::path& initialDir, const std::wstring& defaultName,
		const wchar_t* filter = L"Cherry scene (*.scene)\0*.scene\0", const wchar_t* extension = L"scene")
	{
		wchar_t file[MAX_PATH];
		wcsncpy_s(file, defaultName.c_str(), _TRUNCATE);
		std::error_code ec;
		fs::create_directories(initialDir, ec);
		const std::wstring dir = initialDir.wstring();
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog);
		dialog.hwndOwner = owner;
		dialog.lpstrFilter = filter;
		dialog.lpstrFile = file;
		dialog.nMaxFile = MAX_PATH;
		dialog.lpstrInitialDir = dir.c_str();
		dialog.lpstrDefExt = extension;
		dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
		const BOOL ok = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
		return ok ? std::wstring(file) : std::wstring();
	}

	// Approximate world bounds of an entity for outlines and framing.
	bool EntityBounds(Engine& engine, Scene& scene, Entity entity, BoundingBox& out)
	{
		const MeshRenderer* renderer = scene.TryGet<MeshRenderer>(entity);
		if (!renderer) return false;
		const BoundingBox* bounds = engine.GetRenderer().GetMeshBounds(renderer->mesh);
		if (!bounds) return false;
		bounds->Transform(out, scene.WorldMatrix(entity));
		return true;
	}
}

// =============================================================================
// Camera
// =============================================================================

void EditorLayer::GetEditorCamera(Transform& transform, Camera& camera) const
{
	transform.position = mCamera.position;
	transform.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
	camera.fovY = mCamera.fovY;
	camera.nearZ = 0.05f;
	camera.farZ = 2000.0f;
}

void EditorLayer::LoadSettings(const fs::path& file)
{
	std::ifstream in(file);
	EditorCamera camera;
	if (in >> camera.position.x >> camera.position.y >> camera.position.z >> camera.yaw >> camera.pitch >> camera.speed)
	{
		mCamera = camera;
	}
}

void EditorLayer::SaveSettings(const fs::path& file) const
{
	std::ofstream out(file, std::ios::trunc);
	out << mCamera.position.x << " " << mCamera.position.y << " " << mCamera.position.z << " "
		<< mCamera.yaw << " " << mCamera.pitch << " " << mCamera.speed << "\n";
}

void EditorLayer::UpdateEditorCamera(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit) return;
	const ImGuiIO& io = ImGui::GetIO();

	if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) mCameraLooking = true;
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) mCameraLooking = false;

	Transform transform;
	transform.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
	const XMVECTOR forward = transform.Forward();
	const XMVECTOR right = transform.Right();
	XMVECTOR position = XMLoadFloat3(&mCamera.position);
	const float dt = std::min(io.DeltaTime, 0.1f);

	if (mCameraLooking)
	{
		// Right mouse held: mouse look, WASD + QE to fly, wheel changes speed.
		mCamera.yaw += io.MouseDelta.x * 0.0035f;
		mCamera.pitch = std::clamp(mCamera.pitch + io.MouseDelta.y * 0.0035f, -1.55f, 1.55f);
		if (io.MouseWheel != 0.0f) mCamera.speed = std::clamp(mCamera.speed * std::pow(1.2f, io.MouseWheel), 0.2f, 500.0f);

		XMVECTOR move = XMVectorZero();
		if (ImGui::IsKeyDown(ImGuiKey_W)) move += forward;
		if (ImGui::IsKeyDown(ImGuiKey_S)) move -= forward;
		if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
		if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
		if (ImGui::IsKeyDown(ImGuiKey_E)) move += XMVectorSet(0, 1, 0, 0);
		if (ImGui::IsKeyDown(ImGuiKey_Q)) move -= XMVectorSet(0, 1, 0, 0);
		const float boost = io.KeyShift ? 3.0f : 1.0f;
		position += move * (mCamera.speed * boost * dt);
	}
	else if (viewportHovered)
	{
		// Wheel: dolly. Middle drag: pan.
		if (io.MouseWheel != 0.0f) position += forward * (io.MouseWheel * mCamera.speed * 0.15f);
		if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
		{
			const XMVECTOR up = transform.Up();
			const float scale = 0.0025f * mCamera.speed;
			position += right * (-io.MouseDelta.x * scale) + up * (io.MouseDelta.y * scale);
		}
	}
	XMStoreFloat3(&mCamera.position, position);
}

void EditorLayer::FocusSelection(Engine& engine)
{
	const Entity selected = ValidSelection(engine);
	if (selected == INVALID_ENTITY) return;
	Scene& scene = engine.GetScene();

	XMFLOAT3 center = scene.WorldPosition(selected);
	float radius = 1.0f;
	BoundingBox bounds;
	if (EntityBounds(engine, scene, selected, bounds))
	{
		center = bounds.Center;
		radius = std::max(XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.Extents))), 0.25f);
	}
	Transform transform;
	transform.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
	const float distance = radius / std::tan(mCamera.fovY * 0.5f) * 1.2f;
	XMStoreFloat3(&mCamera.position, XMLoadFloat3(&center) - transform.Forward() * distance);
}

// =============================================================================
// Selection
// =============================================================================

Entity EditorLayer::ValidSelection(Engine& engine)
{
	if (mSelected != INVALID_ENTITY && !engine.GetScene().IsAlive(mSelected)) mSelected = INVALID_ENTITY;
	return mSelected;
}

void EditorLayer::Edited(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit) return;
	engine.MarkSceneDirty();
	mPendingChange = true;   // UpdateUndo commits it as one undo step
}

void EditorLayer::OnSceneReplaced(Engine& engine)
{
	(void)engine;
	mSelected = INVALID_ENTITY;
	mEulerEntity = INVALID_ENTITY;
	mUndo.clear();
	mRedo.clear();
	mBaselineValid = false;   // the next frame snapshots the new scene
}

void EditorLayer::DeleteSelection(Engine& engine)
{
	// Not while playing: game code may still hold the entity.
	if (engine.GetPlayState() != PlayState::Edit) return;
	const Entity selected = ValidSelection(engine);
	if (selected == INVALID_ENTITY) return;
	engine.GetScene().DestroyTree(selected);
	mSelected = INVALID_ENTITY;
	Edited(engine);
}

void EditorLayer::DuplicateSelection(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit) return;
	const Entity selected = ValidSelection(engine);
	if (selected == INVALID_ENTITY) return;
	mSelected = cherry::DuplicateEntity(engine.GetScene(), selected);
	Edited(engine);
}

void EditorLayer::DrawSelection(Engine& engine)
{
	const Entity selected = ValidSelection(engine);
	if (selected == INVALID_ENTITY) return;
	Scene& scene = engine.GetScene();
	DebugDraw& debug = engine.GetRenderer().Debug();
	BoundingBox bounds;
	if (EntityBounds(engine, scene, selected, bounds))
	{
		debug.Box(bounds, DebugDraw::Color(1.0f, 0.75f, 0.1f));
	}
	debug.Axes(scene.WorldPosition(selected), 0.5f);
	if (const PointLightComponent* light = scene.TryGet<PointLightComponent>(selected))
	{
		const XMFLOAT3 position = scene.WorldPosition(selected);
		const float r = light->range;
		debug.Box(BoundingBox(position, XMFLOAT3(r, r, r)), DebugDraw::Color(light->color.x, light->color.y, light->color.z));
	}
}

// =============================================================================
// Scene menu and hotkeys
// =============================================================================

void EditorLayer::OpenSceneDialog(Engine& engine)
{
	if (!engine.ConfirmDiscardSceneChanges()) return;
	const std::wstring file = FileDialog(engine.GetWindow().Handle(), false, engine.Project().content / "Scenes", L"");
	if (!file.empty()) engine.OpenScene(file);
}

void EditorLayer::SaveSceneAs(Engine& engine)
{
	const fs::path current = engine.ScenePath();
	const fs::path dir = current.empty() ? engine.Project().content / "Scenes" : current.parent_path();
	const std::wstring name = current.empty() ? L"NewScene.scene" : current.filename().wstring();
	const std::wstring file = FileDialog(engine.GetWindow().Handle(), true, dir, name);
	if (!file.empty()) engine.SaveScene(file);
}

void EditorLayer::DrawSceneMenu(Engine& engine)
{
	if (!engine.Project().IsValid()) return;
	if (!ImGui::BeginMenu("Scene")) return;
	if (ImGui::MenuItem("New Scene", "Ctrl+N"))
	{
		if (engine.ConfirmDiscardSceneChanges()) engine.NewScene();
	}
	if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) OpenSceneDialog(engine);
	ImGui::Separator();
	if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
	{
		if (engine.ScenePath().empty()) SaveSceneAs(engine);
		else engine.SaveScene();
	}
	if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) SaveSceneAs(engine);
	ImGui::Separator();
	const bool hasFile = !engine.ScenePath().empty();
	const fs::path startup = engine.Project().startupScene.empty() ? fs::path() : (engine.Project().root / engine.Project().startupScene).lexically_normal();
	const bool isStartup = hasFile && startup == engine.ScenePath();
	if (ImGui::MenuItem("Use as Startup Scene", nullptr, isStartup, hasFile && !isStartup))
	{
		engine.SetStartupScene(engine.ScenePath());
	}
	ImGui::SetItemTooltip("The scene the project opens with, and plays first when launched with --play");
	ImGui::EndMenu();
}

void EditorLayer::HandleSceneHotkeys(Engine& engine)
{
	const ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput || !engine.Project().IsValid()) return;
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
	{
		if (io.KeyShift || engine.ScenePath().empty()) SaveSceneAs(engine);
		else engine.SaveScene();
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenSceneDialog(engine);
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
	{
		if (engine.ConfirmDiscardSceneChanges()) engine.NewScene();
	}

	if (engine.GetPlayState() == PlayState::Edit && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
	{
		if (io.KeyShift) Redo(engine);
		else Undo(engine);
	}
	if (engine.GetPlayState() == PlayState::Edit && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo(engine);

	// Gizmo modes (W/E/R) while the viewport has the mouse or focus and the camera isn't flying.
	if ((viewportHovered || viewportFocused) && !mCameraLooking && !io.KeyCtrl)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) mGizmoMode = Gizmo::Mode::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) mGizmoMode = Gizmo::Mode::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) mGizmoMode = Gizmo::Mode::Scale;
	}

	// Selection commands: when the hierarchy or the viewport has focus.
	if (mHierarchyFocused || viewportFocused)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelection(engine);
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelection(engine);
		if (!mCameraLooking && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) FocusSelection(engine);
	}
}

// =============================================================================
// Hierarchy
// =============================================================================

void EditorLayer::DrawCreateMenu(Engine& engine, Scene& scene, Entity parent)
{
	struct Preset { const char* label; const char* mesh; const char* material; };
	static const Preset kMeshes[] =
	{
		{ "Cube", "engine/Cube", "engine/Default" },
		{ "Sphere", "engine/Sphere", "engine/Default" },
		{ "Plane", "engine/Plane", "engine/Default" },
		{ "Cylinder", "engine/Cylinder", "engine/Default" },
	};

	auto place = [&](Entity entity)
	{
		if (parent != INVALID_ENTITY)
		{
			scene.SetParent(entity, parent, false);
		}
		else
		{
			// In front of the editor camera.
			Transform view;
			view.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
			XMStoreFloat3(&scene.Get<Transform>(entity).position, XMLoadFloat3(&mCamera.position) + view.Forward() * 6.0f);
		}
		mSelected = entity;
		Edited(engine);
	};

	if (ImGui::MenuItem("Empty")) place(scene.CreateNamedEntity("Entity"));
	ImGui::Separator();
	for (const Preset& preset : kMeshes)
	{
		if (ImGui::MenuItem(preset.label))
		{
			const Entity entity = scene.CreateNamedEntity(preset.label);
			MeshRenderer renderer;
			renderer.meshAsset = preset.mesh;
			renderer.materialAsset = preset.material;
			scene.Add(entity, renderer);
			place(entity);
		}
	}
	ImGui::Separator();
	if (ImGui::MenuItem("Point Light"))
	{
		const Entity entity = scene.CreateNamedEntity("Point Light");
		scene.Add(entity, PointLightComponent{});
		place(entity);
	}
	if (ImGui::MenuItem("Camera"))
	{
		const Entity entity = scene.CreateNamedEntity("Camera");
		scene.Add(entity, Camera{});
		place(entity);
	}
	if (ImGui::MenuItem("Environment"))
	{
		const Entity entity = scene.CreateNamedEntity("Environment");
		scene.Add(entity, Environment{});
		place(entity);
	}
}

void EditorLayer::DrawHierarchyNode(Engine& engine, Scene& scene, Entity entity, const std::unordered_map<Entity, std::vector<Entity>>& children)
{
	auto it = children.find(entity);
	const bool hasChildren = it != children.end() && !it->second.empty();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
	if (entity == mSelected) flags |= ImGuiTreeNodeFlags_Selected;

	const std::string name = scene.DisplayName(entity);
	const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity) + 1), flags, "%s", name.c_str());
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) { mSelected = entity; mSelectedAsset.clear(); }

	if (ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload(kEntityPayload, &entity, sizeof(entity));
		ImGui::TextUnformatted(name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
		{
			const Entity dragged = *static_cast<const Entity*>(payload->Data);
			if (engine.GetPlayState() == PlayState::Edit && scene.SetParent(dragged, entity, true)) Edited(engine);
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem())
	{
		mSelected = entity;
		if (ImGui::BeginMenu("Create Child")) { DrawCreateMenu(engine, scene, entity); ImGui::EndMenu(); }
		const bool editing = engine.GetPlayState() == PlayState::Edit;
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editing)) DuplicateSelection(engine);
		if (ImGui::MenuItem("Delete", "Del", false, editing)) DeleteSelection(engine);
		if (ImGui::MenuItem("Save as Prefab...", nullptr, false, editing)) SaveAsPrefab(engine, entity);
		if (editing && scene.GetParent(entity) != INVALID_ENTITY && ImGui::MenuItem("Unparent"))
		{
			if (scene.SetParent(entity, INVALID_ENTITY, true)) Edited(engine);
		}
		ImGui::EndPopup();
	}

	if (open)
	{
		if (hasChildren)
		{
			for (Entity child : it->second)
			{
				if (scene.IsAlive(child)) DrawHierarchyNode(engine, scene, child, children);
			}
		}
		ImGui::TreePop();
	}
}

void EditorLayer::DrawHierarchy(Engine& engine)
{
	ImGui::Begin(kHierarchyWindow);
	mHierarchyFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (!engine.Project().IsValid())
	{
		ImGui::End();
		return;
	}
	Scene& scene = engine.GetScene();
	const bool playing = engine.GetPlayState() != PlayState::Edit;

	if (ImGui::Button("+ Create")) ImGui::OpenPopup("CreateEntity");
	if (ImGui::BeginPopup("CreateEntity"))
	{
		DrawCreateMenu(engine, scene, INVALID_ENTITY);
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (playing) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "play world (changes are discarded)");
	else ImGui::TextDisabled("%zu entities", scene.Entities().size());
	ImGui::Separator();

	// Build the tree once per frame: children in scene order.
	std::unordered_map<Entity, std::vector<Entity>> children;
	std::vector<Entity> roots;
	for (Entity entity : scene.Entities())
	{
		const Entity parent = scene.GetParent(entity);
		if (parent == INVALID_ENTITY) roots.push_back(entity);
		else children[parent].push_back(entity);
	}

	ImGui::BeginChild("##tree");
	for (Entity root : roots)
	{
		if (scene.IsAlive(root)) DrawHierarchyNode(engine, scene, root, children);
	}
	// Empty space below the tree: drop here to unparent, click to deselect, right-click to create.
	const ImVec2 rest = ImGui::GetContentRegionAvail();
	ImGui::InvisibleButton("##background", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 20.0f)));
	if (ImGui::IsItemClicked()) mSelected = INVALID_ENTITY;
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
		{
			const Entity dragged = *static_cast<const Entity*>(payload->Data);
			if (engine.GetPlayState() == PlayState::Edit && scene.SetParent(dragged, INVALID_ENTITY, true)) Edited(engine);
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem("##backgroundMenu"))
	{
		DrawCreateMenu(engine, scene, INVALID_ENTITY);
		ImGui::EndPopup();
	}
	ImGui::EndChild();
	ImGui::End();
}

// =============================================================================
// Inspector
// =============================================================================

bool EditorLayer::DrawField(Engine& engine, Scene& scene, Entity entity, const FieldInfo& field, void* data)
{
	if (field.flags & Field_Hidden) return false;
	const char* label = field.name.c_str();
	const float min = field.min, max = field.max;
	const bool readOnly = (field.flags & Field_ReadOnly) != 0;
	ImGui::BeginDisabled(readOnly);
	bool changed = false;

	switch (field.kind)
	{
	case FieldKind::Bool:
		changed = ImGui::Checkbox(label, static_cast<bool*>(data));
		break;
	case FieldKind::Int:
		changed = ImGui::DragInt(label, static_cast<int*>(data), 0.2f, static_cast<int>(min), static_cast<int>(max));
		break;
	case FieldKind::UInt:
		changed = ImGui::DragScalar(label, ImGuiDataType_U32, data, 0.2f);
		break;
	case FieldKind::UInt64:
		ImGui::Text("%s: %016llx", label, static_cast<unsigned long long>(*static_cast<uint64_t*>(data)));
		break;
	case FieldKind::Float:
		if (field.flags & Field_Angle)
		{
			float degrees = *static_cast<float*>(data) * kRadToDeg;
			if (ImGui::DragFloat(label, &degrees, 0.25f, min * kRadToDeg, max * kRadToDeg, "%.1f deg"))
			{
				*static_cast<float*>(data) = degrees * kDegToRad;
				changed = true;
			}
		}
		else
		{
			changed = ImGui::DragFloat(label, static_cast<float*>(data), field.speed, min, max);
		}
		break;
	case FieldKind::Float2:
		changed = ImGui::DragFloat2(label, static_cast<float*>(data), field.speed, min, max);
		break;
	case FieldKind::Float3:
		if (field.flags & Field_Color) changed = ImGui::ColorEdit3(label, static_cast<float*>(data), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		else changed = ImGui::DragFloat3(label, static_cast<float*>(data), field.speed, min, max);
		break;
	case FieldKind::Float4:
		if (field.flags & Field_Color)
		{
			changed = ImGui::ColorEdit4(label, static_cast<float*>(data), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		}
		else if (field.flags & Field_Rotation)
		{
			// Show Euler degrees; keep the numbers the user typed while the quaternion is unchanged.
			XMFLOAT4& q = *static_cast<XMFLOAT4*>(data);
			const bool same = mEulerEntity == entity && XMVector4NearEqual(XMLoadFloat4(&q), XMLoadFloat4(&mEulerQuaternion), XMVectorReplicate(1e-5f));
			if (!same)
			{
				const XMFLOAT3 euler = QuaternionToEuler(q);
				mEulerDegrees = { euler.x * kRadToDeg, euler.y * kRadToDeg, euler.z * kRadToDeg };
				mEulerEntity = entity;
			}
			if (ImGui::DragFloat3(label, &mEulerDegrees.x, 0.25f, 0.0f, 0.0f, "%.1f"))
			{
				XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationRollPitchYaw(
					mEulerDegrees.x * kDegToRad, mEulerDegrees.y * kDegToRad, mEulerDegrees.z * kDegToRad)));
				changed = true;
			}
			mEulerQuaternion = q;
		}
		else
		{
			changed = ImGui::DragFloat4(label, static_cast<float*>(data), field.speed, min, max);
		}
		break;
	case FieldKind::Int3:
		changed = ImGui::DragInt3(label, static_cast<int*>(data), 0.2f);
		break;
	case FieldKind::String:
	{
		std::string& value = *static_cast<std::string*>(data);
		if (!field.assetType.empty())
		{
			if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
			{
				if (ImGui::Selectable("(none)", value.empty())) { value.clear(); changed = true; }
				for (const std::string& path : engine.GetAssets().List(field.assetType))
				{
					if (ImGui::Selectable(path.c_str(), path == value)) { value = path; changed = true; }
				}
				ImGui::EndCombo();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CHERRY_ASSET"))
				{
					const std::string dropped = static_cast<const char*>(payload->Data);
					if (AssetManager::TypeOf(dropped) == field.assetType) { value = dropped; changed = true; }
				}
				ImGui::EndDragDropTarget();
			}
		}
		else
		{
			changed = InputString(label, value);
		}
		break;
	}
	case FieldKind::Enum:
	{
		int32_t current = 0;
		std::memcpy(&current, data, std::min<size_t>(field.size, sizeof(current)));
		const char* preview = current >= 0 && current < static_cast<int32_t>(field.enumNames.size()) ? field.enumNames[current].c_str() : "?";
		if (ImGui::BeginCombo(label, preview))
		{
			for (int32_t i = 0; i < static_cast<int32_t>(field.enumNames.size()); ++i)
			{
				if (ImGui::Selectable(field.enumNames[i].c_str(), i == current))
				{
					std::memset(data, 0, field.size);
					std::memcpy(data, &i, std::min<size_t>(field.size, sizeof(i)));
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		break;
	}
	case FieldKind::EntityRef:
	{
		Entity& target = *static_cast<Entity*>(data);
		const std::string preview = scene.IsAlive(target) ? scene.DisplayName(target) : "(none)";
		if (ImGui::BeginCombo(label, preview.c_str()))
		{
			if (ImGui::Selectable("(none)", !scene.IsAlive(target))) { target = INVALID_ENTITY; changed = true; }
			for (Entity candidate : scene.Entities())
			{
				if (candidate == entity) continue;
				ImGui::PushID(static_cast<int>(candidate));
				if (ImGui::Selectable(scene.DisplayName(candidate).c_str(), candidate == target)) { target = candidate; changed = true; }
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
			{
				target = *static_cast<const Entity*>(payload->Data);
				changed = true;
			}
			ImGui::EndDragDropTarget();
		}
		break;
	}
	}
	ImGui::EndDisabled();
	return changed;
}

void EditorLayer::DrawInspector(Engine& engine)
{
	ImGui::Begin(kInspectorWindow);
	const Entity entity = engine.Project().IsValid() ? ValidSelection(engine) : INVALID_ENTITY;
	if (entity == INVALID_ENTITY && !mSelectedAsset.empty() && engine.Project().IsValid())
	{
		DrawAssetInspector(engine);
		ImGui::End();
		return;
	}
	if (entity == INVALID_ENTITY)
	{
		ImGui::TextDisabled("Select an entity in the Hierarchy, or an asset in Content.");
		ImGui::End();
		return;
	}
	Scene& scene = engine.GetScene();
	const TypeRegistry& registry = TypeRegistry::Get();

	// Name (gives runtime-made entities an EntityInfo on first edit).
	std::string name = scene.Has<EntityInfo>(entity) ? scene.Get<EntityInfo>(entity).name : std::string();
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (InputString("##name", name))
	{
		scene.EnsureGuid(entity);
		scene.Get<EntityInfo>(entity).name = name;
		Edited(engine);
	}
	ImGui::TextDisabled("id %u  guid %016llx", entity, static_cast<unsigned long long>(scene.GetGuid(entity)));
	if (scene.Has<EntityInfo>(entity) && !scene.Get<EntityInfo>(entity).prefab.empty())
	{
		const std::string prefab = scene.Get<EntityInfo>(entity).prefab;
		ImGui::TextColored(ImVec4(0.6f, 0.75f, 1.0f, 1), "Prefab: %s", prefab.c_str());
		if (engine.GetPlayState() == PlayState::Edit)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Save to Prefab"))
			{
				std::string error;
				if (cherry::SavePrefab(scene, entity, engine.GetAssets().FullPath(prefab), error)) CHERRY_LOG("Saved prefab %s", prefab.c_str());
				else CHERRY_ERROR("Saving prefab %s failed: %s", prefab.c_str(), error.c_str());
			}
			ImGui::SetItemTooltip("Overwrite %s with this entity and its children.\nInstances already placed keep their own copy.", prefab.c_str());
		}
	}

	const ComponentInfo* toRemove = nullptr;
	for (const auto& infoPtr : registry.Components())
	{
		const ComponentInfo& info = *infoPtr;
		if (info.Is(Component_Hidden)) continue;
		void* component = info.get(scene, entity);
		if (!component) continue;

		ImGui::PushID(info.name.c_str());
		bool keep = true;
		const bool removable = !info.Is(Component_NotRemovable) && engine.GetPlayState() == PlayState::Edit;
		const std::string header = info.Is(Component_Script) ? info.name + "  (script)" : info.name;
		const bool open = ImGui::CollapsingHeader(header.c_str(), removable ? &keep : nullptr, ImGuiTreeNodeFlags_DefaultOpen);
		if (!keep) toRemove = &info;
		if (open)
		{
			if (info.Is(Component_Transient)) ImGui::TextDisabled("runtime only: not saved");
			for (const FieldInfo& field : info.fields)
			{
				if (!DrawField(engine, scene, entity, field, field.access(component))) continue;
				Edited(engine);
				// A new asset path: drop the old handle so the engine loads the new asset.
				if (info.name == "MeshRenderer")
				{
					MeshRenderer& renderer = *static_cast<MeshRenderer*>(component);
					if (field.name == "mesh") renderer.mesh = {};
					if (field.name == "material") renderer.material = {};
				}
			}
			if (info.fields.empty()) ImGui::TextDisabled("(no editable fields)");
			if (info.Is(Component_Script) && info.asScript)
			{
				Script& script = *info.asScript(component);
				if (!ScriptBinding::IsBound(script) || script.GetEntity() != entity) ScriptBinding::Bind(script, &engine, entity);
				script.OnInspectorGui();
				if (!info.sourceFile.empty())
				{
					if (ImGui::SmallButton("Edit Script")) ShellExecuteW(nullptr, L"open", fs::u8path(info.sourceFile).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
					ImGui::SetItemTooltip("%s", info.sourceFile.c_str());
				}
			}
		}
		ImGui::PopID();
	}
	if (toRemove)
	{
		toRemove->remove(scene, entity);
		Edited(engine);
	}

	for (const auto& system : engine.GameSystems())
	{
		system->OnInspectorGui(engine, entity);
	}

	if (const UnknownComponents* unknown = scene.TryGet<UnknownComponents>(entity))
	{
		ImGui::SeparatorText("Not loaded");
		for (const auto& entry : unknown->entries)
		{
			ImGui::BulletText("%s", entry.first.c_str());
		}
		ImGui::TextDisabled("Kept as they are: their module isn't loaded.");
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (engine.GetPlayState() != PlayState::Edit)
	{
		ImGui::TextDisabled("Playing: field changes apply to the play world and are\nthrown away on Stop. Components can't be added or removed.");
		ImGui::End();
		return;
	}
	if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("AddComponent");
	if (ImGui::BeginPopup("AddComponent"))
	{
		bool any = false;
		for (const auto& infoPtr : registry.Components())
		{
			const ComponentInfo& info = *infoPtr;
			if (info.Is(Component_NotAddable) || info.Is(Component_Hidden) || info.has(scene, entity)) continue;
			any = true;
			if (ImGui::MenuItem(info.name.c_str()))
			{
				info.addDefault(scene, entity);
				if (info.name != "Transform" && !scene.Has<Transform>(entity)) scene.Add(entity, Transform{});
				Edited(engine);
			}
		}
		if (!any) ImGui::TextDisabled("(nothing to add)");
		ImGui::EndPopup();
	}
	ImGui::End();
}

// =============================================================================
// Viewport tools: toolbar, icons, gizmo, click to select
// =============================================================================

XMMATRIX EditorLayer::EditorViewProjection() const
{
	Transform transform;
	Camera camera;
	GetEditorCamera(transform, camera);
	const XMMATRIX world = XMMatrixRotationQuaternion(XMLoadFloat4(&transform.rotation))
		* XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
	const float aspect = viewportSize.x / std::max(viewportSize.y, 1.0f);
	return XMMatrixInverse(nullptr, world) * XMMatrixPerspectiveFovLH(camera.fovY, aspect, camera.nearZ, camera.farZ);
}

namespace
{
	bool ProjectToScreen(FXMMATRIX viewProj, const XMFLOAT3& world, const XMFLOAT2& origin, const XMFLOAT2& size, ImVec2& out)
	{
		const XMVECTOR clip = XMVector4Transform(XMVectorSet(world.x, world.y, world.z, 1.0f), viewProj);
		const float w = XMVectorGetW(clip);
		if (w <= 1e-4f) return false;
		out = ImVec2(origin.x + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - XMVectorGetY(clip) / w * 0.5f) * size.y);
		return true;
	}

	// Entities without a mesh that still need a clickable marker.
	bool HasIcon(Scene& scene, Entity entity)
	{
		return scene.Has<PointLightComponent>(entity) || scene.Has<Camera>(entity)
			|| (!scene.Has<MeshRenderer>(entity) && !scene.Has<Environment>(entity) && scene.Has<Transform>(entity) && scene.Has<EntityInfo>(entity));
	}
}

Entity EditorLayer::PickEntity(Engine& engine, const XMFLOAT2& screenPoint)
{
	Scene& scene = engine.GetScene();
	const XMMATRIX viewProj = EditorViewProjection();

	// Markers first: they sit on top of everything.
	Entity best = INVALID_ENTITY;
	float bestPixels = 12.0f;
	for (Entity entity : scene.Entities())
	{
		if (!HasIcon(scene, entity)) continue;
		ImVec2 point;
		if (!ProjectToScreen(viewProj, scene.WorldPosition(entity), viewportPosition, viewportSize, point)) continue;
		const float d = std::hypot(point.x - screenPoint.x, point.y - screenPoint.y);
		if (d < bestPixels) { bestPixels = d; best = entity; }
	}
	if (best != INVALID_ENTITY) return best;

	// Then meshes, by their bounding boxes.
	const float x = (screenPoint.x - viewportPosition.x) / viewportSize.x * 2.0f - 1.0f;
	const float y = 1.0f - (screenPoint.y - viewportPosition.y) / viewportSize.y * 2.0f;
	const XMMATRIX inverse = XMMatrixInverse(nullptr, viewProj);
	const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(x, y, 0.0f, 1.0f), inverse);
	const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(x, y, 1.0f, 1.0f), inverse);
	Ray ray;
	ray.origin = mCamera.position;
	XMStoreFloat3(&ray.direction, XMVector3Normalize(farPoint - nearPoint));
	if (const std::optional<RaycastHit> hit = engine.Raycast(ray)) return hit->entity;
	return INVALID_ENTITY;
}

void EditorLayer::DrawViewportOverlay(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit || !engine.Project().IsValid()) return;
	HandleViewportDrop(engine);
	Scene& scene = engine.GetScene();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const XMMATRIX viewProj = EditorViewProjection();

	// ---- Markers for lights, cameras and empty entities ------------------------------------
	for (Entity entity : scene.Entities())
	{
		if (!HasIcon(scene, entity)) continue;
		ImVec2 point;
		if (!ProjectToScreen(viewProj, scene.WorldPosition(entity), viewportPosition, viewportSize, point)) continue;
		const bool selected = entity == mSelected;
		const ImU32 outline = selected ? IM_COL32(255, 190, 40, 255) : IM_COL32(20, 20, 20, 200);
		if (const PointLightComponent* light = scene.TryGet<PointLightComponent>(entity))
		{
			const ImU32 fill = IM_COL32(static_cast<int>(std::min(light->color.x, 1.0f) * 255), static_cast<int>(std::min(light->color.y, 1.0f) * 255),
				static_cast<int>(std::min(light->color.z, 1.0f) * 255), 255);
			draw->AddCircleFilled(point, 7.0f, fill);
			draw->AddCircle(point, 8.0f, outline, 0, 2.0f);
		}
		else if (scene.Has<Camera>(entity))
		{
			draw->AddRectFilled(ImVec2(point.x - 8, point.y - 5), ImVec2(point.x + 6, point.y + 5), IM_COL32(200, 200, 210, 255));
			draw->AddTriangleFilled(ImVec2(point.x + 6, point.y), ImVec2(point.x + 11, point.y - 5), ImVec2(point.x + 11, point.y + 5), IM_COL32(200, 200, 210, 255));
			draw->AddRect(ImVec2(point.x - 9, point.y - 6), ImVec2(point.x + 7, point.y + 6), outline, 0.0f, 0, 2.0f);
		}
		else
		{
			draw->AddCircle(point, 5.0f, selected ? outline : IM_COL32(230, 230, 230, 200), 0, 2.0f);
		}
	}

	// ---- Toolbar ------------------------------------------------------------------------
	ImGui::SetCursorScreenPos(ImVec2(viewportPosition.x + 8, viewportPosition.y + 8));
	auto modeButton = [&](const char* label, Gizmo::Mode mode, const char* tip)
	{
		const bool active = mGizmoMode == mode;
		if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(label)) mGizmoMode = mode;
		if (active) ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", tip);
		ImGui::SameLine();
	};
	modeButton("Move", Gizmo::Mode::Translate, "Move (W)");
	modeButton("Rotate", Gizmo::Mode::Rotate, "Rotate (E)");
	modeButton("Scale", Gizmo::Mode::Scale, "Scale (R)");
	if (ImGui::Button(mGizmoLocal ? "Local" : "World")) mGizmoLocal = !mGizmoLocal;
	ImGui::SetItemTooltip("Gizmo axes: the entity's own or the world's (scale is always local)");
	ImGui::SameLine();
	ImGui::Checkbox("Snap", &mSnap.enabled);
	ImGui::SetItemTooltip("Snap moves to %.2f m, turns to %.0f deg, scales to %.2f (hold Ctrl to flip). Steps: Edit menu.", mSnap.translate, mSnap.rotate, mSnap.scale);
	const bool overToolbar = ImGui::IsAnyItemHovered();

	// ---- Gizmo ------------------------------------------------------------------------------
	const Entity selected = ValidSelection(engine);
	if (selected != INVALID_ENTITY && scene.Has<Transform>(selected))
	{
		Gizmo::View view;
		XMStoreFloat4x4(&view.viewProj, viewProj);
		view.cameraPosition = mCamera.position;
		view.originX = viewportPosition.x;
		view.originY = viewportPosition.y;
		view.width = viewportSize.x;
		view.height = viewportSize.y;

		Gizmo::Pose pose;
		XMVECTOR scale, rotation, translation;
		XMMatrixDecompose(&scale, &rotation, &translation, scene.WorldMatrix(selected));
		XMStoreFloat3(&pose.position, translation);
		XMStoreFloat4(&pose.rotation, rotation);
		XMStoreFloat3(&pose.scale, scale);

		Gizmo::Snap snap = mSnap;
		if (io.KeyCtrl) snap.enabled = !snap.enabled;
		if (mGizmo.Manipulate(view, mGizmoMode, mGizmoLocal, snap, pose))
		{
			// Back to a local Transform under the parent.
			const XMMATRIX world = XMMatrixScaling(pose.scale.x, pose.scale.y, pose.scale.z)
				* XMMatrixRotationQuaternion(XMLoadFloat4(&pose.rotation))
				* XMMatrixTranslation(pose.position.x, pose.position.y, pose.position.z);
			const Entity parent = scene.GetParent(selected);
			const XMMATRIX local = parent == INVALID_ENTITY ? world : world * XMMatrixInverse(nullptr, scene.WorldMatrix(parent));
			Transform& transform = scene.Get<Transform>(selected);
			if (XMMatrixDecompose(&scale, &rotation, &translation, local))
			{
				XMStoreFloat3(&transform.position, translation);
				XMStoreFloat4(&transform.rotation, XMQuaternionNormalize(rotation));
				// Only the scale tool writes scale: decomposing adds float noise to the others.
				if (mGizmoMode == Gizmo::Mode::Scale) XMStoreFloat3(&transform.scale, scale);
			}
			Edited(engine);
		}
	}

	// ---- Click to select ----------------------------------------------------------------------
	if (viewportHovered && !overToolbar && !mGizmo.IsHovered() && !mGizmo.IsDragging() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		mSelected = PickEntity(engine, XMFLOAT2(io.MousePos.x, io.MousePos.y));
	}
}

// =============================================================================
// Undo
// =============================================================================

void EditorLayer::UpdateUndo(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit || !engine.Project().IsValid()) return;
	Scene& scene = engine.GetEditScene();
	if (!mBaselineValid)
	{
		mBaseline = cherry::SceneToJson(scene);
		mBaselineValid = true;
		mPendingChange = false;
		return;
	}
	// Commit once the drag (slider, gizmo) that made the change is over.
	if (!mPendingChange || ImGui::IsAnyItemActive() || mGizmo.IsDragging()) return;
	mPendingChange = false;
	std::string now = cherry::SceneToJson(scene);
	if (now == mBaseline) return;
	mUndo.push_back(std::move(mBaseline));
	if (mUndo.size() > 200) mUndo.erase(mUndo.begin());
	mRedo.clear();
	mBaseline = std::move(now);
}

void EditorLayer::RestoreSnapshot(Engine& engine, const std::string& json)
{
	Scene& scene = engine.GetEditScene();
	const uint64_t selectedGuid = scene.GetGuid(ValidSelection(engine));
	if (mVoxelTest) SetVoxelTest(engine, false);   // it lives in the scene being replaced
	std::string error;
	if (!cherry::SceneFromJson(scene, json, error))
	{
		CHERRY_ERROR("Undo failed: %s", error.c_str());
		return;
	}
	mSelected = scene.FindByGuid(selectedGuid);
	mEulerEntity = INVALID_ENTITY;
	engine.MarkSceneDirty();
}

void EditorLayer::Undo(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit) return;
	UpdateUndo(engine);   // commit a change still pending
	if (mUndo.empty()) return;
	mRedo.push_back(mBaseline);
	mBaseline = std::move(mUndo.back());
	mUndo.pop_back();
	RestoreSnapshot(engine, mBaseline);
}

void EditorLayer::Redo(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit || mRedo.empty()) return;
	mUndo.push_back(mBaseline);
	mBaseline = std::move(mRedo.back());
	mRedo.pop_back();
	RestoreSnapshot(engine, mBaseline);
}

void EditorLayer::DrawEditMenu(Engine& engine)
{
	if (!engine.Project().IsValid() || !ImGui::BeginMenu("Edit")) return;
	const bool editing = engine.GetPlayState() == PlayState::Edit;
	if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editing && !mUndo.empty())) Undo(engine);
	if (ImGui::MenuItem("Redo", "Ctrl+Y", false, editing && !mRedo.empty())) Redo(engine);
	ImGui::Separator();
	const bool hasSelection = ValidSelection(engine) != INVALID_ENTITY;
	if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editing && hasSelection)) DuplicateSelection(engine);
	if (ImGui::MenuItem("Delete", "Del", false, editing && hasSelection)) DeleteSelection(engine);
	if (ImGui::MenuItem("Frame Selected", "F", false, hasSelection)) FocusSelection(engine);
	ImGui::Separator();
	if (ImGui::MenuItem("Move", "W", mGizmoMode == Gizmo::Mode::Translate)) mGizmoMode = Gizmo::Mode::Translate;
	if (ImGui::MenuItem("Rotate", "E", mGizmoMode == Gizmo::Mode::Rotate)) mGizmoMode = Gizmo::Mode::Rotate;
	if (ImGui::MenuItem("Scale", "R", mGizmoMode == Gizmo::Mode::Scale)) mGizmoMode = Gizmo::Mode::Scale;
	ImGui::MenuItem("Local Axes", nullptr, &mGizmoLocal);
	ImGui::MenuItem("Snap", nullptr, &mSnap.enabled);
	ImGui::DragFloat("Move step", &mSnap.translate, 0.01f, 0.01f, 10.0f, "%.2f m");
	ImGui::DragFloat("Turn step", &mSnap.rotate, 0.5f, 1.0f, 90.0f, "%.0f deg");
	ImGui::DragFloat("Scale step", &mSnap.scale, 0.01f, 0.01f, 1.0f, "%.2f");
	ImGui::EndMenu();
}

// =============================================================================
// Prefabs
// =============================================================================

void EditorLayer::SaveAsPrefab(Engine& engine, Entity entity)
{
	Scene& scene = engine.GetEditScene();
	if (!scene.IsAlive(entity)) return;
	const fs::path dir = engine.Project().content / "Prefabs";
	std::wstring name = fs::u8path(scene.DisplayName(entity)).wstring() + L".prefab";
	std::replace_if(name.begin(), name.end(), [](wchar_t c) { return c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|'; }, L'_');
	const std::wstring file = FileDialog(engine.GetWindow().Handle(), true, dir, name, L"Cherry prefab (*.prefab)\0*.prefab\0", L"prefab");
	if (file.empty()) return;

	const std::string path = engine.GetAssets().AssetPathOf(file);
	if (path.empty())
	{
		CHERRY_ERROR("Prefabs must be saved inside the project's Content folder");
		return;
	}
	// The entity becomes an instance of the new prefab.
	scene.EnsureGuid(entity);
	scene.Get<EntityInfo>(entity).prefab = path;
	std::string error;
	if (!cherry::SavePrefab(scene, entity, file, error))
	{
		CHERRY_ERROR("Saving the prefab failed: %s", error.c_str());
		return;
	}
	Edited(engine);
	mContentScanTimer = 0.0f;
	CHERRY_LOG("Saved prefab %s", path.c_str());
}
