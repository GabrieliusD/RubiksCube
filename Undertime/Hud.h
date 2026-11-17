#pragma once
#include <Core/Engine.h>
#include <imgui.h>
#include <string>
#include <vector>

// Screen-space HUD drawn with ImGui draw lists inside the game view rectangle.
namespace hud
{
	struct Frame
	{
		ImDrawList* draw = nullptr;
		ImVec2 origin{ 0, 0 };    // top-left of the game view
		ImVec2 size{ 0, 0 };
		float fontSize = 13.0f;   // base font size (DPI-scaled)

		ImVec2 Center() const { return { origin.x + size.x * 0.5f, origin.y + size.y * 0.5f }; }
	};

	Frame Begin(const Engine& engine);
	void End(const Frame& f);

	// Crosshair with an optional progress ring (0..1) and a label underneath.
	void Crosshair(const Frame& f, float progress, const char* label, ImU32 labelColor);
	// Text lines in a dark rounded panel. anchor: 0 = top-left, 1 = top-right, 2 = bottom-left.
	void Panel(const Frame& f, int anchor, const std::vector<std::string>& lines, float scale = 1.0f);
	// Centred message in the upper third, faded by alpha.
	void Message(const Frame& f, const std::string& text, float alpha);
	// Full-view overlay with a title and lines (start / pause screens).
	void Overlay(const Frame& f, const char* title, const std::vector<std::string>& lines, const char* footer);
}

// One message at a time; newer messages replace older ones.
class MessageLog
{
public:
	void Show(const std::string& text, float seconds);
	void Update(float deltaTime);
	void Draw(const hud::Frame& f) const;
	const std::string& Current() const { return mText; }

private:
	std::string mText;
	float mTimeLeft = 0.0f;
	float mDuration = 0.0f;
};
