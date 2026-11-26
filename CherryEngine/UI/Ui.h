#pragma once
#include "CherryAPI.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Rml { class Context; class ElementDocument; class RenderInterface; }
class Input;

// The game's user interface, built on RmlUi (https://mikke89.github.io/RmlUiDoc/): documents
// written like HTML (.rml) and styled like CSS (.rcss), bound to the game's values through
// data models, drawn over the game view by the renderer (after the tonemap, so in the editor
// viewport too). Games include RmlUi's headers (<RmlUi/Core.h>) and use the context directly.
//
// One context per play session: made before the game's OnStartup and destroyed after its
// OnShutdown, with every document and data model in it, so nothing a hot-reloaded module
// registered outlives its DLL. Don't register RmlUi factories (custom elements, decorators)
// from a game module: those are global and would.
//
// Files: paths are under the project's Content folder ("UI/Hud.rml"); inside documents, links
// are relative to the document, and a leading '/' starts from Content ("/UI/Common.rcss").
// Editing a document's files while playing reloads it: a style sheet change restyles the
// open documents in place, an .rml change loads that document again. A reload replaces the
// ElementDocument, so keep state in data models (which live on the context), or look the
// document up again with Document(path).
//
// Input: the mouse goes to the UI first. While it's over an element, or a press that started
// on one is still held, the game's Input reports no mouse (as over ImGui windows). Give
// elements that shouldn't catch it `pointer-events: none` (a full-screen <body>, for one).
// Keyboard and touch input aren't passed to documents yet.
//
// The `cursor` property works for pointer, text, move and not-allowed (others show the arrow).
// Units: `dp` follows the display's DPI (1dp = 1px at 96 DPI), `px` are view pixels.
// F11 shows RmlUi's debugger (inspect elements and their styles, the event log) while playing.
class CHERRY_API Ui
{
public:
	Ui();
	~Ui();
	Ui(const Ui&) = delete;
	Ui& operator=(const Ui&) = delete;

	// The play session's context (null in edit mode, or without a renderer).
	Rml::Context* GetContext() const;
	// Loads an .rml document from Content (once: a second call returns the same document) and
	// shows it if `show`. Null if it can't be loaded (the log says why).
	Rml::ElementDocument* LoadDocument(const std::string& contentPath, bool show = true);
	// A document loaded with LoadDocument, or null.
	Rml::ElementDocument* Document(const std::string& contentPath) const;
	void CloseDocument(const std::string& contentPath);

	// Adds a font face: a .ttf or .otf file, absolute or under Content. With `family` empty, the
	// family, weight and style come from the file; otherwise the face is registered as that
	// family at `weight` (100..900, 0 = the file's own), italic or not, as documents name them
	// (font-family, font-weight). A `fallback` face is used for characters other faces lack.
	// Faces stay loaded until the engine closes; loading one again does nothing.
	bool LoadFontFace(const std::string& path, const std::string& family = {}, int weight = 0, bool italic = false, bool fallback = false);

	// The mouse is over the UI (or held after a press on it) this frame.
	bool WantsMouse() const;
	// CPU time of the last Update (data bindings, styles, layout, animations), in ms. Drawing is
	// RenderStats::gameUiCpuMs.
	float LastUpdateMs() const;

	void SetDebuggerVisible(bool visible);
	bool IsDebuggerVisible() const;

	// ---- Engine-facing ----
	// Sets RmlUi up to draw through `renderInterface` (Renderer::UiRenderInterface). Without
	// one (no renderer), the UI stays off.
	bool Initialize(Rml::RenderInterface* renderInterface);
	void Shutdown();
	// Play starts / stops: the context comes and goes.
	void BeginSession(const std::filesystem::path& contentRoot, uint32_t viewWidth, uint32_t viewHeight, float dpiScale);
	void EndSession();
	// Before the game's update: sizes the context to the view, gives it the mouse, and takes the
	// mouse away from the game while the UI has it.
	void ProcessInput(Input& input, uint32_t viewWidth, uint32_t viewHeight, float dpiScale);
	// After the game's update: data bindings, layout, animations; reloads changed files.
	void Update(float deltaTime);

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};
