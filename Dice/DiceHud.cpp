// The prototype's on-screen UI (PROTOTYPE_EXPORT.md B15, Part C's CSS): the score / time box
// with Start, the FPS box, the end card and the floating score popups. They're an RmlUi
// document (Content/UI/Hud.rml and Hud.rcss, a port of the reference's markup and stylesheet)
// drawn by the engine over the game view; this file fills its "dice" data model. The hover
// ring stays an ImGui overlay (DiceModule.cpp).
#include "DiceModule.h"
#include "DiceVisuals.h"
#include <RmlUi/Core.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace DirectX;

namespace
{
	constexpr float kPopupLife = 0.9f;
	constexpr size_t kPopupPool = 140;
	constexpr const char* kHudDocument = "UI/Hud.rml";
}

// Where ImGui overlays drawn "on the 3D view" go (the hover ring): in the full-window game the
// view is under all of ImGui, so the background list puts them over it and under the panel; in
// the editor the view is itself an ImGui window, so only the foreground list is over it.
ImDrawList* DiceModule::SceneOverlay(Engine& engine) const
{
	return engine.IsEditorMode() ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
}

// ---- The document and its data model --------------------------------------------------------

void DiceModule::CreateHud(Engine& engine)
{
	Ui& ui = engine.GetUi();
	// The reference's system-ui: Segoe UI on Windows (no weight 800 there, so the nearest,
	// bold, stands in for it: bold loads before black, which wins RmlUi's tie), Roboto on Android
	// (one variable font; every weight comes from it).
#if defined(__ANDROID__)
	for (const char* file : { "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf" })
	{
		bool any = false;
		for (int weight : { 400, 600, 700, 800, 900 }) any |= ui.LoadFontFace(file, "system-ui", weight);
		if (any) break;
	}
#else
	const std::string fonts = "C:/Windows/Fonts/";
	ui.LoadFontFace(fonts + "segoeui.ttf", "system-ui", 400);
	ui.LoadFontFace(fonts + "seguisb.ttf", "system-ui", 600);
	ui.LoadFontFace(fonts + "segoeuib.ttf", "system-ui", 700);
	ui.LoadFontFace(fonts + "seguibl.ttf", "system-ui", 900);
#endif

	Rml::Context* context = ui.GetContext();
	if (!context) return;
	Rml::DataModelConstructor model = context->CreateDataModel("dice");
	if (!model) return;
	model.Bind("score", &mHud.score);
	model.Bind("time", &mHud.time);
	model.Bind("startLabel", &mHud.startLabel);
	model.Bind("fps", &mHud.fps);
	model.Bind("ended", &mHud.ended);
	model.Bind("total", &mHud.total);
	model.Bind("summary", &mHud.summary);
	model.BindEventCallback("start", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { StartSession(); });
	mHudModel = model.GetModelHandle();
	ui.LoadDocument(kHudDocument);
	MakePopupPool(engine);
}

// The 140 popup elements, in the document's #popups (again after the document is reloaded).
void DiceModule::MakePopupPool(Engine& engine)
{
	mPopupSlots.clear();
	mNextPopupSlot = 0;
	mHudDocument = engine.GetUi().Document(kHudDocument);
	Rml::Element* container = mHudDocument ? mHudDocument->GetElementById("popups") : nullptr;
	if (!container) return;
	mPopupSlots.resize(kPopupPool);
	for (PopupSlot& slot : mPopupSlots)
	{
		Rml::ElementPtr element = mHudDocument->CreateElement("div");
		element->SetClass("pop", true);
		element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
		slot.element = container->AppendChild(std::move(element));
	}
}

// ---- Popups (B15): pool of 140 (the oldest is reused when full), 0.9 s, rising 0.9 units ----
void DiceModule::TakePopups()
{
	for (dice::PopupRequest& request : mSim.popupsOut)
	{
		if (mPopups.size() >= kPopupPool) mPopups.erase(mPopups.begin());
		// Slots go round, so a full pool takes the oldest popup's.
		const int slot = mNextPopupSlot;
		mNextPopupSlot = (mNextPopupSlot + 1) % static_cast<int>(kPopupPool);
		mPopups.push_back({ request.x, request.y, request.z, std::move(request.text), request.style, mSim.now, slot });
	}
	mSim.popupsOut.clear();
}

