// The prototype's on-screen UI (PROTOTYPE_EXPORT.md B15, Part C's CSS): the score / time box
// with Start, the FPS box, the end card and the floating score popups, drawn with ImGui over
// the game view at the reference's pixel sizes (times the display's DPI scale), in Segoe UI
// (the reference's system-ui on Windows).
#include "DiceModule.h"
#include "DiceVisuals.h"
#include <imgui.h>
#include <fstream>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;

namespace
{
	constexpr ImU32 Rgba(uint32_t rgb, float alpha = 1.0f)
	{
		return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, static_cast<int>(alpha * 255.0f + 0.5f));
	}
	constexpr uint32_t kText = 0xd9dde3, kDim = 0x8b93a0, kLine = 0x2c313a, kAccent = 0xffd866;
	constexpr float kPopupLife = 0.9f;
	constexpr size_t kPopupPool = 140;

	float Dpi() { return ImGui::GetStyle().FontScaleDpi > 0 ? ImGui::GetStyle().FontScaleDpi : 1.0f; }
}

void DiceModule::LoadFonts()
{
	ImGuiIO& io = ImGui::GetIO();
	ImFontAtlas* atlas = io.Fonts;
	// The first font in the atlas is ImGui's default unless FontDefault says otherwise: keep
	// the engine's (the editor and the debug panel stay as they are).
	// Before the first frame the atlas is still empty (ImGui adds its default font then): add
	// that default first, as ImGui itself would.
	if (!io.FontDefault && atlas->Fonts.Size == 0) atlas->AddFontDefault();
	ImFont* engineDefault = io.FontDefault ? io.FontDefault : atlas->Fonts[0];
	auto load = [&](const char* file) -> ImFont*
	{
		const std::string path = std::string("C:\\Windows\\Fonts\\") + file;
		if (std::ifstream(path, std::ios::binary)) return atlas->AddFontFromFileTTF(path.c_str(), 16.0f);
		return nullptr;
	};
	mFontRegular = load("segoeui.ttf");
	mFontSemibold = load("seguisb.ttf");
	mFontBold = load("segoeuib.ttf");
	mFontBlack = load("seguibl.ttf");
	if (engineDefault) io.FontDefault = engineDefault;
}

void DiceModule::ReleaseFonts()
{
	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	for (ImFont** font : { &mFontRegular, &mFontSemibold, &mFontBold, &mFontBlack })
		if (*font) { atlas->RemoveFont(*font); *font = nullptr; }
}

ImFont* DiceModule::FontFor(int weight) const
{
	ImFont* font = weight >= 900 ? mFontBlack : weight >= 700 ? mFontBold : weight >= 600 ? mFontSemibold : mFontRegular;
	if (!font) font = mFontBold ? mFontBold : mFontRegular;
	return font ? font : ImGui::GetIO().Fonts->Fonts[0];
}

// Text with the reference's letter-spacing (0.08em on the small upper-case labels).
static void DrawSpaced(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text, float spacing)
{
	char glyph[2] = { 0, 0 };
	for (const char* c = text; *c; ++c)
	{
		glyph[0] = *c;
		draw->AddText(font, size, at, color, glyph);
		at.x += font->CalcTextSizeA(size, FLT_MAX, 0, glyph).x + spacing;
	}
}

static float SpacedWidth(ImFont* font, float size, const char* text, float spacing)
{
	const size_t n = std::strlen(text);
	return font->CalcTextSizeA(size, FLT_MAX, 0, text).x + spacing * static_cast<float>(n > 0 ? n - 1 : 0);
}

// Where things drawn "on the 3D view" go (the ring, popups): in the full-window game the view
// is under all of ImGui, so the background list puts them over it and under the panel; in the
// editor the view is itself an ImGui window, so only the foreground list is over it.
ImDrawList* DiceModule::SceneOverlay(Engine& engine) const
{
	return engine.IsEditorMode() ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
}

