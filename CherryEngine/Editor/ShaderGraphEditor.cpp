// The shader graph editor window (see ShaderGraphEditor.h).
#include "Editor/ShaderGraphEditor.h"
#include "Core/Engine.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "imgui_internal.h"
#include "imgui_node_editor.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ed = ax::NodeEditor;
namespace fs = std::filesystem;
using namespace sg;

namespace
{
	const char* kWindowId = "###ShaderGraph";
	const char* kAssetPayload = "CHERRY_ASSET";   // as in ContentBrowser.cpp
	const char* kPreviewMeshes[] = { "engine/Sphere", "engine/Cube", "engine/Plane", "engine/Cylinder" };
	const char* kPreviewMeshNames[] = { "Sphere", "Cube", "Plane", "Cylinder" };

	// Ids: nodes by their id; pins (node << 8 | output << 7 | index); links by the input they end
	// at (an input has one link at most).
	ed::NodeId NodeKey(uint32_t node) { return ed::NodeId(static_cast<uintptr_t>(node)); }
	ed::PinId PinKey(uint32_t node, uint32_t index, bool output)
	{
		return ed::PinId((static_cast<uintptr_t>(node) << 8) | (output ? 0x80u : 0u) | (index & 0x7Fu));
	}
	ed::LinkId LinkKey(const Link& link) { return ed::LinkId((static_cast<uintptr_t>(link.toNode) << 8) | (link.toPin & 0xFFu)); }

	struct PinRef
	{
		uint32_t node = 0;
		uint32_t index = 0;
		bool output = false;
	};
	PinRef DecodePin(uint64_t value)
	{
		PinRef pin;
		pin.node = static_cast<uint32_t>(value >> 8);
		pin.output = (value & 0x80u) != 0;
		pin.index = static_cast<uint32_t>(value & 0x7Fu);
		return pin;
	}

	ImVec4 CategoryColor(const std::string& category)
	{
		if (category == "Output") return ImVec4(1.00f, 0.55f, 0.35f, 1);
		if (category == "Inputs") return ImVec4(0.55f, 0.85f, 1.00f, 1);
		if (category == "Constants") return ImVec4(0.75f, 0.95f, 0.55f, 1);
		if (category == "Parameters") return ImVec4(0.55f, 1.00f, 0.75f, 1);
		if (category == "Math") return ImVec4(0.85f, 0.85f, 0.90f, 1);
		if (category == "Vector") return ImVec4(0.95f, 0.80f, 0.45f, 1);
		if (category == "Utility") return ImVec4(0.90f, 0.65f, 1.00f, 1);
		if (category == "Functions") return ImVec4(1.00f, 0.75f, 0.85f, 1);
		return ImVec4(1.0f, 0.4f, 0.4f, 1);
	}

	ImU32 PinColor(PinType type)
	{
		switch (type)
		{
		case PinType::Float1: return IM_COL32(150, 220, 150, 255);
		case PinType::Float2: return IM_COL32(120, 200, 255, 255);
		case PinType::Float3: return IM_COL32(255, 220, 90, 255);
		case PinType::Float4: return IM_COL32(255, 130, 200, 255);
		default: return IM_COL32(220, 220, 220, 255);
		}
	}

	const char* TypeName(PinType type)
	{
		switch (type)
		{
		case PinType::Float1: return "float";
		case PinType::Float2: return "float2";
		case PinType::Float3: return "float3";
		case PinType::Float4: return "float4";
		case PinType::Dynamic: return "float1..4 (matches the other inputs)";
		default: return "any width";
		}
	}

	// A short label for an unconnected input that uses an expression instead of a constant.
	const char* DefaultExprLabel(const char* expr)
	{
		const std::string e = expr;
		if (e == "input.uv") return "mesh UV";
		if (e == "input.normalW") return "mesh normal";
		if (e == "input.positionW") return "world position";
		if (e == "input.time") return "time";
		return "default";
	}

	void PinIcon(PinType type, bool connected)
	{
		const float size = ImGui::GetTextLineHeight();
		ImGui::Dummy(ImVec2(size * 0.8f, size));
		const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (connected) draw->AddCircleFilled(center, size * 0.27f, PinColor(type));
		else draw->AddCircle(center, size * 0.27f, PinColor(type), 0, 1.5f);
	}

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	// InputText on a std::string.
	bool InputString(const char* label, std::string& text, bool multiline = false, const ImVec2& size = ImVec2(0, 0))
	{
		auto resize = [](ImGuiInputTextCallbackData* data) -> int
		{
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
			{
				std::string* string = static_cast<std::string*>(data->UserData);
				string->resize(static_cast<size_t>(data->BufTextLen));
				data->Buf = string->data();
			}
			return 0;
		};
		const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | (multiline ? ImGuiInputTextFlags_AllowTabInput : 0);
		return multiline
			? ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, flags, resize, &text)
			: ImGui::InputText(label, text.data(), text.capacity() + 1, flags, resize, &text);
	}

	// An asset picker: combo of `paths` plus "(none)", and a drop target for dragged assets of `type`.
	bool AssetCombo(const char* label, std::string& value, const std::vector<std::string>& paths, const std::string& type, const std::string& exclude = {})
	{
		bool changed = false;
		if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
		{
			if (ImGui::Selectable("(none)", value.empty())) { value.clear(); changed = true; }
			for (const std::string& path : paths)
			{
				if (path == exclude) continue;
				if (ImGui::Selectable(path.c_str(), path == value)) { value = path; changed = true; }
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload))
			{
				const std::string dropped = static_cast<const char*>(payload->Data);
				if (AssetManager::TypeOf(dropped) == type && dropped != exclude) { value = dropped; changed = true; }
			}
			ImGui::EndDragDropTarget();
		}
		return changed;
	}

	// The editor for an input's constant (or a node value) of `width` components.
	bool ConstantEdit(const char* label, Float4& value, int width, bool color, float itemWidth)
	{
		if (color && width >= 3)
		{
			ImGui::SetNextItemWidth(itemWidth);
			const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
			return width == 3 ? ImGui::ColorEdit3(label, value.data(), flags) : ImGui::ColorEdit4(label, value.data(), flags);
		}
		ImGui::SetNextItemWidth(itemWidth);
		switch (width)
		{
		case 2: return ImGui::DragFloat2(label, value.data(), 0.01f, 0, 0, "%.3g");
		case 3: return ImGui::DragFloat3(label, value.data(), 0.01f, 0, 0, "%.3g");
		case 4: return ImGui::DragFloat4(label, value.data(), 0.01f, 0, 0, "%.3g");
		default: return ImGui::DragFloat(label, value.data(), 0.01f, 0, 0, "%.3g");
		}
	}

	bool IsColorInput(const std::string& name) { return name.find("Color") != std::string::npos || name == "Emissive"; }
}