void DiceModule::UpdateHud(Engine& engine)
{
	if (!mHudModel) return;
	auto set = [&](std::string& field, const std::string& value, const char* name)
	{
		if (field == value) return;
		field = value;
		mHudModel.DirtyVariable(name);
	};
	char text[128];

	// Score / Time / Start.
	std::snprintf(text, sizeof(text), "%lld", mSim.score);
	set(mHud.score, text, "score");
	std::snprintf(text, sizeof(text), "%.1f", mSim.phase == dice::Phase::Free ? mSim.config.sessionSeconds : mSim.timeLeft);
	set(mHud.time, text, "time");
	set(mHud.startLabel, (mSim.phase == dice::Phase::Running || mSim.phase == dice::Phase::Finishing) ? "Restart" : "Start", "startLabel");
	// FPS, updated every 0.5 s.
	if (mFps > 0) std::snprintf(text, sizeof(text), "%d fps", mFps);
	else std::snprintf(text, sizeof(text), "-- fps");
	set(mHud.fps, text, "fps");

	// End card: "Time's up", the total, rolls / sixes / bumps / clashes, Restart.
	const bool ended = mSim.phase == dice::Phase::Ended;
	if (ended != mHud.ended)
	{
		mHud.ended = ended;
		mHudModel.DirtyVariable("ended");
	}
	std::snprintf(text, sizeof(text), "%lld", mSim.score);
	set(mHud.total, text, "total");
	std::snprintf(text, sizeof(text), "%d rolls \xC2\xB7 %d sixes \xC2\xB7 %d bumps \xC2\xB7 %d clashes",
		mSim.sStats.rolls, mSim.sStats.sixes, mSim.sStats.bumps, mSim.sStats.clashes);
	set(mHud.summary, text, "summary");

	// Popups: placed over their point, growing in, then fading (the reference's numbers).
	if (engine.GetUi().Document(kHudDocument) != mHudDocument) MakePopupPool(engine);   // Hud.rml was reloaded
	if (mPopupSlots.size() != kPopupPool) return;
	XMMATRIX viewProj;
	const bool canProject = ViewProjection(engine, viewProj);
	const ViewRect view = engine.GameViewRect();
	bool shownNow[kPopupPool] = {};
	size_t keep = 0;
	for (size_t k = 0; k < mPopups.size(); ++k)
	{
		ShownPopup& p = mPopups[k];
		const float age = static_cast<float>((mSim.now - p.t) / kPopupLife);
		if (age >= 1) continue;
		mPopups[keep++] = std::move(p);
		const ShownPopup& q = mPopups[keep - 1];
		if (!canProject) continue;
		const dice::Vec3 at = dice::ToEngine({ q.x, q.y + age * 0.9, q.z });
		XMFLOAT3 ndc;
		XMStoreFloat3(&ndc, XMVector3TransformCoord(XMVectorSet(static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z), 1), viewProj));
		const float x = (ndc.x * 0.5f + 0.5f) * view.width, y = (0.5f - ndc.y * 0.5f) * view.height;   // view pixels
		const float scale = age < 0.15f ? 0.6f + age / 0.15f * 0.5f : 1.1f - std::min(0.1f, age - 0.15f);
		const float opacity = age > 0.6f ? 1 - (age - 0.6f) / 0.4f : 1.0f;

		PopupSlot& slot = mPopupSlots[q.slot];
		Rml::Element* element = slot.element;
		shownNow[q.slot] = true;
		if (slot.shownAt != q.t)
		{
			// A new popup in this element: its text and the stylesheet's classes (.free, .six, ...).
			slot.shownAt = q.t;
			slot.opacity = -1.0f;
			element->SetInnerRML(Rml::StringUtilities::EncodeRml(q.text));
			element->SetClass("free", (q.style & dice::Pop_Free) != 0);
			element->SetClass("bump", (q.style & dice::Pop_Bump) != 0);
			element->SetClass("six", (q.style & dice::Pop_Six) != 0);
			element->SetClass("clash", (q.style & dice::Pop_Clash) != 0);
			element->SetClass("stack", (q.style & dice::Pop_Stack) != 0);
			element->SetClass("topple", (q.style & dice::Pop_Topple) != 0);
			element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::Block));
		}
		// transform: translate(x, y) translate(-50%, -50%) scale(s)
		element->SetProperty(Rml::PropertyId::Transform, Rml::Transform::MakeProperty({
			Rml::Transforms::Translate2D(x, y, Rml::Unit::PX), Rml::Transforms::Translate2D(-50.0f, -50.0f, Rml::Unit::PERCENT),
			Rml::Transforms::Scale2D(scale) }));
		if (opacity != slot.opacity)
		{
			slot.opacity = opacity;
			element->SetProperty(Rml::PropertyId::Opacity, Rml::Property(opacity, Rml::Unit::NUMBER));
		}
	}
	mPopups.resize(keep);
	for (size_t i = 0; i < kPopupPool; ++i)
	{
		PopupSlot& slot = mPopupSlots[i];
		if (shownNow[i] || slot.shownAt < 0) continue;
		slot.shownAt = -1.0;
		slot.element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
	}
}

void DiceModule::StartSession()
{
	mAudio.Start();   // the reference starts its audio on Start too
	mSim.StartSession();
}
