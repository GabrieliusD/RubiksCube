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
	// A small hint under the crosshair ("E  Talk to Agnes Rook").
	void Prompt(const Frame& f, const std::string& text);
	// A spoken line at the bottom of the view: the speaker's name, then the line, faded by alpha.
	void Subtitle(const Frame& f, const std::string& speaker, const std::string& text, float alpha);
	// The watch, bottom right: the outside time on the big dial (cyan hands), your own time on
	// the small one (one turn a minute). `night` 0..1 darkens the face; `blur` 0..1 when
	// outside days flash by, the hands smear into a glowing ring. `lines` go in a panel
	// beside it; `warning` makes the rim pulse (an era change is near).
	void Watch(const Frame& f, float outsideHour, double personalSeconds, float night, float blur, bool warning, float time,
		const std::vector<std::string>& lines);
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
