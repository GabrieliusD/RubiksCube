#include "Hud.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
	constexpr float kPi = 3.14159265f;
	const ImU32 kPanelBg = IM_COL32(12, 10, 16, 175);
	const ImU32 kText = IM_COL32(236, 228, 214, 255);
	const ImU32 kDim = IM_COL32(170, 160, 150, 255);

	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		std::stringstream stream(text);
		std::string line;
		while (std::getline(stream, line)) lines.push_back(line);
		return lines;
	}

	ImVec2 TextSize(float size, const std::string& text)
	{
		return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
	}
}

namespace hud
{
	Frame Begin(const Engine& engine)
	{
		const ViewRect rect = engine.GameViewRect();
		Frame f;
		f.draw = ImGui::GetForegroundDrawList();
		f.origin = { rect.x, rect.y };
		f.size = { rect.width, rect.height };
		f.fontSize = ImGui::GetFontSize();
		// Keep the HUD inside the game view (matters in the editor viewport).
		f.draw->PushClipRect(f.origin, { f.origin.x + f.size.x, f.origin.y + f.size.y }, true);
		return f;
	}

	void End(const Frame& f)
	{
		f.draw->PopClipRect();
	}

	void Crosshair(const Frame& f, float progress, const char* label, ImU32 labelColor)
	{
		const ImVec2 c = f.Center();
		const float s = f.fontSize / 13.0f;
		f.draw->AddCircleFilled(c, 2.2f * s, IM_COL32(255, 255, 255, 220));
		f.draw->AddCircle(c, 2.2f * s + 1.0f, IM_COL32(0, 0, 0, 140), 0, 1.0f);

		if (progress > 0.0f)
		{
			const float radius = 14.0f * s;
			f.draw->AddCircle(c, radius, IM_COL32(0, 0, 0, 120), 48, 4.0f * s);
			const float start = -kPi * 0.5f;
			f.draw->PathArcTo(c, radius, start, start + kPi * 2.0f * std::min(progress, 1.0f), 48);
			f.draw->PathStroke(IM_COL32(255, 214, 140, 255), 0, 3.0f * s);
		}

		if (label && *label)
		{
			const float size = f.fontSize * 1.1f;
			const ImVec2 ts = TextSize(size, label);
			const ImVec2 pos{ c.x - ts.x * 0.5f, c.y + 22.0f * s };
			f.draw->AddText(nullptr, size, { pos.x + 1, pos.y + 1 }, IM_COL32(0, 0, 0, 200), label);
			f.draw->AddText(nullptr, size, pos, labelColor, label);
		}
	}

	void Panel(const Frame& f, int anchor, const std::vector<std::string>& lines, float scale)
	{
		if (lines.empty()) return;
		const float size = f.fontSize * scale;
		const float pad = size * 0.7f;
		const float lineHeight = size * 1.35f;
		float width = 0.0f;
		for (const auto& line : lines) width = std::max(width, TextSize(size, line).x);
		const ImVec2 panelSize{ width + pad * 2.0f, lineHeight * lines.size() + pad * 2.0f - (lineHeight - size) };

		const float margin = size;
		ImVec2 pos = { f.origin.x + margin, f.origin.y + margin };
		if (anchor == 1) pos.x = f.origin.x + f.size.x - margin - panelSize.x;
		if (anchor == 2) pos.y = f.origin.y + f.size.y - margin - panelSize.y;

		f.draw->AddRectFilled(pos, { pos.x + panelSize.x, pos.y + panelSize.y }, kPanelBg, size * 0.4f);
		for (size_t i = 0; i < lines.size(); ++i)
		{
			f.draw->AddText(nullptr, size, { pos.x + pad, pos.y + pad + i * lineHeight }, i == 0 ? kText : kDim, lines[i].c_str());
		}
	}

	void Message(const Frame& f, const std::string& text, float alpha)
	{
		if (text.empty() || alpha <= 0.0f) return;
		const std::vector<std::string> lines = SplitLines(text);
		const float size = f.fontSize * 1.35f;
		const float lineHeight = size * 1.4f;
		float width = 0.0f;
		for (const auto& line : lines) width = std::max(width, TextSize(size, line).x);
		const float pad = size;
		const ImVec2 boxSize{ width + pad * 2.0f, lineHeight * lines.size() + pad * 2.0f - (lineHeight - size) };
		const ImVec2 pos{ f.origin.x + (f.size.x - boxSize.x) * 0.5f, f.origin.y + f.size.y * 0.22f - boxSize.y * 0.5f };

		const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
		f.draw->AddRectFilled(pos, { pos.x + boxSize.x, pos.y + boxSize.y }, IM_COL32(8, 6, 12, a * 200 / 255), size * 0.4f);
		for (size_t i = 0; i < lines.size(); ++i)
		{
			const float w = TextSize(size, lines[i]).x;
			f.draw->AddText(nullptr, size, { pos.x + (boxSize.x - w) * 0.5f, pos.y + pad + i * lineHeight }, IM_COL32(240, 232, 220, a), lines[i].c_str());
		}
	}

