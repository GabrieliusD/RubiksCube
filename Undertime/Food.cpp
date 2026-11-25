// Hunger and food (Docs/Hauling.md H5): the meter drains in your own time, the same at any
// depth, so it limits how long you stay down. F eats the most filling thing you carry; a fire
// pit or stove cooks raw food into meals that fill more. Empty, you're weaker (pick, hammer,
// axe and the barrow) until you eat; nobody starves to death.
#include "UndertimeModule.h"
#include <imgui.h>
#include <algorithm>

void UndertimeModule::UpdateHunger(Engine& engine, float dt)
{
	const float before = hunger;
	hunger = std::max(0.0f, hunger - dt / (cfg::kHungerMinutes * 60.0f));
	if (before > cfg::kHungerWarn && hunger <= cfg::kHungerWarn && !hungerWarned)
	{
		messages.Show("You're getting hungry (F to eat).", 2.5f);
		hungerWarned = true;
	}
	if (before > 0.0f && hunger <= 0.0f && !starvingWarned)
	{
		messages.Show("You're starving: weaker until you eat.", 3.0f);
		starvingWarned = true;
	}
	if (engine.GetInput().WasKeyPressed('F'))
	{
		const std::string food = econ::BestFood([this](const std::string& id) { return Have(id); });
		if (food.empty()) messages.Show("You've nothing to eat. The trading post sells bread.", 2.0f);
		else if (hunger >= 0.98f) messages.Show("You're not hungry.", 1.2f);
		else Eat(food);
	}
}

bool UndertimeModule::Eat(const std::string& id)
{
	const econ::Item* item = econ::FindItem(id);
	if (!item || item->kind != econ::Kind::Food || Have(id) <= 0) return false;
	Take(id, 1);
	hunger = std::min(1.0f, hunger + item->food);
	if (hunger > cfg::kHungerWarn) hungerWarned = false;
	starvingWarned = false;
	messages.Show(std::string("You ate ") + item->name + ".", 1.5f);
	return true;
}

void UndertimeModule::OnSlept(Engine&, double)
{
	hunger = std::max(0.0f, hunger - cfg::kSleepHunger);
	hungerWarned = starvingWarned = false;
}

void UndertimeModule::DrawCooking(Engine&)
{
	const ImVec4 gold(1.0f, 0.77f, 0.47f, 1.0f);
	ImGui::TextColored(gold, "Cooking");
	ImGui::SameLine();
	ImGui::TextDisabled("cooked food fills more");
	ImGui::Separator();
	const auto have = [this](const std::string& id) { return Have(id); };
	const auto& recipes = econ::CookRecipes();
	for (size_t i = 0; i < recipes.size(); ++i)
	{
		const econ::Recipe& r = recipes[i];
		const econ::Item* item = econ::FindItem(r.item);
		if (!item) continue;
		std::string needs;
		for (const auto& [id, n] : r.needs) needs += (needs.empty() ? "" : " + ") + std::to_string(n) + " " + econ::NameOf(id);
		const econ::Verdict v = econ::CanMake(r, have);
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!v.ok);
		if (ImGui::Button("Cook"))
		{
			for (const auto& [id, n] : r.needs) Take(id, n);
			purse.Add(r.item, r.makes);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("%s%s", r.makes > 1 ? (std::to_string(r.makes) + " x ").c_str() : "", item->name);
		ImGui::SameLine();
		ImGui::TextDisabled("from %s  (fills %d%%)", needs.c_str(), static_cast<int>(item->food * 100.0f + 0.5f));
		ImGui::PopID();
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("Hunger: %d%% full", static_cast<int>(hunger * 100.0f + 0.5f));
	int row = 100;
	for (const econ::Item& item : econ::Items())
	{
		if (item.kind != econ::Kind::Food || Have(item.id) <= 0) continue;
		ImGui::PushID(row++);
		if (ImGui::SmallButton("Eat")) Eat(item.id);
		ImGui::SameLine();
		ImGui::Text("%s  x%d", item.name, Have(item.id));
		ImGui::PopID();
	}
}
