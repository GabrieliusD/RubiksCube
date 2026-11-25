// The runtime debug panel (PROTOTYPE_EXPORT.md B12): generated from dice::Controls() as the
// reference's is from its CONTROLS list, plus presets (the whole config as JSON in
// Content/Presets/), the face check and the live stats.
#include "DiceModule.h"
#include "DiceControls.h"
#include <Core/Log.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
	const ImVec4 kAccent(1.0f, 0.847f, 0.4f, 1.0f);   // #ffd866

	// One control: the label with its value right-aligned (in the accent colour), then the widget.
	bool DrawControl(const dice::Control& control, DiceConfig& config)
	{
		const dice::Field* field = dice::FindField(control.key);
		if (!field) return false;
		ImGui::PushID(control.key);
		bool changed = false;
		const float width = ImGui::GetContentRegionAvail().x;
		if (control.kind == dice::Control::Check)
		{
			changed = ImGui::Checkbox(control.label, &(config.*field->b));
		}
		else if (control.kind == dice::Control::Select)
		{
			ImGui::TextUnformatted(control.label);
			int value = dice::GetEnum(config, field->kind);
			ImGui::SetNextItemWidth(width);
			if (ImGui::Combo("##select", &value, control.options.data(), static_cast<int>(control.options.size())))
			{
				dice::SetEnum(config, field->kind, value);
				changed = true;
			}
		}
		else
		{
			const double current = field->f ? config.*field->f : config.*field->i;
			char text[32];
			// The reference's fmt: integers as they are, the rest with two decimals.
			if (current == std::floor(current)) std::snprintf(text, sizeof(text), "%.0f", current);
			else std::snprintf(text, sizeof(text), "%.2f", current);
			const float startX = ImGui::GetCursorPosX();
			ImGui::TextUnformatted(control.label);
			ImGui::SameLine(startX + width - ImGui::CalcTextSize(text).x);   // the value, right-aligned
			ImGui::TextColored(kAccent, "%s", text);
			float value = static_cast<float>(current);
			ImGui::SetNextItemWidth(width);
			if (ImGui::SliderFloat("##range", &value, control.min, control.max, ""))
			{
				const double snapped = dice::SnapToStep(value, control.min, control.max, control.step);
				if (field->f) config.*field->f = static_cast<float>(snapped);
				else config.*field->i = static_cast<int>(std::lround(snapped));
				changed = snapped != current;
			}
		}
		ImGui::PopID();
		return changed;
	}
}

std::filesystem::path DiceModule::PresetDir(Engine& engine) const
{
	return std::filesystem::path(engine.ContentPath(L"Presets"));
}

void DiceModule::RefreshPresets(Engine& engine)
{
	mPresets.clear();
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(PresetDir(engine), error))
		if (entry.path().extension() == ".json") mPresets.push_back(entry.path().stem().string());
	std::sort(mPresets.begin(), mPresets.end());
	mPresetsListed = true;
}

bool DiceModule::LoadPreset(Engine& engine, const std::string& name)
{
	std::ifstream file(PresetDir(engine) / (name + ".json"));
	if (!file) { mPresetMessage = "Can't read " + name + ".json"; return false; }
	std::stringstream text;
	text << file.rdbuf();
	DiceConfig config = DiceConfig{};   // a preset states what it changes from the defaults
	std::vector<std::string> problems;
	if (!dice::ConfigFromJson(text.str(), config, problems)) { mPresetMessage = name + ": " + problems.front(); return false; }
	ApplyConfig(engine, config);
	mPresetMessage = problems.empty() ? "Loaded " + name : name + ": " + problems.front() + (problems.size() > 1 ? " (and more)" : "");
	for (const std::string& problem : problems) CHERRY_WARN("Dice preset %s: %s", name.c_str(), problem.c_str());
	return true;
}

bool DiceModule::SavePreset(Engine& engine, const std::string& name)
{
	std::error_code error;
	std::filesystem::create_directories(PresetDir(engine), error);
	std::ofstream file(PresetDir(engine) / (name + ".json"));
	if (!file) { mPresetMessage = "Can't write " + name + ".json"; return false; }
	file << dice::ConfigToJson(mSim.config) << "\n";
	mPresetMessage = "Saved " + name;
	RefreshPresets(engine);
	return true;
}

