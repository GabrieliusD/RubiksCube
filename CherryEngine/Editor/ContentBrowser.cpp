// The Content panel (the project's Content folder as a tree, drag assets out of it),
// the asset inspector (material editor) and dropping assets into the viewport.
#include "Editor/EditorLayer.h"
#include "Core/Engine.h"
#include "Scene/Scene.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <optional>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{
	const char* kContentWindow = "Content";
	const char* kAssetPayload = "CHERRY_ASSET";

	ImVec4 TypeColor(const std::string& type)
	{
		if (type == "mesh") return ImVec4(0.55f, 0.80f, 1.00f, 1);
		if (type == "material") return ImVec4(1.00f, 0.70f, 0.45f, 1);
		if (type == "texture") return ImVec4(0.70f, 0.95f, 0.60f, 1);
		if (type == "scene") return ImVec4(0.95f, 0.85f, 0.45f, 1);
		return ImVec4(0.55f, 0.55f, 0.55f, 1);
	}

	void AssetDragSource(const std::string& path)
	{
		if (ImGui::BeginDragDropSource())
		{
			ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
			ImGui::TextUnformatted(path.c_str());
			ImGui::EndDragDropSource();
		}
	}

	std::string UniqueName(const fs::path& folder, const std::string& stem, const std::string& extension)
	{
		std::string name = stem + extension;
		for (int i = 2; fs::exists(folder / name); ++i) name = stem + std::to_string(i) + extension;
		return name;
	}
}

void EditorLayer::SelectAsset(const std::string& path)
{
	mSelectedAsset = path;
	mSelected = INVALID_ENTITY;
	mMaterialEditPath.clear();   // reloaded by the asset inspector
}

