// The UI inspector (Window "UI"): the game's RmlUi documents as a tree, a pick mode for the
// game view, and the selected element's box, classes and properties, each property with the
// style sheet and line it comes from. Values typed here are applied at once as inline styles
// (temporary: they go with a reload); to keep a change, open the source and edit the .rcss,
// which hot-reloads (UI/Ui.h). "Copy as RCSS" puts the element's properties on the clipboard.
#include "Editor/EditorLayer.h"
#include "Core/Engine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kUiWindow = "UI";

	// The debugger's own documents live in the game's context too; they aren't the game's.
	bool IsDebuggerDocument(Rml::Element* document)
	{
		return document->GetId().rfind("rmlui-debug", 0) == 0;
	}

	bool IsText(Rml::Element* element) { return element->GetTagName() == "#text"; }

	std::string Label(Rml::Element* element)
	{
		if (IsText(element))
		{
			std::string text = static_cast<Rml::ElementText*>(element)->GetText();
			if (text.size() > 32) text = text.substr(0, 32) + "...";
			return "\"" + text + "\"";
		}
		std::string label = element->GetTagName();
		if (!element->GetId().empty()) label += "#" + element->GetId();
		std::string classes = element->GetClassNames();
		std::replace(classes.begin(), classes.end(), ' ', '.');
		if (!classes.empty()) label += "." + classes;
		return label;
	}

	// The topmost visible element under `point` (view pixels), the deepest one there: children
	// are tried last to first, text nodes give their parent. Unlike Context::GetElementAtPoint
	// this also finds elements with pointer-events: none.
	Rml::Element* PickElement(Rml::Element* element, Rml::Vector2f point)
	{
		if (!element->IsVisible() || IsText(element)) return nullptr;
		for (int i = element->GetNumChildren() - 1; i >= 0; --i)
			if (Rml::Element* hit = PickElement(element->GetChild(i), point)) return hit;
		return element->IsPointWithinElement(point) ? element : nullptr;
	}

	Rml::Element* PickInContext(Rml::Context& context, Rml::Vector2f point)
	{
		for (int i = context.GetNumDocuments() - 1; i >= 0; --i)
		{
			Rml::ElementDocument* document = context.GetDocument(i);
			if (!document || IsDebuggerDocument(document)) continue;
			if (Rml::Element* hit = PickElement(document, point)) return hit;
		}
		return nullptr;
	}

	bool Contains(Rml::Element* root, Rml::Element* element)
	{
		if (root == element) return true;
		for (int i = 0; i < root->GetNumChildren(); ++i)
			if (Contains(root->GetChild(i), element)) return true;
		return false;
	}

	// The element is still in one of the context's documents (a reload or a data binding may
	// have destroyed it).
	bool InContext(Rml::Context& context, Rml::Element* element)
	{
		if (!element) return false;
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::ElementDocument* document = context.GetDocument(i); document && Contains(document, element)) return true;
		return false;
	}

	bool IsAncestorOf(Rml::Element* ancestor, Rml::Element* element)
	{
		for (Rml::Element* e = element ? element->GetParentNode() : nullptr; e; e = e->GetParentNode())
			if (e == ancestor) return true;
		return false;
	}

	// Opens a style sheet at a line: VS Code if it's installed ("code -g file:line"), otherwise
	// whatever opens the file.
	void OpenSource(const fs::path& file, int line)
	{
#if defined(_WIN32)
		const std::wstring arguments = L"-g \"" + file.wstring() + L":" + std::to_wstring(std::max(line, 1)) + L"\"";
		const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"code", arguments.c_str(), nullptr, SW_HIDE));
		if (result <= 32) ShellExecuteW(nullptr, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		(void)file;
		(void)line;
#endif
	}

	void DrawBox(ImDrawList* draw, const DirectX::XMFLOAT2& origin, Rml::Element* element, ImU32 color, float thickness)
	{
		const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
		const ImVec2 min(origin.x + offset.x, origin.y + offset.y);
		draw->AddRect(min, ImVec2(min.x + size.x, min.y + size.y), color, 0.0f, 0, thickness);
	}
}

void EditorLayer::DrawUiTreeNode(Rml::Element* element, Rml::Element* selected, Rml::Element*& hovered)
{
	const int children = element->GetNumChildren();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (children == 0) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (element == selected) flags |= ImGuiTreeNodeFlags_Selected;
	if (mUiReveal && IsAncestorOf(element, selected)) ImGui::SetNextItemOpen(true);
	const bool hidden = !element->IsVisible();
	if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	const bool open = ImGui::TreeNodeEx(element, flags, "%s", Label(element).c_str());
	if (hidden) ImGui::PopStyleColor();
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) mUiSelected = element;
	if (ImGui::IsItemHovered()) hovered = element;
	if (element == selected && mUiReveal) ImGui::SetScrollHereY();
	if (open && children > 0)
	{
		for (int i = 0; i < children; ++i) DrawUiTreeNode(element->GetChild(i), selected, hovered);
		ImGui::TreePop();
	}
}