// A whole new config: the board is rebuilt (B12: some settings only take effect then), and
// dieSize stays as it was (load-time only).
void DiceModule::ApplyConfig(Engine& engine, const DiceConfig& config)
{
	const float dieSize = mSim.config.dieSize;
	mSim.config = config;
	mSim.config.dieSize = dieSize;
	BuildBoard(engine);
}

void DiceModule::DrawPanel(Engine& engine)
{
#if defined(__ANDROID__)
	// On a phone the panel would cover the board: it starts folded (tap its title to open it).
	ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
#endif
	if (!ImGui::Begin("Dice##Panel"))
	{
		ImGui::End();
		return;
	}
	bool rebuild = false;

	// Presets: every tunable at once.
	ImGui::SeparatorText("Presets");
	if (!mPresetsListed) RefreshPresets(engine);
	{
		const float width = ImGui::GetContentRegionAvail().x;
		ImGui::SetNextItemWidth(width * 0.6f);
		const char* shown = mPresetIndex >= 0 && mPresetIndex < static_cast<int>(mPresets.size()) ? mPresets[mPresetIndex].c_str() : "(choose)";
		if (ImGui::BeginCombo("##preset", shown))
		{
			for (int k = 0; k < static_cast<int>(mPresets.size()); ++k)
				if (ImGui::Selectable(mPresets[k].c_str(), k == mPresetIndex)) mPresetIndex = k;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Load") && mPresetIndex >= 0 && mPresetIndex < static_cast<int>(mPresets.size())) LoadPreset(engine, mPresets[mPresetIndex]);
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) { ApplyConfig(engine, DiceConfig{}); mPresetMessage = "Defaults (B12)"; }
		ImGui::SetNextItemWidth(width * 0.6f);
		ImGui::InputTextWithHint("##name", "preset name", mPresetName, sizeof(mPresetName));
		ImGui::SameLine();
		if (ImGui::Button("Save") && mPresetName[0] != 0) SavePreset(engine, mPresetName);
		if (!mPresetMessage.empty()) ImGui::TextDisabled("%s", mPresetMessage.c_str());
	}

	// The reference's controls.
	for (const dice::Control& control : dice::Controls())
	{
		if (control.kind == dice::Control::Header) { ImGui::SeparatorText(control.label); continue; }
		if (DrawControl(control, mSim.config) && control.rebuild) rebuild = true;
	}
	if (!mAudio.Running()) ImGui::TextDisabled("Click once anywhere to enable sound.");

	ImGui::SeparatorText("Checks");
	{
		const char* faces[] = { "Random", "1", "2", "3", "4", "5", "6" };
		ImGui::TextUnformatted("Every die dealt showing");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		rebuild |= ImGui::Combo("##face", &mForcedFace, faces, 7);
	}

	// Live (the reference's order).
	ImGui::SeparatorText("Live");
	if (ImGui::BeginTable("##live", 2, ImGuiTableFlags_SizingStretchProp))
	{
		auto row = [](const char* label, const char* value)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(value).x);
			ImGui::TextColored(kAccent, "%s", value);
		};
		char v[64];
		std::snprintf(v, sizeof(v), "%d", mSim.diceCount); row("Dice on board", v);
		std::snprintf(v, sizeof(v), "%d", mSim.activeCount); row("Dice rolling now", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.rolls); row("Total rolls", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.bumps); row("Bumps (pushes)", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.bounces); row("Bounce-offs", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.stacks); row("Stack landings", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.topples); row("Topples", v);
		std::snprintf(v, sizeof(v), "%lld", mSim.stats.clashes); row("Clashes", v);
		std::snprintf(v, sizeof(v), "%.2f", mSim.ExpectedFace()); row("Expected face (luck)", v);
		std::snprintf(v, sizeof(v), "%d", mSim.slotCount); row("Slots on board", v);
		if (mSim.hoverCell < 0) std::snprintf(v, sizeof(v), "-");
		else std::snprintf(v, sizeof(v), "#%d (%.1f, %.1f)", mSim.hoverCell, mSim.slotX[mSim.hoverCell], mSim.slotZ[mSim.hoverCell]);
		row("Hover slot", v);
		ImGui::EndTable();
	}
	if (ImGui::Button("Re-deal board")) rebuild = true;
	ImGui::TextDisabled("Hover dice to roll them. Mouse wheel zooms.");
	const RenderStats& stats = engine.GetRenderer().Stats();
	ImGui::TextDisabled("Draw calls %u, GPU %.2f ms", stats.drawCalls, stats.gpuFrameMs);
	if (rebuild) BuildBoard(engine);
	ImGui::End();
}