bool DiceModule::Project(Engine& engine, double x, double y, double z, ImVec2& screen) const
{
	XMMATRIX viewProj;
	if (!ViewProjection(engine, viewProj)) return false;
	const dice::Vec3 p = dice::ToEngine({ x, y, z });
	XMFLOAT3 ndc;
	XMStoreFloat3(&ndc, XMVector3TransformCoord(XMVectorSet(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1), viewProj));
	const ViewRect view = engine.GameViewRect();
	screen = ImVec2(view.x + (ndc.x * 0.5f + 0.5f) * view.width, view.y + (0.5f - ndc.y * 0.5f) * view.height);
	return true;
}

// ---- Popups (B15): pool of 140 (the oldest is reused when full), 0.9 s, rising 0.9 units ----
void DiceModule::TakePopups()
{
	for (dice::PopupRequest& request : mSim.popupsOut)
	{
		if (mPopups.size() >= kPopupPool) mPopups.erase(mPopups.begin());
		mPopups.push_back({ request.x, request.y, request.z, std::move(request.text), request.style, mSim.now });
	}
	mSim.popupsOut.clear();
}

void DiceModule::DrawPopups(Engine& engine)
{
	ImDrawList* draw = SceneOverlay(engine);
	const ViewRect view = engine.GameViewRect();
	draw->PushClipRect(ImVec2(view.x, view.y), ImVec2(view.x + view.width, view.y + view.height), false);
	size_t keep = 0;
	for (size_t k = 0; k < mPopups.size(); ++k)
	{
		ShownPopup& p = mPopups[k];
		const float age = static_cast<float>((mSim.now - p.t) / kPopupLife);
		if (age >= 1) continue;
		mPopups[keep++] = std::move(p);
		const ShownPopup& q = mPopups[keep - 1];
		ImVec2 at;
		if (!Project(engine, q.x, q.y + age * 0.9, q.z, at)) continue;
		const float scale = age < 0.15f ? 0.6f + age / 0.15f * 0.5f : 1.1f - std::min(0.1f, age - 0.15f);
		const float alpha = age > 0.6f ? 1 - (age - 0.6f) / 0.4f : 1.0f;

		// The stylesheet's cascade: .pop, then .free, .bump, .six, .clash, .stack, .topple.
		uint32_t color = 0xffe27a;
		float size = 14;
		int weight = 800;
		if (q.style & dice::Pop_Free) { color = 0xb8c0cc; weight = 600; }
		if (q.style & dice::Pop_Bump) color = 0x7fd3ff;
		if (q.style & dice::Pop_Six) { size = 17; color = 0xfff1a8; }
		if (q.style & dice::Pop_Clash) { color = 0xff8a3d; size = 19; }
		if (q.style & dice::Pop_Stack) color = 0xb6f08a;
		if (q.style & dice::Pop_Topple) { color = 0x7fe0ff; size = 19; }

		ImFont* font = FontFor(weight);
		const float px = size * scale * Dpi();
		const ImVec2 extent = font->CalcTextSizeA(px, FLT_MAX, 0, q.text.c_str());
		const ImVec2 corner(at.x - extent.x * 0.5f, at.y - extent.y * 0.5f);
		// text-shadow: 0 1px 2px #000, 0 0 6px rgba(0,0,0,.6)
		const float d = Dpi();
		for (int s = 0; s < 8; ++s)
		{
			const float a = s * 0.785398f;
			draw->AddText(font, px, ImVec2(corner.x + std::cos(a) * 2.5f * d, corner.y + std::sin(a) * 2.5f * d), IM_COL32(0, 0, 0, static_cast<int>(40 * alpha)), q.text.c_str());
		}
		draw->AddText(font, px, ImVec2(corner.x, corner.y + 1 * d), IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), q.text.c_str());
		draw->AddText(font, px, corner, Rgba(color, alpha), q.text.c_str());
	}
	mPopups.resize(keep);
	draw->PopClipRect();
}