void EditorLayer::DrawUiInspector(Engine& engine)
{
	// Docked with the Inspector the first time it appears.
	if (ImGuiWindow* inspector = ImGui::FindWindowByName("Inspector"); inspector && inspector->DockId)
		ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
	// With CHERRY_DEV_UI_SELECT (screenshots) the panel floats on the right, whatever the saved layout.
	if (const char* dev = std::getenv("CHERRY_DEV_UI_SELECT"); dev && *dev)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.62f, viewport->WorkPos.y + 40.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.36f, viewport->WorkSize.y * 0.9f), ImGuiCond_Always);
		ImGui::SetNextWindowFocus();
	}
	const bool visible = ImGui::Begin(kUiWindow);
	Ui& ui = engine.GetUi();
	Rml::Context* context = ui.GetContext();
	if (!context)
	{
		mUiPicking = false;
		mUiSelected = nullptr;
		if (visible) ImGui::TextDisabled("Play (F5) to inspect the game's UI documents.");
		ImGui::End();
		return;
	}
	if (!InContext(*context, mUiSelected)) mUiSelected = nullptr;
	// CHERRY_DEV_UI_SELECT="selector": select the first element it matches, once one exists (checks without a mouse).
	static const char* devSelect = std::getenv("CHERRY_DEV_UI_SELECT");
	if (devSelect && *devSelect && !mUiSelected)
	{
		for (int i = 0; i < context->GetNumDocuments() && !mUiSelected; ++i)
			if (Rml::ElementDocument* document = context->GetDocument(i); document && !IsDebuggerDocument(document))
				mUiSelected = document->QuerySelector(devSelect);
		if (mUiSelected)
		{
			mUiReveal = true;
			devSelect = nullptr;
		}
	}
	Rml::Element* selected = mUiSelected;
	Rml::Element* hovered = nullptr;
	ImDrawList* overlay = ImGui::GetForegroundDrawList();

	// ---- Picking in the game view ----
	if (mUiPicking)
	{
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const Rml::Vector2f point(mouse.x - viewportPosition.x, mouse.y - viewportPosition.y);
		if (viewportHovered)
		{
			hovered = PickInContext(*context, point);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered)
			{
				mUiSelected = hovered;
				selected = hovered;
				mUiReveal = true;
				mUiPicking = false;
			}
			viewportHovered = false;   // the game and its UI don't see the mouse while picking
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) mUiPicking = false;
	}

	if (visible)
	{
		if (mUiPicking) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(mUiPicking ? "Picking... (Esc)" : "Pick")) mUiPicking = !mUiPicking;
		if (mUiPicking) ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click an element in the game view to select it");
		ImGui::SameLine();
		bool debugger = ui.IsDebuggerVisible();
		if (ImGui::Checkbox("RmlUi debugger (F11)", &debugger)) ui.SetDebuggerVisible(debugger);

		// ---- Tree ----
		const float treeHeight = std::max(ImGui::GetContentRegionAvail().y * 0.4f, ImGui::GetTextLineHeightWithSpacing() * 6.0f);
		ImGui::BeginChild("##uitree", ImVec2(0, treeHeight), ImGuiChildFlags_Borders);
		for (int i = 0; i < context->GetNumDocuments(); ++i)
		{
			Rml::ElementDocument* document = context->GetDocument(i);
			if (!document || IsDebuggerDocument(document)) continue;
			const std::string& source = document->GetSourceURL();
			ImGui::PushID(document);
			if (mUiReveal && (selected == document || IsAncestorOf(document, selected))) ImGui::SetNextItemOpen(true);
			const bool open = ImGui::TreeNodeEx("##document", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s%s",
				source.c_str(), document->IsVisible() ? "" : " (hidden)");
			if (open)
			{
				for (int c = 0; c < document->GetNumChildren(); ++c) DrawUiTreeNode(document->GetChild(c), selected, hovered);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		mUiReveal = false;
		ImGui::EndChild();

		// ---- The selected element ----
		if (!selected)
		{
			ImGui::TextDisabled("Select an element in the tree, or Pick one in the game view.");
		}
		else
		{
			ImGui::TextWrapped("%s", selected->GetAddress(false, true).c_str());
			const Rml::Vector2f offset = selected->GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = selected->GetBox().GetSize(Rml::BoxArea::Border);
			ImGui::TextDisabled("Border box %.0f, %.0f  %.0f x %.0f px%s", offset.x, offset.y, size.x, size.y,
				selected->IsVisible(true) ? "" : "  (not visible)");
			if (Rml::Element* parent = selected->GetParentNode())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Parent"))
				{
					mUiSelected = parent;
					mUiReveal = true;
				}
			}

			// Classes: remove with x, add by name.
			ImGui::SeparatorText("Classes");
			std::istringstream classes(selected->GetClassNames());
			std::string name;
			bool any = false;
			while (classes >> name)
			{
				any = true;
				ImGui::PushID(name.c_str());
				if (ImGui::SmallButton(("x " + name).c_str())) selected->SetClass(name, false);
				ImGui::PopID();
				ImGui::SameLine();
			}
			if (!any) ImGui::TextDisabled("none");
			else ImGui::NewLine();
			ImGui::SetNextItemWidth(160);
			if (ImGui::InputTextWithHint("##addclass", "add class", mUiNewClass, sizeof(mUiNewClass), ImGuiInputTextFlags_EnterReturnsTrue)
				&& mUiNewClass[0])
			{
				selected->SetClass(mUiNewClass, true);
				mUiNewClass[0] = 0;
			}

			// Properties: value (editable), then where it's set.
			ImGui::SeparatorText("Properties");
			ImGui::TextDisabled("Edits apply as inline styles until the document reloads.");
			std::ostringstream rcss;
			if (ImGui::BeginTable("##properties", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
			{
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("border-bottom-right-radius").x);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 140);
				std::vector<std::pair<std::string, Rml::Property>> properties;
				for (Rml::PropertiesIteratorView it = selected->IterateLocalProperties(); !it.AtEnd(); ++it)
					properties.emplace_back(it.GetName(), it.GetProperty());
				std::sort(properties.begin(), properties.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				std::string remove;
				for (const auto& [property, value] : properties)
				{
					const std::string text = value.ToString();
					rcss << "\t" << property << ": " << text << ";\n";
					ImGui::PushID(property.c_str());
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(property.c_str());
					ImGui::TableNextColumn();
					char buffer[256];
					std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputText("##value", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue))
					{
						mUiError = selected->SetProperty(property, buffer) ? "" : "Couldn't parse " + property + ": " + buffer;
					}
					ImGui::TableNextColumn();
					if (const Rml::PropertySource* source = value.source.get())
					{
						const std::string where = fs::u8path(source->path).filename().string() + ":" + std::to_string(source->line_number);
						if (ImGui::SmallButton(where.c_str()))
						{
							fs::path file = fs::u8path(source->path);
							if (file.is_relative()) file = engine.Project().content / file;
							OpenSource(file, source->line_number);
						}
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s, line %d (click to open)", source->rule_name.c_str(), source->path.c_str(), source->line_number);
					}
					else
					{
						ImGui::TextDisabled("inline");
						ImGui::SameLine();
						if (ImGui::SmallButton("x")) remove = property;
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
				if (!remove.empty()) selected->RemoveProperty(remove);
			}
			// Add a property.
			ImGui::SetNextItemWidth(120);
			ImGui::InputTextWithHint("##newprop", "property", mUiNewProperty, sizeof(mUiNewProperty));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-60);
			const bool enter = ImGui::InputTextWithHint("##newvalue", "value", mUiNewValue, sizeof(mUiNewValue), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			if ((ImGui::Button("Set") || enter) && mUiNewProperty[0])
			{
				if (selected->SetProperty(mUiNewProperty, mUiNewValue))
				{
					mUiError.clear();
					mUiNewProperty[0] = mUiNewValue[0] = 0;
				}
				else
				{
					mUiError = std::string("Couldn't parse ") + mUiNewProperty + ": " + mUiNewValue;
				}
			}
			if (!mUiError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", mUiError.c_str());
			if (ImGui::Button("Copy as RCSS"))
			{
				const std::string block = selected->GetAddress(false, false) + "\n{\n" + rcss.str() + "}\n";
				ImGui::SetClipboardText(block.c_str());
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("The element's properties as a rule, to paste into its style sheet");

			// Text of a text node, or the element's own text.
			if (selected->GetNumChildren() == 1 && IsText(selected->GetChild(0)))
				ImGui::TextDisabled("Text: %s", static_cast<Rml::ElementText*>(selected->GetChild(0))->GetText().c_str());
		}
	}
	ImGui::End();

	// ---- Boxes over the game view: selected (cyan), under the mouse or the tree's cursor (orange) ----
	if (engine.IsEditorMode())
	{
		overlay->PushClipRect(ImVec2(viewportPosition.x, viewportPosition.y),
			ImVec2(viewportPosition.x + viewportSize.x, viewportPosition.y + viewportSize.y), false);
		if (selected) DrawBox(overlay, viewportPosition, selected, IM_COL32(80, 220, 255, 255), 2.0f);
		if (hovered && hovered != selected) DrawBox(overlay, viewportPosition, hovered, IM_COL32(255, 160, 60, 255), 1.5f);
		if (mUiPicking && hovered)
		{
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			overlay->AddText(ImVec2(mouse.x + 14, mouse.y + 10), IM_COL32(255, 200, 120, 255), Label(hovered).c_str());
		}
		overlay->PopClipRect();
	}
}