// =============================================================================
// Open / close / save
// =============================================================================

ShaderGraphEditor::~ShaderGraphEditor()
{
	if (mContext && ImGui::GetCurrentContext()) ed::DestroyEditor(mContext);
}

Graph ShaderGraphEditor::DefaultGraph()
{
	Graph graph;
	const GraphLoader none;
	Node output = MakeNode("Output", graph.nextId++, &none);
	output.x = 360.0f;
	Node color = MakeNode("ColorParameter", graph.nextId++, &none);
	color.name = "Base Color";
	color.value = { 0.8f, 0.8f, 0.8f, 1.0f };
	Node roughness = MakeNode("ScalarParameter", graph.nextId++, &none);
	roughness.name = "Roughness";
	roughness.value = { 0.5f, 0, 0, 0 };
	roughness.y = 110.0f;
	graph.nodes = { output, color, roughness };
	graph.Connect(color.id, 1, output.id, 0);       // RGB -> Base Color
	graph.Connect(roughness.id, 0, output.id, 2);   // -> Roughness
	return graph;
}

void ShaderGraphEditor::Open(Engine& engine, const std::string& path)
{
	if (path == mPath)
	{
		mFocusWindow = true;
		return;
	}
	if (mDirty)
	{
		RequestClose(path);
		return;
	}
	if (!mPath.empty()) Close(engine, false);
	Load(engine, path);
}

void ShaderGraphEditor::Load(Engine& engine, const std::string& path)
{
	AssetManager& assets = engine.GetAssets();
	mGraph = Graph{};
	if (!assets.ReadShaderGraph(path, mGraph))
	{
		CHERRY_ERROR("Can't open shader graph %s", path.c_str());
		return;
	}
	const GraphLoader loader = assets.GraphLoader();
	for (Node& node : mGraph.nodes) FitInputs(node, ResolvePins(node, &loader));

	ed::Config config;
	config.SettingsName = nullptr;   // node positions live in the graph file
	config.SettingsContent = ed::SettingsContentFlags::None;
	mContext = ed::CreateEditor(&config);
	mPath = path;
	mPlaced.clear();
	mNavigateIn = 2;
	mFocusWindow = true;
	mDirty = mPending = mMoved = false;
	mUndo.clear();
	mRedo.clear();
	mBaseline = GraphToJson(mGraph);
	Recompile(engine);
}

void ShaderGraphEditor::Close(Engine& engine, bool revert)
{
	if (revert) engine.GetAssets().RevertShaderGraph(mPath);
	if (mContext) ed::DestroyEditor(mContext);
	mContext = nullptr;
	mPath.clear();
	mGraph = Graph{};
	mDirty = mPending = false;
	mCompiled = CompiledGraph{};
	mNodeErrors.clear();
}

void ShaderGraphEditor::RequestClose(const std::string& thenOpen)
{
	mPrompt = true;
	mPromptThenOpen = thenOpen;
}

void ShaderGraphEditor::Shutdown(Engine& engine)
{
	if (!mPath.empty()) Close(engine, false);
}

bool ShaderGraphEditor::Save(Engine& engine)
{
	CommitIfIdle();
	if (!engine.GetAssets().WriteShaderGraph(mPath, mGraph))
	{
		CHERRY_ERROR("Couldn't save %s", mPath.c_str());
		return false;
	}
	mDirty = false;
	CHERRY_LOG("Saved %s", mPath.c_str());
	return true;
}

// =============================================================================
// Edits and undo
// =============================================================================

void ShaderGraphEditor::Edited(bool recompile)
{
	mPending = true;
	mDirty = true;
	if (recompile) mRecompileIn = 0.2f;
}