// ---- HUD (B15): score, time and Start top left; FPS top right ----------------------------
void DiceModule::DrawHud(Engine& engine)
{
	const ViewRect view = engine.GameViewRect();
	const float d = Dpi();
	ImDrawList* fg = ImGui::GetForegroundDrawList();

	// FPS: 13px text, padding 6 / 10, radius 8.
	{
		char text[32];
		if (mFps > 0) std::snprintf(text, sizeof(text), "%d fps", mFps);
		else std::snprintf(text, sizeof(text), "-- fps");
		ImFont* font = FontFor(400);
		const ImVec2 size = font->CalcTextSizeA(13 * d, FLT_MAX, 0, text);
		const ImVec2 max(view.x + view.width - 12 * d, view.y + 12 * d + size.y + 12 * d);
		const ImVec2 min(max.x - size.x - 20 * d, view.y + 12 * d);
		fg->AddRectFilled(min, max, IM_COL32(20, 22, 26, 199), 8 * d);
		fg->AddRect(min, max, Rgba(kLine), 8 * d);
		fg->AddText(font, 13 * d, ImVec2(min.x + 10 * d, min.y + 6 * d), Rgba(kText), text);
	}

	// Score / Time / Start: padding 10 / 14, gap 14, radius 10.
	char score[32], time[32];
	std::snprintf(score, sizeof(score), "%lld", mSim.score);
	std::snprintf(time, sizeof(time), "%.1f", mSim.phase == dice::Phase::Free ? mSim.config.sessionSeconds : mSim.timeLeft);
	const char* buttonLabel = (mSim.phase == dice::Phase::Running || mSim.phase == dice::Phase::Finishing) ? "Restart" : "Start";
	ImFont* big = FontFor(800);
	ImFont* label = FontFor(400);
	ImFont* button = FontFor(700);
	const float lblSize = 10 * d, bigSize = 26 * d, buttonSize = 14 * d, spacing = 0.8f * d;
	const float lblH = 10 * 1.35f * d, bigH = 26 * 1.35f * d;
	const float scoreW = std::max(70 * d, big->CalcTextSizeA(bigSize, FLT_MAX, 0, score).x);
	const float timeW = std::max(70 * d, big->CalcTextSizeA(bigSize, FLT_MAX, 0, time).x);
	const ImVec2 buttonText = button->CalcTextSizeA(buttonSize, FLT_MAX, 0, buttonLabel);
	const ImVec2 buttonExtent(buttonText.x + 32 * d, buttonText.y + 16 * d);
	const float innerH = std::max(lblH + bigH, buttonExtent.y);
	const ImVec2 boxMin(view.x + 12 * d, view.y + 12 * d);
	const ImVec2 boxMax(boxMin.x + 14 * d + scoreW + 14 * d + timeW + 14 * d + buttonExtent.x + 14 * d, boxMin.y + 10 * d + innerH + 10 * d);

	ImGui::SetNextWindowPos(boxMin);
	ImGui::SetNextWindowSize(ImVec2(boxMax.x - boxMin.x, boxMax.y - boxMin.y));
	ImGui::SetNextWindowBgAlpha(0.0f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	if (ImGui::Begin("##DiceHud", nullptr, flags))
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(boxMin, boxMax, IM_COL32(20, 22, 26, 199), 10 * d);
		draw->AddRect(boxMin, boxMax, Rgba(kLine), 10 * d);
		float x = boxMin.x + 14 * d;
		const float top = boxMin.y + 10 * d + (innerH - lblH - bigH) * 0.5f;
		for (int column = 0; column < 2; ++column)
		{
			DrawSpaced(draw, label, lblSize, ImVec2(x, top + 1 * d), Rgba(kDim), column == 0 ? "SCORE" : "TIME", spacing);
			draw->AddText(big, bigSize, ImVec2(x, top + lblH), Rgba(kText), column == 0 ? score : time);
			x += (column == 0 ? scoreW : timeW) + 14 * d;
		}
		const ImVec2 bMin(x, boxMin.y + 10 * d + (innerH - buttonExtent.y) * 0.5f);
		ImGui::SetCursorScreenPos(bMin);
		if (ImGui::InvisibleButton("##start", buttonExtent)) StartSession();
		draw->AddRectFilled(bMin, ImVec2(bMin.x + buttonExtent.x, bMin.y + buttonExtent.y), Rgba(kAccent), 6 * d);
		draw->AddText(button, buttonSize, ImVec2(bMin.x + 16 * d, bMin.y + 8 * d), Rgba(0x1a1a1a), buttonLabel);
	}
	ImGui::End();
	ImGui::PopStyleVar(2);

	if (mSim.phase == dice::Phase::Ended) DrawEndCard(engine);
}

