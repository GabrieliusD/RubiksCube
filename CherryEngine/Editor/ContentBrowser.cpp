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
#include <cctype>
#include <fstream>

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
		if (type == "prefab") return ImVec4(0.60f, 0.75f, 1.00f, 1);
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
		// Depth-first tree order with folders before files at every level: sort by a key of
		// one ("0" folder / "1" file, name) pair per path element.
		struct Item { fs::path file; std::vector<std::string> key; };
		std::vector<Item> items;
		for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
		{
			Item item{ it->path(), {} };
			const fs::path relative = item.file.lexically_relative(root);
			const bool isFolder = it->is_directory(ec);
			size_t index = 0;
			const size_t count = static_cast<size_t>(std::distance(relative.begin(), relative.end()));
			for (const fs::path& element : relative)
			{
				const bool elementIsFolder = ++index < count || isFolder;   // every element but the last is a folder
				item.key.push_back((elementIsFolder ? "0" : "1") + element.u8string());
			}
			items.push_back(std::move(item));
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
		std::vector<fs::path> files;
		for (Item& item : items) files.push_back(std::move(item.file));
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
	const ModuleStatus moduleStatus = engine.GetModuleStatus();
	ImGui::BeginDisabled(moduleStatus.isStatic);
	if (ImGui::SmallButton("New Script...")) mOpenNewScript = true;
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Create a C++ script class in %s/Scripts and compile it", engine.Project().root.filename().u8string().c_str());
	DrawNewScriptPopup(engine);
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
				type == "mesh" || type == "material" || type == "prefab" ? "\nDrag into the viewport or onto an asset field" : type == "scene" ? "\nDouble-click to open" : "");
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
	else if (type == "prefab")
	{
		if (engine.GetPlayState() == PlayState::Edit && ImGui::Button("Place in Scene"))
		{
			Transform view;
			view.SetRotationEuler(mCamera.pitch, mCamera.yaw, 0.0f);
			XMFLOAT3 position;
			XMStoreFloat3(&position, XMLoadFloat3(&mCamera.position) + view.Forward() * 6.0f);
			PlacePrefab(engine, path, position);
		}
		ImGui::TextDisabled("Or drag it into the viewport. Scripts spawn it with Spawn(\"%s\", position).", path.c_str());
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

void EditorLayer::PlacePrefab(Engine& engine, const std::string& path, const XMFLOAT3& position)
{
	const Entity root = engine.Spawn(path, position);
	if (root == INVALID_ENTITY) return;
	mSelected = root;
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
		if (type == "mesh" || type == "prefab")
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
			if (type == "prefab") PlacePrefab(engine, path, position);
			else PlaceMesh(engine, path, position);
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

// =============================================================================
// New Script
// =============================================================================

namespace
{
	bool IsIdentifier(const std::string& name)
	{
		if (name.empty() || name.size() > 60 || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
		return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
	}

	bool WriteText(const fs::path& file, const std::string& text)
	{
		std::ofstream stream(file, std::ios::binary | std::ios::trunc);
		stream << text;
		return static_cast<bool>(stream);
	}

	// <dir>/<name>.h and .cpp from the script template.
	bool CreateScriptFiles(const fs::path& dir, const std::string& name, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(dir, ec);
		const fs::path header = dir / (name + ".h");
		const fs::path source = dir / (name + ".cpp");
		if (fs::exists(header) || fs::exists(source))
		{
			error = name + ".h or " + name + ".cpp already exists in " + dir.u8string();
			return false;
		}
		const std::string h =
			"#pragma once\n"
			"#include <Game/Script.h>\n"
			"\n"
			"// " + name + ": describe what it does.\n"
			"class " + name + " : public Script\n"
			"{\n"
			"public:\n"
			"\t// Fields registered in " + name + ".cpp are edited in the inspector and saved with the scene.\n"
			"\tfloat speed = 1.0f;\n"
			"\n"
			"\tvoid OnStart() override;\n"
			"\tvoid OnUpdate(float deltaTime) override;\n"
			"\n"
			"private:\n"
			"\t// Runtime state: not saved, reset on every Play.\n"
			"\tfloat mTime = 0.0f;\n"
			"};\n";
		const std::string cpp =
			"#include \"" + name + ".h\"\n"
			"#include <Core/Engine.h>\n"
			"#include <Game/GameModule.h>   // CHERRY_SCRIPT\n"
			"\n"
			"// Registers the script when the game module loads (no line in RegisterTypes needed).\n"
			"CHERRY_SCRIPT(" + name + ", \"" + name + "\")\n"
			"{\n"
			"\tscript.Field(\"speed\", &" + name + "::speed, 0.0f, 10.0f);\n"
			"}\n"
			"\n"
			"void " + name + "::OnStart()\n"
			"{\n"
			"\t// Runs once, on the first frame of play (also for entities spawned later).\n"
			"}\n"
			"\n"
			"void " + name + "::OnUpdate(float deltaTime)\n"
			"{\n"
			"\tmTime += deltaTime;\n"
			"\t// Every frame while playing. Helpers: GetTransform(), Get<T>(), Has<T>(),\n"
			"\t// Spawn(\"Prefabs/x.prefab\", position), Destroy(), Find(\"name\"), GetScene(), GetEngine().\n"
			"}\n";
		if (!WriteText(header, h) || !WriteText(source, cpp))
		{
			error = "can't write the files in " + dir.u8string();
			return false;
		}
		return true;
	}
}

bool EditorLayer::CreateScript(Engine& engine, const std::string& name, bool openSource, std::string& error)
{
	if (!IsIdentifier(name))
	{
		error = "Use a C++ class name: letters, digits and _, not starting with a digit.";
		return false;
	}
	if (TypeRegistry::Get().Find(name))
	{
		error = "A component or script called " + name + " already exists.";
		return false;
	}
	const ModuleStatus status = engine.GetModuleStatus();
	if (status.isStatic || !engine.Project().IsValid())
	{
		error = "New scripts need a project with a game module to compile.";
		return false;
	}
	// The project folder is the game module's source folder (next to its CMakeLists.txt).
	const fs::path dir = engine.Project().root / "Scripts";
	if (!CreateScriptFiles(dir, name, error)) return false;

	CHERRY_LOG("Created script %s in %s; compiling", name.c_str(), dir.u8string().c_str());
	engine.CompileModule(true);   // configure picks up the new files (Scripts/ is globbed), then build and reload
	if (openSource) ShellExecuteW(nullptr, L"open", (dir / (name + ".cpp")).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return true;
}

void EditorLayer::DrawNewScriptPopup(Engine& engine)
{
	if (mOpenNewScript)
	{
		ImGui::OpenPopup("New Script");
		mOpenNewScript = false;
		mNewScriptError.clear();
	}
	if (!ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::TextUnformatted("Class name:");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	const bool enter = ImGui::InputText("##scriptName", mNewScriptName, sizeof(mNewScriptName), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::Checkbox("Open the .cpp file afterwards", &mNewScriptOpenSource);
	ImGui::TextDisabled("Writes Scripts/%s.h and .cpp in %s,\nthen compiles. It appears in Add Component when the module reloads.",
		mNewScriptName[0] ? mNewScriptName : "<Name>", engine.Project().root.filename().u8string().c_str());
	if (!mNewScriptError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", mNewScriptError.c_str());

	if (ImGui::Button("Create") || enter)
	{
		if (CreateScript(engine, mNewScriptName, mNewScriptOpenSource, mNewScriptError))
		{
			mNewScriptName[0] = '\0';
			mContentScanTimer = 0.0f;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