	void Prompt(const Frame& f, const std::string& text)
	{
		if (text.empty()) return;
		const float size = f.fontSize * 1.1f;
		const ImVec2 t = TextSize(size, text);
		const ImVec2 c = f.Center();
		const ImVec2 pos{ c.x - t.x * 0.5f, c.y + f.fontSize * 3.2f };
		const float pad = size * 0.45f;
		f.draw->AddRectFilled({ pos.x - pad, pos.y - pad * 0.6f }, { pos.x + t.x + pad, pos.y + t.y + pad * 0.6f }, kPanelBg, size * 0.3f);
		f.draw->AddText(nullptr, size, pos, kText, text.c_str());
	}

	void Subtitle(const Frame& f, const std::string& speaker, const std::string& text, float alpha)
	{
		if (text.empty() || alpha <= 0.0f) return;
		const float size = f.fontSize * 1.2f;
		const std::string name = speaker + ":  ";
		const ImVec2 n = TextSize(size, name), t = TextSize(size, text);
		const float width = n.x + t.x;
		const ImVec2 pos{ f.origin.x + (f.size.x - width) * 0.5f, f.origin.y + f.size.y * 0.8f };
		const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
		const float pad = size * 0.5f;
		f.draw->AddRectFilled({ pos.x - pad, pos.y - pad * 0.6f }, { pos.x + width + pad, pos.y + t.y + pad * 0.6f }, IM_COL32(8, 6, 12, a * 170 / 255), size * 0.3f);
		f.draw->AddText(nullptr, size, pos, IM_COL32(255, 196, 120, a), name.c_str());
		f.draw->AddText(nullptr, size, { pos.x + n.x, pos.y }, IM_COL32(240, 232, 220, a), text.c_str());
	}