// ---- End card: "Time's up", the total, rolls / sixes / bumps / clashes, Restart ----------
void DiceModule::DrawEndCard(Engine& engine)
{
	const ViewRect view = engine.GameViewRect();
	const float d = Dpi();
	char total[32], sub[128];
	std::snprintf(total, sizeof(total), "%lld", mSim.score);
	std::snprintf(sub, sizeof(sub), "%d rolls \xC2\xB7 %d sixes \xC2\xB7 %d bumps \xC2\xB7 %d clashes",
		mSim.sStats.rolls, mSim.sStats.sixes, mSim.sStats.bumps, mSim.sStats.clashes);
	ImFont* label = FontFor(400);
	ImFont* black = FontFor(900);
	ImFont* button = FontFor(700);
	const float spacing = 0.8f * d;
	const float lblW = SpacedWidth(label, 10 * d, "TIME'S UP", spacing);
	const ImVec2 totalSize = black->CalcTextSizeA(48 * d, FLT_MAX, 0, total);
	const ImVec2 subSize = label->CalcTextSizeA(13 * d, FLT_MAX, 0, sub);
	const ImVec2 buttonText = button->CalcTextSizeA(14 * d, FLT_MAX, 0, "Restart");
	const ImVec2 buttonExtent(buttonText.x + 32 * d, buttonText.y + 16 * d);
	const float contentW = std::max({ 240 * d - 60 * d, lblW, totalSize.x, subSize.x, buttonExtent.x });
	const float lblH = 10 * 1.35f * d, totalH = 48 * 1.35f * d, subH = 13 * 1.35f * d;
	const float height = 22 * d + lblH + 4 * d + totalH + 8 * d + subH + 14 * d + buttonExtent.y + 22 * d;
	const float width = contentW + 60 * d;
	const ImVec2 min(view.x + (view.width - width) * 0.5f, view.y + (view.height - height) * 0.5f);
	const ImVec2 max(min.x + width, min.y + height);

	ImGui::SetNextWindowPos(min);
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::SetNextWindowBgAlpha(0.0f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	if (ImGui::Begin("##DiceEndCard", nullptr, flags))
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(min, max, IM_COL32(28, 31, 37, 242), 14 * d);
		draw->AddRect(min, max, Rgba(kLine), 14 * d);
		const float cx = min.x + width * 0.5f;
		float y = min.y + 22 * d;
		DrawSpaced(draw, label, 10 * d, ImVec2(cx - lblW * 0.5f, y + 1 * d), Rgba(kDim), "TIME'S UP", spacing);
		y += lblH + 4 * d;
		draw->AddText(black, 48 * d, ImVec2(cx - totalSize.x * 0.5f, y), Rgba(kAccent), total);
		y += totalH + 8 * d;
		draw->AddText(label, 13 * d, ImVec2(cx - subSize.x * 0.5f, y), Rgba(kDim), sub);
		y += subH + 14 * d;
		const ImVec2 bMin(cx - buttonExtent.x * 0.5f, y);
		ImGui::SetCursorScreenPos(bMin);
		if (ImGui::InvisibleButton("##restart", buttonExtent)) StartSession();
		draw->AddRectFilled(bMin, ImVec2(bMin.x + buttonExtent.x, bMin.y + buttonExtent.y), Rgba(kAccent), 6 * d);
		draw->AddText(button, 14 * d, ImVec2(bMin.x + 16 * d, bMin.y + 8 * d), Rgba(0x1a1a1a), "Restart");
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
}

void DiceModule::StartSession()
{
	mAudio.Start();   // the reference starts its audio on Start too
	mSim.StartSession();
}