void ShaderGraphEditor::CommitIfIdle()
{
	if (!mPending || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
	const std::string json = GraphToJson(mGraph);
	if (json != mBaseline)
	{
		mUndo.push_back(mBaseline);
		mRedo.clear();
		mBaseline = json;
	}
	mPending = false;
}

bool ShaderGraphEditor::Undo()
{
	if (mUndo.empty()) return false;
	mRedo.push_back(mBaseline);
	mBaseline = mUndo.back();
	mUndo.pop_back();
	return true;
}

bool ShaderGraphEditor::Redo()
{
	if (mRedo.empty()) return false;
	mUndo.push_back(mBaseline);
	mBaseline = mRedo.back();
	mRedo.pop_back();
	return true;
}

void ShaderGraphEditor::Restore(Engine& engine, const std::string& json)
{
	std::string error;
	Graph graph;
	if (!GraphFromJson(json, graph, error)) return;
	const GraphLoader loader = engine.GetAssets().GraphLoader();
	for (Node& node : graph.nodes) FitInputs(node, ResolvePins(node, &loader));
	mGraph = std::move(graph);
	mPlaced.clear();   // positions are given to the canvas again
	mDirty = true;
	mPending = false;
	mRecompileIn = 0.0f;
}

void ShaderGraphEditor::Recompile(Engine& engine)
{
	mRecompileIn = -1.0f;
	mCompiled = engine.GetAssets().PreviewShaderGraph(mPath, mGraph);
	mNodeErrors.clear();
	for (const GraphError& error : mCompiled.errors)
	{
		std::string& text = mNodeErrors[error.node];
		text += (text.empty() ? "" : "\n") + error.message;
	}
}

// =============================================================================
// Graph helpers
// =============================================================================

uint32_t ShaderGraphEditor::AddNode(Engine& engine, const std::string& type, const ImVec2& position)
{
	const GraphLoader loader = engine.GetAssets().GraphLoader();
	Node node = MakeNode(type, mGraph.nextId++, &loader);
	node.x = position.x;
	node.y = position.y;
	const uint32_t id = node.id;
	const NodePins pins = ResolvePins(node, &loader);
	mGraph.nodes.push_back(std::move(node));

	// Created by dragging a link out of a pin: connect it.
	if (mCreateFromPin)
	{
		const PinRef from = DecodePin(mCreateFromPin);
		if (from.output && !pins.inputs.empty()) mGraph.Connect(from.node, from.index, id, 0);
		else if (!from.output && !pins.outputs.empty()) mGraph.Connect(id, 0, from.node, from.index);
		mCreateFromPin = 0;
	}
	Edited();
	return id;
}

void ShaderGraphEditor::Duplicate(Engine& engine, const std::vector<uint32_t>& nodes)
{
	(void)engine;
	std::map<uint32_t, uint32_t> copies;
	for (uint32_t id : nodes)
	{
		const Node* source = mGraph.Find(id);
		if (!source) continue;
		Node copy = *source;
		copy.id = mGraph.nextId++;
		copy.x += 40.0f;
		copy.y += 40.0f;
		copies[id] = copy.id;
		mGraph.nodes.push_back(copy);
	}
	const std::vector<Link> links = mGraph.links;
	for (const Link& link : links)
	{
		auto from = copies.find(link.fromNode);
		auto to = copies.find(link.toNode);
		if (from != copies.end() && to != copies.end()) mGraph.Connect(from->second, link.fromPin, to->second, link.toPin);
	}
	ed::SetCurrentEditor(mContext);
	ed::ClearSelection();
	for (const auto& [from, to] : copies) ed::SelectNode(NodeKey(to), true);
	if (!copies.empty()) Edited();
}

bool ShaderGraphEditor::WouldLoop(uint32_t from, uint32_t to) const
{
	// A link from -> to closes a loop if `from` can already be reached from `to`.
	std::vector<uint32_t> stack{ to };
	std::set<uint32_t> seen;
	while (!stack.empty())
	{
		const uint32_t node = stack.back();
		stack.pop_back();
		if (node == from) return true;
		if (!seen.insert(node).second) continue;
		for (const Link& link : mGraph.links) if (link.fromNode == node) stack.push_back(link.toNode);
	}
	return false;
}

std::vector<uint32_t> ShaderGraphEditor::SelectedNodes() const
{
	std::vector<uint32_t> result;
	if (!mContext) return result;
	ed::SetCurrentEditor(mContext);
	const int count = ed::GetSelectedObjectCount();
	if (count <= 0) return result;
	std::vector<ed::NodeId> ids(static_cast<size_t>(count));
	const int nodes = ed::GetSelectedNodes(ids.data(), count);
	for (int i = 0; i < nodes; ++i) result.push_back(static_cast<uint32_t>(ids[i].Get()));
	return result;
}

// =============================================================================
// Drawing
// =============================================================================

void ShaderGraphEditor::Draw(Engine& engine)
{
	mFocused = false;
	if (!mDeferredOpen.empty())
	{
		const std::string path = mDeferredOpen;
		mDeferredOpen.clear();
		Open(engine, path);
	}
	if (mPath.empty())
	{
		DrawSavePrompt(engine);
		return;
	}

	if (mRecompileIn >= 0.0f)
	{
		mRecompileIn -= ImGui::GetIO().DeltaTime;
		if (mRecompileIn < 0.0f) Recompile(engine);
	}

	const std::string title = fs::u8path(mPath).filename().u8string() + (mDirty ? " *" : "") + kWindowId;
	if (ImGuiWindow* viewport = ImGui::FindWindowByName("Viewport"))
	{
		if (viewport->DockId) ImGui::SetNextWindowDockID(viewport->DockId, ImGuiCond_FirstUseEver);
	}
	ImGui::SetNextWindowSize(ImVec2(1100, 650), ImGuiCond_FirstUseEver);
	if (mFocusWindow)
	{
		ImGui::SetNextWindowFocus();
		mFocusWindow = false;
	}
	bool open = true;
	if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		mFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		DrawToolbar(engine);
		if (ImGui::BeginTable("##ShaderGraphLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail()))
		{
			ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 320.0f);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			DrawCanvas(engine);
			ImGui::TableNextColumn();
			if (ImGui::BeginChild("##ShaderGraphDetails", ImVec2(0, ImGui::GetContentRegionAvail().y)))
			{
				DrawDetails(engine);
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
		HandleShortcuts(engine);
	}
	ImGui::End();
	CommitIfIdle();

	if (!open)
	{
		if (mDirty) RequestClose({});
		else Close(engine, true);
	}
	DrawSavePrompt(engine);
}

void ShaderGraphEditor::DrawToolbar(Engine& engine)
{
	if (ImGui::Button("Save")) Save(engine);
	ImGui::SetItemTooltip("Write %s (Ctrl+S)", mPath.c_str());
	ImGui::SameLine();
	ImGui::BeginDisabled(mUndo.empty() && !mPending);
	if (ImGui::Button("Undo")) { CommitIfIdle(); if (Undo()) Restore(engine, mBaseline); }
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(mRedo.empty());
	if (ImGui::Button("Redo") && Redo()) Restore(engine, mBaseline);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Fit")) mNavigateIn = 1;
	ImGui::SetItemTooltip("Show the whole graph");

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	if (mGraph.IsFunction())
	{
		ImGui::TextColored(CategoryColor("Functions"), "Function (use it with a Subgraph node)");
	}
	else
	{
		int blend = static_cast<int>(mGraph.blend);
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::Combo("Blend", &blend, "Opaque\0Masked\0Translucent\0"))
		{
			mGraph.blend = static_cast<BlendMode>(blend);
			Edited();
		}
		ImGui::SetItemTooltip("Opaque: solid. Masked: pixels with Opacity Mask below 1/3 are cut out.\n"
			"Translucent: blended by Opacity, drawn after solid surfaces (no depth write).");
	}

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	const std::string& dxcError = engine.GetRenderer().SurfaceShaderError(engine.GetAssets().GetSurfaceShader(mPath));
	if (mRecompileIn >= 0.0f) ImGui::TextDisabled("Compiling...");
	else if (!mCompiled.errors.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%zu error%s", mCompiled.errors.size(), mCompiled.errors.size() == 1 ? "" : "s");
	else if (!dxcError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "HLSL error (see Details)");
	else ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "Compiled");
}

void ShaderGraphEditor::DrawCanvas(Engine& engine)
{
	AssetManager& assets = engine.GetAssets();
	const GraphLoader loader = assets.GraphLoader();
	ed::SetCurrentEditor(mContext);
	ed::Begin("##ShaderGraphCanvas", ImVec2(0, ImGui::GetContentRegionAvail().y));

	// ---- Nodes -------------------------------------------------------------------------
	for (Node& node : mGraph.nodes)
	{
		if (mPlaced.insert(node.id).second) ed::SetNodePosition(NodeKey(node.id), ImVec2(node.x, node.y));
		DrawNode(engine, node, ResolvePins(node, &loader));
	}

	// ---- Links (coloured by the output's type) -------------------------------------------
	for (const Link& link : mGraph.links)
	{
		ImU32 color = IM_COL32(220, 220, 220, 255);
		if (const Node* from = mGraph.Find(link.fromNode))
		{
			const NodePins pins = ResolvePins(*from, &loader);
			if (link.fromPin < pins.outputs.size()) color = PinColor(pins.outputs[link.fromPin].type);
		}
		ed::Link(LinkKey(link), PinKey(link.fromNode, link.fromPin, true), PinKey(link.toNode, link.toPin, false),
			ImGui::ColorConvertU32ToFloat4(color), 2.0f);
	}

	// ---- New links / nodes dragged out of pins -------------------------------------------------
	if (ed::BeginCreate(ImVec4(1, 1, 1, 1), 2.0f))
	{
		ed::PinId a, b;
		if (ed::QueryNewLink(&a, &b) && a && b)
		{
			const PinRef pa = DecodePin(a.Get()), pb = DecodePin(b.Get());
			const PinRef& out = pa.output ? pa : pb;
			const PinRef& in = pa.output ? pb : pa;
			if (pa.output == pb.output || pa.node == pb.node || WouldLoop(out.node, in.node))
			{
				ed::RejectNewItem(ImVec4(1, 0.3f, 0.3f, 1), 2.0f);
			}
			else if (ed::AcceptNewItem(ImVec4(0.5f, 1, 0.5f, 1), 3.0f))
			{
				mGraph.Connect(out.node, out.index, in.node, in.index);
				Edited();
			}
		}
		ed::PinId pin;
		if (ed::QueryNewNode(&pin) && pin)
		{
			if (ed::AcceptNewItem())
			{
				mCreateFromPin = pin.Get();
				mCreatePosition = ImGui::GetMousePos();   // canvas coordinates here
				ed::Suspend();
				ImGui::OpenPopup("Create Node");
				ed::Resume();
			}
		}
		ed::EndCreate();
	}

	// ---- Deleted links / nodes (Delete key, context menu) -------------------------------------
	if (ed::BeginDelete())
	{
		ed::LinkId linkId;
		while (ed::QueryDeletedLink(&linkId))
		{
			if (ed::AcceptDeletedItem())
			{
				const uintptr_t key = linkId.Get();
				mGraph.Disconnect(static_cast<uint32_t>(key >> 8), static_cast<uint32_t>(key & 0xFFu));
				Edited();
			}
		}
		ed::NodeId nodeId;
		while (ed::QueryDeletedNode(&nodeId))
		{
			if (ed::AcceptDeletedItem())
			{
				mGraph.RemoveNode(static_cast<uint32_t>(nodeId.Get()));
				mPlaced.erase(static_cast<uint32_t>(nodeId.Get()));
				Edited();
			}
		}
		ed::EndDelete();
	}

	// ---- Positions back into the graph (an undo step when the drag ends) ---------------------
	for (Node& node : mGraph.nodes)
	{
		const ImVec2 position = ed::GetNodePosition(NodeKey(node.id));
		if (!std::isfinite(position.x) || !std::isfinite(position.y) || std::fabs(position.x) > 1e8f) continue;
		if (std::fabs(position.x - node.x) > 0.5f || std::fabs(position.y - node.y) > 0.5f)
		{
			node.x = position.x;
			node.y = position.y;
			mMoved = true;
		}
	}
	if (mMoved && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		mMoved = false;
		Edited(mGraph.IsFunction());   // a function's pin order follows node height
	}

	// ---- Context menus ---------------------------------------------------------------------
	const ImVec2 mouse = ImGui::GetMousePos();   // canvas coordinates
	ed::Suspend();
	ed::NodeId contextNode;
	if (ed::ShowNodeContextMenu(&contextNode))
	{
		mContextNode = static_cast<uint32_t>(contextNode.Get());
		ImGui::OpenPopup("Node Menu");
	}
	else if (ed::ShowBackgroundContextMenu())
	{
		mCreatePosition = mouse;
		mCreateFromPin = 0;
		ImGui::OpenPopup("Create Node");
	}
	if (ImGui::BeginPopup("Node Menu"))
	{
		const Node* node = mGraph.Find(mContextNode);
		const NodeDef* def = node ? FindNodeDef(node->type) : nullptr;
		ImGui::TextDisabled("%s", def ? def->title.c_str() : "Node");
		ImGui::Separator();
		if (ImGui::MenuItem("Duplicate", "Ctrl+D")) Duplicate(engine, { mContextNode });
		if (ImGui::MenuItem("Disconnect all"))
		{
			mGraph.links.erase(std::remove_if(mGraph.links.begin(), mGraph.links.end(), [&](const Link& link)
			{
				return link.fromNode == mContextNode || link.toNode == mContextNode;
			}), mGraph.links.end());
			Edited();
		}
		if (node && node->type == "Subgraph" && !node->asset.empty() && ImGui::MenuItem("Open subgraph")) mDeferredOpen = node->asset;
		if (ImGui::MenuItem("Delete", "Del")) ed::DeleteNode(NodeKey(mContextNode));
		ImGui::EndPopup();
	}
	DrawCreatePopup(engine);
	ed::Resume();

	if (mNavigateIn > 0 && --mNavigateIn == 0) ed::NavigateToContent(0.0f);
	ed::End();
}

void ShaderGraphEditor::DrawNode(Engine& engine, Node& node, const NodePins& pins)
{
	(void)engine;
	const NodeDef* def = FindNodeDef(node.type);
	auto error = mNodeErrors.find(node.id);
	const bool hasError = error != mNodeErrors.end();
	if (hasError)
	{
		ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
		ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 2.5f);
	}
	ed::BeginNode(NodeKey(node.id));
	ImGui::PushID(static_cast<int>(node.id));

	// Title and a summary of the settings.
	ImGui::TextColored(CategoryColor(def ? def->category : ""), "%s", def ? def->title.c_str() : (node.type + " (unknown)").c_str());
	if (def)
	{
		if (def->settings & Setting_Name) ImGui::TextDisabled("%s", node.name.empty() ? "(no name)" : node.name.c_str());
		if (def->settings & (Setting_Texture | Setting_Subgraph))
		{
			ImGui::TextDisabled("%s", node.asset.empty() ? "(none)" : fs::u8path(node.asset).filename().u8string().c_str());
		}
		if (def->settings & Setting_Text) ImGui::TextDisabled(".%s", node.text.c_str());
		if (def->settings & Setting_Code)
		{
			std::string firstLine = node.text.substr(0, node.text.find('\n'));
			if (firstLine.size() > 28) firstLine = firstLine.substr(0, 28) + "...";
			ImGui::TextDisabled("%s", firstLine.c_str());
		}
		if ((def->settings & Setting_Value) && !(def->settings & Setting_Name) && node.type != "FunctionInput")
		{
			if (def->valueEdit == ValueEdit::Color)
			{
				ImGui::ColorButton("##value", ImVec4(node.value[0], node.value[1], node.value[2], node.value[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(60, 14));
			}
			else
			{
				const int width = static_cast<int>(def->valueEdit);
				std::string text;
				for (int i = 0; i < width; ++i)
				{
					char buffer[24];
					std::snprintf(buffer, sizeof(buffer), "%s%.3g", i ? ", " : "", node.value[i]);
					text += buffer;
				}
				ImGui::TextUnformatted(text.c_str());
			}
		}
	}
	ImGui::Dummy(ImVec2(0, 2));

	// Inputs, with their constants while unconnected.
	ImGui::BeginGroup();
	for (uint32_t i = 0; i < pins.inputs.size(); ++i)
	{
		const PinInfo& pin = pins.inputs[i];
		const bool connected = mGraph.InputLink(node.id, i) != nullptr;
		ed::BeginPin(PinKey(node.id, i, false), ed::PinKind::Input);
		ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
		PinIcon(pin.type, connected);
		ed::EndPin();
		ImGui::SameLine();
		ImGui::TextUnformatted(pin.name.c_str());
		if (connected) continue;
		ImGui::SameLine();
		if (pin.defaultExpr)
		{
			ImGui::TextDisabled("(%s)", DefaultExprLabel(pin.defaultExpr));
		}
		else if (i < node.inputs.size())
		{
			const int width = std::max(PinWidth(pin.type), 1);
			char label[16];
			std::snprintf(label, sizeof(label), "##in%u", i);
			if (ConstantEdit(label, node.inputs[i], width, false, 46.0f * width)) Edited();
		}
	}
	ImGui::EndGroup();

	// Outputs, right-aligned.
	if (!pins.outputs.empty())
	{
		ImGui::SameLine(0, 18);
		ImGui::BeginGroup();
		float widest = 0.0f;
		for (const PinInfo& pin : pins.outputs) widest = std::max(widest, ImGui::CalcTextSize(pin.name.c_str()).x);
		for (uint32_t i = 0; i < pins.outputs.size(); ++i)
		{
			const PinInfo& pin = pins.outputs[i];
			bool connected = false;
			for (const Link& link : mGraph.links) connected |= link.fromNode == node.id && link.fromPin == i;
			ImGui::Dummy(ImVec2(widest - ImGui::CalcTextSize(pin.name.c_str()).x, 0));
			ImGui::SameLine(0, 0);
			ImGui::TextUnformatted(pin.name.c_str());
			ImGui::SameLine();
			ed::BeginPin(PinKey(node.id, i, true), ed::PinKind::Output);
			ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
			PinIcon(pin.type, connected);
			ed::EndPin();
		}
		ImGui::EndGroup();
	}

	ImGui::PopID();
	ed::EndNode();
	if (hasError)
	{
		ed::PopStyleVar();
		ed::PopStyleColor();
	}

	// The error as a tooltip.
	if (hasError && ed::GetHoveredNode() == NodeKey(node.id))
	{
		ed::Suspend();
		ImGui::SetTooltip("%s", error->second.c_str());
		ed::Resume();
	}
}

void ShaderGraphEditor::DrawCreatePopup(Engine& engine)
{
	if (!ImGui::BeginPopup("Create Node")) return;
	if (ImGui::IsWindowAppearing())
	{
		mSearch[0] = '\0';
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::SetNextItemWidth(220.0f);
	const bool enter = ImGui::InputTextWithHint("##search", "Search nodes...", mSearch, sizeof(mSearch), ImGuiInputTextFlags_EnterReturnsTrue);
	const std::string filter = Lower(mSearch);

	std::string chosen;
	if (filter.empty())
	{
		std::vector<std::string> categories;
		for (const NodeDef& def : NodeDefs())
		{
			if (std::find(categories.begin(), categories.end(), def.category) == categories.end()) categories.push_back(def.category);
		}
		for (const std::string& category : categories)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(category));
			const bool open = ImGui::BeginMenu(category.c_str());
			ImGui::PopStyleColor();
			if (!open) continue;
			for (const NodeDef& def : NodeDefs())
			{
				if (def.category != category) continue;
				if (ImGui::MenuItem(def.title.c_str())) chosen = def.type;
				ImGui::SetItemTooltip("%s", def.description.c_str());
			}
			ImGui::EndMenu();
		}
	}
	else
	{
		bool first = true;
		for (const NodeDef& def : NodeDefs())
		{
			if (Lower(def.title).find(filter) == std::string::npos && Lower(def.category).find(filter) == std::string::npos) continue;
			if (ImGui::MenuItem(def.title.c_str(), def.category.c_str()) || (enter && first)) chosen = def.type;
			ImGui::SetItemTooltip("%s", def.description.c_str());
			first = false;
		}
		if (first) ImGui::TextDisabled("No node matches");
	}

	if (!chosen.empty())
	{
		ed::SetCurrentEditor(mContext);
		AddNode(engine, chosen, mCreatePosition);
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void ShaderGraphEditor::DrawDetails(Engine& engine)
{
	AssetManager& assets = engine.GetAssets();
	ImGui::TextUnformatted(mPath.c_str());

	// ---- Preview ---------------------------------------------------------------------
	if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) DrawPreview(engine);

	// ---- Selected node / graph settings ---------------------------------------------------
	const std::vector<uint32_t> selected = SelectedNodes();
	Node* node = selected.size() == 1 ? mGraph.Find(selected[0]) : nullptr;
	if (node)
	{
		const NodeDef* def = FindNodeDef(node->type);
		if (ImGui::CollapsingHeader(def ? def->title.c_str() : node->type.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			DrawNodeSettings(engine, *node);
		}
	}
	else if (ImGui::CollapsingHeader("Graph", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (!mGraph.IsFunction())
		{
			int blend = static_cast<int>(mGraph.blend);
			if (ImGui::Combo("Blend mode", &blend, "Opaque\0Masked\0Translucent\0"))
			{
				mGraph.blend = static_cast<BlendMode>(blend);
				Edited();
			}
			if (ImGui::DragFloat("Bounds padding", &mGraph.boundsPadding, 0.01f, 0.0f, 100.0f, "%.2f m")) Edited();
			ImGui::SetItemTooltip("Added to mesh bounds for culling: set it to how far World Position Offset moves vertices.");
		}
		else
		{
			std::vector<FunctionPin> inputs, outputs;
			FunctionSignature(mGraph, inputs, outputs);
			ImGui::TextWrapped("A function: other graphs use it through a Subgraph node. Its pins (top to bottom):");
			for (const FunctionPin& pin : inputs) ImGui::BulletText("in  %s (%s)", pin.name.c_str(), TypeName(static_cast<PinType>(pin.width)));
			for (const FunctionPin& pin : outputs) ImGui::BulletText("out %s (%s)", pin.name.c_str(), TypeName(static_cast<PinType>(pin.width)));
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Right-click the canvas (or drag a link into empty space) to add nodes. Select a node to edit its settings.");
		ImGui::PopStyleColor();
	}

	// ---- Errors -------------------------------------------------------------------------
	const std::string& dxcError = engine.GetRenderer().SurfaceShaderError(assets.GetSurfaceShader(mPath));
	if (!mCompiled.errors.empty() || !dxcError.empty())
	{
		if (ImGui::CollapsingHeader("Errors", ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (const GraphError& error : mCompiled.errors)
			{
				ImGui::PushID(&error);
				const Node* errorNode = mGraph.Find(error.node);
				const NodeDef* def = errorNode ? FindNodeDef(errorNode->type) : nullptr;
				if (errorNode)
				{
					if (ImGui::SmallButton("Show"))
					{
						ed::SetCurrentEditor(mContext);
						ed::SelectNode(NodeKey(error.node));
						ed::NavigateToSelection(false, 0.3f);
					}
					ImGui::SetItemTooltip("Select the node and go to it");
					ImGui::SameLine();
				}
				const std::string label = (def ? def->title + ": " : std::string()) + error.message;
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.5f, 1));
				ImGui::TextWrapped("%s", label.c_str());
				ImGui::PopStyleColor();
				ImGui::PopID();
			}
			if (mCompiled.errors.empty() && !dxcError.empty())
			{
				ImGui::TextColored(ImVec4(1, 0.55f, 0.5f, 1), "The generated HLSL doesn't compile (a Custom HLSL node?):");
				ImGui::TextWrapped("%s", dxcError.c_str());
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("Materials keep the last version that compiled.");
			ImGui::PopStyleColor();
		}
	}

	// ---- Parameters -----------------------------------------------------------------------
	if (!mCompiled.parameters.empty() && ImGui::CollapsingHeader("Parameters"))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Each material using this graph can set these.");
		ImGui::PopStyleColor();
		for (const GraphParameter& param : mCompiled.parameters)
		{
			const char* kinds[] = { "Scalar", "Vector", "Color", "Texture" };
			if (param.kind == ParamKind::Texture)
			{
				ImGui::BulletText("%s  (Texture, %s)", param.name.c_str(), param.defaultTexture.empty() ? "no default" : param.defaultTexture.c_str());
			}
			else
			{
				ImGui::BulletText("%s  (%s)", param.name.c_str(), kinds[static_cast<int>(param.kind)]);
			}
		}
	}

	// ---- Generated code ---------------------------------------------------------------------
	if (ImGui::CollapsingHeader("Generated HLSL"))
	{
		ImGui::InputTextMultiline("##hlsl", const_cast<char*>(mCompiled.hlsl.c_str()), mCompiled.hlsl.size() + 1,
			ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 20), ImGuiInputTextFlags_ReadOnly);
		ImGui::TextDisabled("Also in Saved/ShaderCache next to the project.");
	}
}

void ShaderGraphEditor::DrawNodeSettings(Engine& engine, Node& node)
{
	AssetManager& assets = engine.GetAssets();
	const NodeDef* def = FindNodeDef(node.type);
	if (!def)
	{
		ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Unknown node type \"%s\"", node.type.c_str());
		return;
	}
	ImGui::PushID(static_cast<int>(node.id));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", def->description.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	const uint32_t settings = def->settings;
	const GraphLoader loader = assets.GraphLoader();

	if (settings & Setting_Name)
	{
		if (InputString("Name", node.name)) Edited();
	}
	if (settings & Setting_Value)
	{
		const char* label = node.type == "FunctionInput" ? "Default" : "Value";
		bool changed = false;
		switch (def->valueEdit)
		{
		case ValueEdit::Color: changed = ImGui::ColorEdit4(label, node.value.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
		case ValueEdit::Float2: changed = ImGui::DragFloat2(label, node.value.data(), 0.01f); break;
		case ValueEdit::Float3: changed = ImGui::DragFloat3(label, node.value.data(), 0.01f); break;
		case ValueEdit::Float4: changed = ImGui::DragFloat4(label, node.value.data(), 0.01f); break;
		default: changed = ImGui::DragFloat(label, node.value.data(), 0.01f); break;
		}
		if (changed) Edited();
	}
	if (settings & Setting_Text)
	{
		if (InputString("Mask", node.text)) Edited();
	}
	if (settings & Setting_Code)
	{
		ImGui::TextDisabled("HLSL (inputs A..D, `input` is the SurfaceInput):");
		if (InputString("##code", node.text, true, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10))) Edited();
	}
	if (settings & Setting_Texture)
	{
		if (AssetCombo("Texture", node.asset, assets.List("texture"), "texture")) Edited();
		ImGui::SetItemTooltip("The default; each material can pick its own.");
	}
	if (settings & Setting_Sampler)
	{
		int sampler = std::clamp(node.option, 0, 5);
		if (ImGui::Combo("Sampler", &sampler, kSamplerNames, 6)) { node.option = sampler; Edited(); }
	}
	if (settings & Setting_TextureKind)
	{
		int kind = std::clamp(node.option2, 0, 2);
		if (ImGui::Combo("Kind", &kind, kTextureKindNames, 3)) { node.option2 = kind; Edited(); }
		ImGui::SetItemTooltip("Color: sRGB texture (albedo).\nNormal map: linear, RGB unpacked to -1..1; feed it to a Normal Map node.\nData: linear (masks, roughness).");
	}
	if (settings & Setting_Width)
	{
		int width = std::clamp(node.option, 1, 4);
		if (ImGui::SliderInt(node.type == "Custom" ? "Output width" : "Width", &width, 1, 4)) { node.option = width; Edited(); }
	}
	if (settings & Setting_Octaves)
	{
		int octaves = std::clamp(node.option, 1, 8);
		if (ImGui::SliderInt("Octaves", &octaves, 1, 8)) { node.option = octaves; Edited(); }
	}
	if (settings & Setting_Subgraph)
	{
		std::vector<std::string> functions;
		for (const std::string& path : assets.List("shadergraph"))
		{
			const Graph* graph = path == mPath ? nullptr : loader(path);
			if (graph && graph->IsFunction()) functions.push_back(path);
		}
		if (AssetCombo("Function", node.asset, functions, "shadergraph", mPath))
		{
			// New pins: drop links to pins that are gone.
			const NodePins pins = ResolvePins(node, &loader);
			FitInputs(node, pins);
			mGraph.links.erase(std::remove_if(mGraph.links.begin(), mGraph.links.end(), [&](const Link& link)
			{
				return (link.toNode == node.id && link.toPin >= pins.inputs.size()) || (link.fromNode == node.id && link.fromPin >= pins.outputs.size());
			}), mGraph.links.end());
			Edited();
		}
		if (!node.asset.empty() && ImGui::Button("Open function")) mDeferredOpen = node.asset;
	}

	// Constants of unconnected inputs (with colour pickers).
	const NodePins pins = ResolvePins(node, &loader);
	bool header = false;
	for (uint32_t i = 0; i < pins.inputs.size() && i < node.inputs.size(); ++i)
	{
		const PinInfo& pin = pins.inputs[i];
		if (pin.defaultExpr || mGraph.InputLink(node.id, i)) continue;
		if (!header)
		{
			ImGui::SeparatorText("Unconnected inputs");
			header = true;
		}
		const int width = std::max(PinWidth(pin.type), 1);
		if (ConstantEdit(pin.name.c_str(), node.inputs[i], width, IsColorInput(pin.name), ImGui::CalcItemWidth())) Edited();
		ImGui::SetItemTooltip("%s", TypeName(pin.type));
	}
	ImGui::PopID();
}

void ShaderGraphEditor::DrawPreview(Engine& engine)
{
	AssetManager& assets = engine.GetAssets();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::Combo("Mesh", &mPreviewMesh, kPreviewMeshNames, IM_ARRAYSIZE(kPreviewMeshNames));
	const float size = std::max(64.0f, std::min(ImGui::GetContentRegionAvail().x, 300.0f));
	const MaterialHandle material = assets.ShaderPreviewMaterial(mPath);
	const MeshHandle mesh = assets.GetMesh(kPreviewMeshes[std::clamp(mPreviewMesh, 0, 3)]);
	const float scale = ImGui::GetWindowDpiScale() > 0.0f ? ImGui::GetWindowDpiScale() : 1.0f;
	const unsigned long long texture = engine.GetRenderer().RenderPreview(mesh, material, static_cast<uint32_t>(size * scale), mPreviewYaw, mPreviewPitch);
	if (!texture)
	{
		ImGui::Dummy(ImVec2(size, size));
		return;
	}
	ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(size, size));
	// An Image can't be dragged by itself: an invisible button over it can.
	ImGui::SetCursorScreenPos(ImGui::GetItemRectMin());
	ImGui::InvisibleButton("##previewDrag", ImVec2(size, size));
	ImGui::SetItemTooltip("Drag to turn. Uses the graph's default parameters and the scene's sun and ambient.");
	if (ImGui::IsItemActive())
	{
		const ImVec2 delta = ImGui::GetIO().MouseDelta;
		mPreviewYaw -= delta.x * 0.01f;
		mPreviewPitch = std::clamp(mPreviewPitch + delta.y * 0.01f, -1.4f, 1.4f);
	}
}

void ShaderGraphEditor::DrawSavePrompt(Engine& engine)
{
	if (mPrompt)
	{
		ImGui::OpenPopup("Save shader graph?");
		mPrompt = false;
	}
	if (!ImGui::BeginPopupModal("Save shader graph?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::Text("%s has unsaved changes.", mPath.c_str());
	auto finish = [&](bool save)
	{
		if (save) Save(engine);
		const std::string next = mPromptThenOpen;
		Close(engine, !save);
		if (!next.empty()) Load(engine, next);
		ImGui::CloseCurrentPopup();
	};
	if (ImGui::Button("Save")) finish(true);
	ImGui::SameLine();
	if (ImGui::Button("Discard")) finish(false);
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void ShaderGraphEditor::HandleShortcuts(Engine& engine)
{
	const ImGuiIO& io = ImGui::GetIO();
	if (!mFocused || io.WantTextInput || !io.KeyCtrl) return;
	if (ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(engine);
	if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
	{
		CommitIfIdle();
		if (io.KeyShift ? Redo() : Undo()) Restore(engine, mBaseline);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && Redo()) Restore(engine, mBaseline);
	if (ImGui::IsKeyPressed(ImGuiKey_D, false)) Duplicate(engine, SelectedNodes());
}