void EditorLayer::DrawContentBrowser(Engine& engine)
{
	ImGui::Begin(kContentWindow);
	AssetManager& assets = engine.GetAssets();
	const fs::path root = assets.ContentRoot();
	if (!engine.Project().IsValid() || root.empty())
	{
		ImGui::End();
		return;
	}

	// Rescan the folder every couple of seconds (cheap for a project's Content).
	mContentScanTimer -= ImGui::GetIO().DeltaTime;
	bool refresh = ImGui::SmallButton("Refresh");
	if (refresh || mContentScanTimer <= 0.0f)
	{
		mContentScanTimer = 2.0f;
		mContent.clear();
		std::error_code ec;
		std::vector<fs::path> files;
		for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) files.push_back(it->path());
		std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b)
		{
			// Folders before files at each level, then by name.
			const fs::path pa = a.parent_path(), pb = b.parent_path();
			if (pa != pb) return a < b;
			const bool da = fs::is_directory(a), db = fs::is_directory(b);
			return da != db ? da : a.filename() < b.filename();
		});
		for (const fs::path& file : files)
		{
			ContentEntry entry;
			const fs::path relative = file.lexically_relative(root);
			entry.path = relative.generic_u8string();
			entry.name = file.filename().u8string();
			entry.depth = static_cast<int>(std::distance(relative.begin(), relative.end())) - 1;
			entry.folder = fs::is_directory(file, ec);
			mContent.push_back(std::move(entry));
		}
		if (refresh) assets.Refresh();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("New Material"))
	{
		// Next to the selected asset, else in Content/Materials.
		fs::path folder = root / "Materials";
		if (!mSelectedAsset.empty() && mSelectedAsset.find('#') == std::string::npos && mSelectedAsset.rfind("engine/", 0) != 0)
		{
			const fs::path selected = root / fs::u8path(mSelectedAsset);
			folder = fs::is_directory(selected) ? selected : selected.parent_path();
		}
		std::error_code ec;
		fs::create_directories(folder, ec);
		const std::string path = (folder / UniqueName(folder, "NewMaterial", ".mat")).lexically_relative(root).generic_u8string();
		if (assets.WriteMaterial(path, MaterialFile{}))
		{
			SelectAsset(path);
			mContentScanTimer = 0.0f;
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Open Folder"))
	{
		std::error_code ec;
		fs::create_directories(root, ec);
		ShellExecuteW(nullptr, L"explore", root.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	ImGui::Separator();

	ImGui::BeginChild("##files");
	if (mContent.empty()) ImGui::TextDisabled("Content is empty. Put .glb/.gltf, .png, .mat or .scene files in\n%s", root.u8string().c_str());

	// The tree: skip the children of closed folders.
	int hiddenBelow = INT_MAX;
	std::vector<bool> openStack;
	for (const ContentEntry& entry : mContent)
	{
		while (static_cast<int>(openStack.size()) > entry.depth)
		{
			if (openStack.back()) ImGui::TreePop();
			openStack.pop_back();
		}
		if (entry.depth > hiddenBelow) continue;
		hiddenBelow = INT_MAX;
		ImGui::PushID(entry.path.c_str());
		if (entry.folder)
		{
			const bool open = ImGui::TreeNodeEx(entry.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | (entry.depth == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0));
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) SelectAsset(entry.path);
			openStack.push_back(open);
			if (!open) hiddenBelow = entry.depth;
		}
		else
		{
			const std::string type = AssetManager::TypeOf(entry.path);
			ImGui::PushStyleColor(ImGuiCol_Text, TypeColor(type));
			ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
			const bool selected = mSelectedAsset == entry.path;
			if (ImGui::Selectable(entry.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				SelectAsset(entry.path);
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && type == "scene")
				{
					if (engine.ConfirmDiscardSceneChanges()) engine.OpenScene(root / fs::u8path(entry.path));
				}
			}
			ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
			ImGui::PopStyleColor();
			if (!type.empty()) AssetDragSource(entry.path);
			if (!type.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)%s", entry.path.c_str(), type.c_str(),
				type == "mesh" || type == "material" ? "\nDrag into the viewport or onto an asset field" : type == "scene" ? "\nDouble-click to open" : "");
		}
		ImGui::PopID();
	}
	while (!openStack.empty())
	{
		if (openStack.back()) ImGui::TreePop();
		openStack.pop_back();
	}
	ImGui::EndChild();
	ImGui::End();
}

void EditorLayer::DrawAssetInspector(Engine& engine)
{
	AssetManager& assets = engine.GetAssets();
	const std::string& path = mSelectedAsset;
	const std::string type = AssetManager::TypeOf(path);
	ImGui::TextUnformatted(path.c_str());
	ImGui::TextColored(TypeColor(type), "%s", type.empty() ? "folder / other file" : type.c_str());
	ImGui::Separator();

	if (type == "material")
	{
		const bool editable = path.find('#') == std::string::npos && path.rfind("engine/", 0) != 0;
		if (mMaterialEditPath != path)
		{
			mMaterialEdit = MaterialFile{};
			assets.ReadMaterial(path, mMaterialEdit);
			mMaterialEditPath = path;
			mMaterialChanged = false;
		}
		ImGui::BeginDisabled(!editable);
		bool changed = false;
		changed |= ImGui::ColorEdit4("baseColor", &mMaterialEdit.baseColor.x, ImGuiColorEditFlags_Float);
		changed |= ImGui::SliderFloat("roughness", &mMaterialEdit.roughness, 0.02f, 1.0f);
		changed |= ImGui::SliderFloat("metallic", &mMaterialEdit.metallic, 0.0f, 1.0f);
		if (ImGui::BeginCombo("albedoTexture", mMaterialEdit.albedoTexture.empty() ? "(none)" : mMaterialEdit.albedoTexture.c_str()))
		{
			if (ImGui::Selectable("(none)", mMaterialEdit.albedoTexture.empty())) { mMaterialEdit.albedoTexture.clear(); changed = true; }
			for (const std::string& texture : assets.List("texture"))
			{
				if (ImGui::Selectable(texture.c_str(), texture == mMaterialEdit.albedoTexture)) { mMaterialEdit.albedoTexture = texture; changed = true; }
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload))
			{
				const std::string dropped = static_cast<const char*>(payload->Data);
				if (AssetManager::TypeOf(dropped) == "texture") { mMaterialEdit.albedoTexture = dropped; changed = true; }
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::EndDisabled();

		if (changed)
		{
			assets.GetMaterial(path);   // make sure it is loaded, so the preview shows
			assets.PreviewMaterial(path, mMaterialEdit);
			mMaterialChanged = true;
		}
		// Write the file once the drag is over.
		if (mMaterialChanged && !ImGui::IsAnyItemActive())
		{
			if (!assets.WriteMaterial(path, mMaterialEdit)) CHERRY_ERROR("Couldn't save %s", path.c_str());
			mMaterialChanged = false;
		}
		if (!editable) ImGui::TextDisabled(path.find('#') != std::string::npos ? "Part of a glTF file: edit it in the modelling tool,\nor make a .mat material instead." : "Built-in material (read only).");
	}
	else if (type == "mesh")
	{
		const MeshHandle mesh = assets.GetMesh(path);
		if (const BoundingBox* bounds = engine.GetRenderer().GetMeshBounds(mesh))
		{
			ImGui::Text("Size: %.2f x %.2f x %.2f m", bounds->Extents.x * 2, bounds->Extents.y * 2, bounds->Extents.z * 2);
		}
		else
		{
			ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Can't load this mesh (see Output).");
		}
		if (engine.GetPlayState() == PlayState::Edit && ImGui::Button("Place in Scene"))
		{
			Transform view;
			view.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
			XMFLOAT3 position;
			XMStoreFloat3(&position, XMLoadFloat3(&mCamera.position) + view.Forward() * 6.0f);
			PlaceMesh(engine, path, position);
		}
		ImGui::TextDisabled("Or drag it into the viewport.");
	}
	else if (type == "scene")
	{
		if (ImGui::Button("Open Scene"))
		{
			if (engine.ConfirmDiscardSceneChanges()) engine.OpenScene(assets.FullPath(path));
		}
	}
	else if (type == "texture")
	{
		ImGui::TextDisabled("Assign it to a material's albedoTexture.");
	}
}

void EditorLayer::PlaceMesh(Engine& engine, const std::string& path, const XMFLOAT3& position)
{
	Scene& scene = engine.GetEditScene();
	const Entity entity = scene.CreateNamedEntity(fs::u8path(path).stem().u8string());
	scene.Get<Transform>(entity).position = position;
	MeshRenderer renderer;
	renderer.meshAsset = path;
	// A glTF model brings its own first material if it has one.
	const std::vector<std::string> materials = engine.GetAssets().List("material");
	const std::string own = path + "#0";
	renderer.materialAsset = std::find(materials.begin(), materials.end(), own) != materials.end() ? own : "engine/Default";
	scene.Add(entity, renderer);
	mSelected = entity;
	mSelectedAsset.clear();
	Edited(engine);
}

void EditorLayer::HandleViewportDrop(Engine& engine)
{
	if (engine.GetPlayState() != PlayState::Edit) return;
	const ImRect rect(ImVec2(viewportPosition.x, viewportPosition.y), ImVec2(viewportPosition.x + viewportSize.x, viewportPosition.y + viewportSize.y));
	if (!ImGui::BeginDragDropTargetCustom(rect, ImGui::GetID("##viewportDrop"))) return;
	const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload);
	if (payload)
	{
		const std::string path = static_cast<const char*>(payload->Data);
		const std::string type = AssetManager::TypeOf(path);
		const XMFLOAT2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
		Scene& scene = engine.GetEditScene();
		if (type == "mesh")
		{
			// On the surface under the mouse, else a few metres in front of the camera.
			const float x = (mouse.x - viewportPosition.x) / viewportSize.x * 2.0f - 1.0f;
			const float y = 1.0f - (mouse.y - viewportPosition.y) / viewportSize.y * 2.0f;
			const XMMATRIX inverse = XMMatrixInverse(nullptr, EditorViewProjection());
			const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(x, y, 0.0f, 1.0f), inverse);
			const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(x, y, 1.0f, 1.0f), inverse);
			Ray ray;
			ray.origin = mCamera.position;
			XMStoreFloat3(&ray.direction, XMVector3Normalize(farPoint - nearPoint));
			XMFLOAT3 position;
			if (const std::optional<RaycastHit> hit = engine.Raycast(ray)) position = hit->point;
			else XMStoreFloat3(&position, XMLoadFloat3(&ray.origin) + XMLoadFloat3(&ray.direction) * 6.0f);
			PlaceMesh(engine, path, position);
		}
		else if (type == "material")
		{
			const Entity target = PickEntity(engine, mouse);
			if (target != INVALID_ENTITY && scene.Has<MeshRenderer>(target))
			{
				MeshRenderer& renderer = scene.Get<MeshRenderer>(target);
				renderer.materialAsset = path;
				renderer.material = {};
				mSelected = target;
				mSelectedAsset.clear();
				Edited(engine);
			}
		}
		else if (type == "scene")
		{
			if (engine.ConfirmDiscardSceneChanges()) engine.OpenScene(engine.GetAssets().FullPath(path));
		}
	}
	ImGui::EndDragDropTarget();
}