	void Watch(const Frame& f, float outsideHour, double personalSeconds, float night, float blur, bool warning, float time,
		const std::vector<std::string>& lines)
	{
		const float s = f.fontSize / 13.0f;
		const float r = 46.0f * s;
		const float margin = f.fontSize;
		const ImVec2 c{ f.origin.x + f.size.x - margin - r, f.origin.y + f.size.y - margin - r };
		auto lerp = [](float a, float b, float t) { return static_cast<int>(a + (b - a) * t); };
		const ImU32 cyan = IM_COL32(111, 240, 255, 255);

		// Case and face: cream by day, deep blue at night.
		const float pulse = warning ? 0.5f + 0.5f * std::sin(time * 6.0f) : 0.0f;
		f.draw->AddCircleFilled({ c.x + 2 * s, c.y + 3 * s }, r + 4 * s, IM_COL32(0, 0, 0, 110), 48);
		f.draw->AddCircleFilled(c, r + 4 * s, IM_COL32(lerp(176, 111, pulse), lerp(138, 240, pulse), lerp(70, 255, pulse), 255), 48);
		f.draw->AddCircleFilled(c, r, IM_COL32(lerp(236, 28, night), lerp(228, 36, night), lerp(210, 60, night), 245), 48);
		const ImU32 ink = IM_COL32(lerp(60, 190, night), lerp(52, 196, night), lerp(46, 214, night), 255);
		for (int i = 0; i < 12; ++i)
		{
			const float a = i / 12.0f * 2.0f * kPi;
			const float inner = i % 3 == 0 ? 0.80f : 0.87f;
			const ImVec2 d{ std::sin(a), -std::cos(a) };
			f.draw->AddLine({ c.x + d.x * r * inner, c.y + d.y * r * inner }, { c.x + d.x * r * 0.94f, c.y + d.y * r * 0.94f }, ink, (i % 3 == 0 ? 2.5f : 1.2f) * s);
		}

		// Your own time: the small dial, one turn a minute.
		const ImVec2 sc{ c.x, c.y + r * 0.42f };
		const float sr = r * 0.26f;
		f.draw->AddCircle(sc, sr, ink, 32, 1.0f * s);
		const float own = static_cast<float>(std::fmod(personalSeconds, 60.0) / 60.0) * 2.0f * kPi;
		f.draw->AddLine(sc, { sc.x + std::sin(own) * sr * 0.9f, sc.y - std::cos(own) * sr * 0.9f }, IM_COL32(255, 196, 119, 255), 1.6f * s);

		// Outside time: cyan hands, or a glowing ring when they spin too fast to follow.
		const float hourAngle = std::fmod(outsideHour, 12.0f) / 12.0f * 2.0f * kPi;
		const float minuteAngle = (outsideHour - std::floor(outsideHour)) * 2.0f * kPi;
		const int handAlpha = static_cast<int>(255.0f * (1.0f - std::clamp(blur * 1.5f, 0.0f, 1.0f)));
		if (handAlpha > 0)
		{
			const ImU32 hand = (cyan & 0x00FFFFFFu) | (static_cast<ImU32>(handAlpha) << 24);
			f.draw->AddLine(c, { c.x + std::sin(hourAngle) * r * 0.5f, c.y - std::cos(hourAngle) * r * 0.5f }, hand, 3.2f * s);
			f.draw->AddLine(c, { c.x + std::sin(minuteAngle) * r * 0.76f, c.y - std::cos(minuteAngle) * r * 0.76f }, hand, 1.8f * s);
		}
		if (blur > 0.0f)
		{
			const int a = static_cast<int>(200.0f * blur);
			f.draw->AddCircle(c, r * 0.62f, IM_COL32(111, 240, 255, a), 48, 5.0f * s);
			f.draw->AddCircle(c, r * 0.62f, IM_COL32(220, 255, 255, a), 48, 1.5f * s);
		}
		f.draw->AddCircleFilled(c, 3.0f * s, cyan, 12);

		// The lines, in a panel to the left of the watch.
		if (lines.empty()) return;
		const float size = f.fontSize;
		const float pad = size * 0.7f;
		const float lineHeight = size * 1.35f;
		float width = 0.0f;
		for (const auto& line : lines) width = std::max(width, TextSize(size, line).x);
		const ImVec2 panelSize{ width + pad * 2.0f, lineHeight * lines.size() + pad * 2.0f - (lineHeight - size) };
		const ImVec2 pos{ c.x - r - 10.0f * s - panelSize.x, c.y + r - panelSize.y };
		f.draw->AddRectFilled(pos, { pos.x + panelSize.x, pos.y + panelSize.y }, kPanelBg, size * 0.4f);
		for (size_t i = 0; i < lines.size(); ++i)
		{
			const ImU32 colour = i == 0 ? kText : (warning && i + 1 == lines.size() ? cyan : kDim);
			f.draw->AddText(nullptr, size, { pos.x + pad, pos.y + pad + i * lineHeight }, colour, lines[i].c_str());
		}
	}

	void Overlay(const Frame& f, const char* title, const std::vector<std::string>& lines, const char* footer)
	{
		f.draw->AddRectFilled(f.origin, { f.origin.x + f.size.x, f.origin.y + f.size.y }, IM_COL32(6, 5, 10, 200));
		const float titleSize = f.fontSize * 4.0f;
		const float size = f.fontSize * 1.3f;
		const float lineHeight = size * 1.5f;
		const float totalHeight = titleSize * 1.6f + lineHeight * (lines.size() + 2);
		float y = f.origin.y + (f.size.y - totalHeight) * 0.5f;

		const ImVec2 titleExtent = TextSize(titleSize, title);
		f.draw->AddText(nullptr, titleSize, { f.origin.x + (f.size.x - titleExtent.x) * 0.5f, y }, IM_COL32(255, 210, 150, 255), title);
		y += titleSize * 1.6f;
		for (const auto& line : lines)
		{
			const float w = TextSize(size, line).x;
			f.draw->AddText(nullptr, size, { f.origin.x + (f.size.x - w) * 0.5f, y }, kText, line.c_str());
			y += lineHeight;
		}
		if (footer)
		{
			y += lineHeight;
			const float w = TextSize(size, footer).x;
			f.draw->AddText(nullptr, size, { f.origin.x + (f.size.x - w) * 0.5f, y }, IM_COL32(255, 196, 119, 255), footer);
		}
	}
}

void MessageLog::Show(const std::string& text, float seconds)
{
	mText = text;
	mTimeLeft = mDuration = seconds;
}

void MessageLog::Update(float dt)
{
	mTimeLeft = std::max(0.0f, mTimeLeft - dt);
	if (mTimeLeft <= 0.0f) mText.clear();
}

void MessageLog::Draw(const hud::Frame& f) const
{
	const float alpha = std::min(1.0f, mTimeLeft / cfg::kMessageFadeSeconds);
	hud::Message(f, mText, alpha);
}
