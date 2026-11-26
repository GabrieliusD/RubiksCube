#include "UI/Ui.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "imgui.h"
#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kContextName = "game";
	constexpr float kWatchInterval = 0.5f;   // seconds between checks for edited UI files

	// Log lines and the clock.
	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		double GetElapsedTime() override
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
		}
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
		{
			switch (type)
			{
			case Rml::Log::LT_ERROR:
			case Rml::Log::LT_ASSERT: CHERRY_ERROR("UI: %s", message.c_str()); break;
			case Rml::Log::LT_WARNING: CHERRY_WARN("UI: %s", message.c_str()); break;
			case Rml::Log::LT_INFO:
			case Rml::Log::LT_ALWAYS: CHERRY_LOG("UI: %s", message.c_str()); break;
			default: break;   // LT_DEBUG
			}
			return true;
		}
		// The `cursor` property of what's under the mouse ("pointer", "text", "move"...).
		void SetMouseCursor(const Rml::String& name) override { cursor = name; }
		Rml::String cursor;
	private:
		std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
	};

	// Files: relative paths are under the project's Content folder; UTF-8 names work on Windows.
	class FileInterface final : public Rml::FileInterface
	{
	public:
		fs::path root;

		fs::path Resolve(const Rml::String& path) const
		{
			fs::path p = fs::u8path(path);
			if (p.is_relative() && !root.empty()) p = root / p;
			return p;
		}
		Rml::FileHandle Open(const Rml::String& path) override
		{
#if defined(_WIN32)
			std::FILE* file = _wfopen(Resolve(path).c_str(), L"rb");
#else
			std::FILE* file = std::fopen(Resolve(path).c_str(), "rb");
#endif
			return reinterpret_cast<Rml::FileHandle>(file);
		}
		void Close(Rml::FileHandle file) override { std::fclose(reinterpret_cast<std::FILE*>(file)); }
		size_t Read(void* buffer, size_t size, Rml::FileHandle file) override { return std::fread(buffer, 1, size, reinterpret_cast<std::FILE*>(file)); }
		bool Seek(Rml::FileHandle file, long offset, int origin) override { return std::fseek(reinterpret_cast<std::FILE*>(file), offset, origin) == 0; }
		size_t Tell(Rml::FileHandle file) override { return static_cast<size_t>(std::ftell(reinterpret_cast<std::FILE*>(file))); }
	};

	fs::file_time_type WriteTime(const fs::path& path)
	{
		std::error_code error;
		const fs::file_time_type time = fs::last_write_time(path, error);
		return error ? fs::file_time_type::min() : time;
	}
}

struct Ui::Impl
{
	SystemInterface system;
	FileInterface files;
	bool initialized = false;
	Rml::Context* context = nullptr;
	bool debuggerReady = false;

	struct Document
	{
		Rml::ElementDocument* document = nullptr;
		bool shown = false;
	};
	std::map<std::string, Document> documents;   // by Content path

	// Font faces loaded so far (path + family + weight + style) and the bytes of those loaded
	// from memory, which RmlUi reads until it shuts down.
	std::set<std::string> fontKeys;
	std::deque<std::vector<Rml::byte>> fontData;

	// Edited files: the UI files (.rml, .rcss) in the folders of the open documents.
	std::map<fs::path, fs::file_time_type> watched;
	float watchTimer = 0.0f;

	bool mouseInside = false;
	bool wantsMouse = false;
	float updateMs = 0.0f;
	uint32_t width = 0, height = 0;
	float dpi = 0.0f;

	void WatchFolderOf(const std::string& contentPath);
	void PollChanges();
};

Ui::Ui() : mImpl(std::make_unique<Impl>()) {}
Ui::~Ui() = default;

bool Ui::Initialize(Rml::RenderInterface* renderInterface)
{
	Impl& u = *mImpl;
	if (u.initialized || !renderInterface) return u.initialized;
	Rml::SetSystemInterface(&u.system);
	Rml::SetFileInterface(&u.files);
	Rml::SetRenderInterface(renderInterface);
	if (!Rml::Initialise())
	{
		CHERRY_ERROR("UI: RmlUi didn't start");
		return false;
	}
	u.initialized = true;
	return true;
}

void Ui::Shutdown()
{
	Impl& u = *mImpl;
	if (!u.initialized) return;
	EndSession();
	Rml::Shutdown();   // releases the font textures through the render interface
	u.fontData.clear();
	u.fontKeys.clear();
	u.initialized = false;
}

void Ui::BeginSession(const fs::path& contentRoot, uint32_t viewWidth, uint32_t viewHeight, float dpiScale)
{
	Impl& u = *mImpl;
	if (!u.initialized) return;
	EndSession();
	u.files.root = contentRoot;
	u.width = std::max(viewWidth, 1u);
	u.height = std::max(viewHeight, 1u);
	u.dpi = dpiScale > 0.0f ? dpiScale : 1.0f;
	u.context = Rml::CreateContext(kContextName, Rml::Vector2i(static_cast<int>(u.width), static_cast<int>(u.height)));
	if (!u.context)
	{
		CHERRY_ERROR("UI: couldn't make the game's context");
		return;
	}
	u.context->SetDensityIndependentPixelRatio(u.dpi);
	u.debuggerReady = Rml::Debugger::Initialise(u.context);
}

void Ui::EndSession()
{
	Impl& u = *mImpl;
	if (!u.context) return;
	if (u.debuggerReady) Rml::Debugger::Shutdown();
	u.debuggerReady = false;
	Rml::RemoveContext(kContextName);   // its documents, data models and event listeners go with it
	u.context = nullptr;
	u.documents.clear();
	u.watched.clear();
	u.mouseInside = false;
	u.wantsMouse = false;
	// Documents of the next session (maybe a reloaded module's) read their files again.
	Rml::Factory::ClearStyleSheetCache();
	Rml::Factory::ClearTemplateCache();
}

Rml::Context* Ui::GetContext() const { return mImpl->context; }

Rml::ElementDocument* Ui::LoadDocument(const std::string& contentPath, bool show)
{
	Impl& u = *mImpl;
	if (!u.context) return nullptr;
	auto found = u.documents.find(contentPath);
	if (found != u.documents.end() && found->second.document)
	{
		if (show && !found->second.shown) { found->second.document->Show(); found->second.shown = true; }
		return found->second.document;
	}
	Rml::ElementDocument* document = u.context->LoadDocument(contentPath);
	if (!document)
	{
		CHERRY_ERROR("UI: can't load %s", contentPath.c_str());
		return nullptr;
	}
	if (show) document->Show();
	u.documents[contentPath] = { document, show };
	u.WatchFolderOf(contentPath);
	return document;
}

Rml::ElementDocument* Ui::Document(const std::string& contentPath) const
{
	auto found = mImpl->documents.find(contentPath);
	return found != mImpl->documents.end() ? found->second.document : nullptr;
}

void Ui::CloseDocument(const std::string& contentPath)
{
	auto found = mImpl->documents.find(contentPath);
	if (found == mImpl->documents.end()) return;
	if (found->second.document) found->second.document->Close();
	mImpl->documents.erase(found);
}

bool Ui::LoadFontFace(const std::string& path, const std::string& family, int weight, bool italic, bool fallback)
{
	Impl& u = *mImpl;
	if (!u.initialized) return false;
	const std::string key = path + "|" + family + "|" + std::to_string(weight) + (italic ? "|i" : "|n");
	if (u.fontKeys.count(key)) return true;
	const Rml::Style::FontWeight rmlWeight = weight > 0 ? static_cast<Rml::Style::FontWeight>(weight) : Rml::Style::FontWeight::Auto;
	bool ok = false;
	if (family.empty())
	{
		ok = Rml::LoadFontFace(path, fallback, rmlWeight);
	}
	else
	{
		Rml::String bytes;
		if (Rml::GetFileInterface()->LoadFile(path, bytes) && !bytes.empty())
		{
			std::vector<Rml::byte>& data = u.fontData.emplace_back(bytes.begin(), bytes.end());
			ok = Rml::LoadFontFace(Rml::Span<const Rml::byte>(data.data(), data.size()), family,
				italic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal, rmlWeight, fallback);
			if (!ok) u.fontData.pop_back();
		}
	}
	if (ok) u.fontKeys.insert(key);
	else CHERRY_WARN("UI: can't load the font %s", path.c_str());
	return ok;
}

bool Ui::WantsMouse() const { return mImpl->wantsMouse; }
float Ui::LastUpdateMs() const { return mImpl->updateMs; }

void Ui::SetDebuggerVisible(bool visible)
{
	if (mImpl->debuggerReady) Rml::Debugger::SetVisible(visible);
}

bool Ui::IsDebuggerVisible() const
{
	return mImpl->debuggerReady && Rml::Debugger::IsVisible();
}

void Ui::ProcessInput(Input& input, uint32_t viewWidth, uint32_t viewHeight, float dpiScale)
{
	Impl& u = *mImpl;
	u.wantsMouse = false;
	if (!u.context) return;
	viewWidth = std::max(viewWidth, 1u);
	viewHeight = std::max(viewHeight, 1u);
	if (viewWidth != u.width || viewHeight != u.height)
	{
		u.width = viewWidth;
		u.height = viewHeight;
		u.context->SetDimensions(Rml::Vector2i(static_cast<int>(u.width), static_cast<int>(u.height)));
	}
	if (dpiScale > 0.0f && dpiScale != u.dpi)
	{
		u.dpi = dpiScale;
		u.context->SetDensityIndependentPixelRatio(u.dpi);
	}

	// A captured mouse (first-person look) has no pointer to point with.
	const bool pointer = input.HasMouse() && !input.IsMouseCaptured();
	constexpr MouseButton kButtons[] = { MouseButton::Left, MouseButton::Right, MouseButton::Middle };   // RmlUi's 0, 1, 2
	if (pointer)
	{
		const DirectX::XMFLOAT2 at = input.MousePosition();
		u.context->ProcessMouseMove(static_cast<int>(at.x), static_cast<int>(at.y), 0);
		u.mouseInside = true;
		for (int b = 0; b < 3; ++b)
			if (input.WasMousePressed(kButtons[b])) u.context->ProcessMouseButtonDown(b, 0);
		if (input.WheelDelta() != 0.0f) u.context->ProcessMouseWheel(-input.WheelDelta(), 0);
	}
	// A release anywhere ends a press (also one that ended outside the view).
	for (int b = 0; b < 3; ++b)
		if (input.WasMouseReleased(kButtons[b])) u.context->ProcessMouseButtonUp(b, 0);
	if (!pointer && u.mouseInside)
	{
		u.context->ProcessMouseLeave();
		u.mouseInside = false;
	}

	u.wantsMouse = u.mouseInside && u.context->IsMouseInteracting();
	if (u.wantsMouse) input.SuppressMouse();
}

void Ui::Update(float deltaTime)
{
	Impl& u = *mImpl;
	if (!u.context) return;
	u.watchTimer += deltaTime;
	if (u.watchTimer >= kWatchInterval)
	{
		u.watchTimer = 0.0f;
		u.PollChanges();
	}
	const auto start = std::chrono::steady_clock::now();
	u.context->Update();
	u.updateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

	// ImGui owns the OS cursor and resets it every frame: ask again while over the UI.
	if (u.wantsMouse)
	{
		const Rml::String& cursor = u.system.cursor;
		if (cursor == "pointer") ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		else if (cursor == "text") ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		else if (cursor == "move") ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		else if (cursor == "not-allowed") ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
		else ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
	}
}

// ---- Hot reload ----------------------------------------------------------------------------

void Ui::Impl::WatchFolderOf(const std::string& contentPath)
{
	const fs::path folder = (files.root / fs::u8path(contentPath)).parent_path();
	std::error_code error;
	for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_regular_file(error)) continue;
		const fs::path extension = it->path().extension();
		if (extension != ".rml" && extension != ".rcss") continue;
		if (!watched.count(it->path())) watched[it->path()] = WriteTime(it->path());
	}
}

void Ui::Impl::PollChanges()
{
	bool styles = false;
	std::set<fs::path> changedDocuments;
	for (auto& [path, time] : watched)
	{
		const fs::file_time_type now = WriteTime(path);
		if (now == time) continue;
		time = now;
		if (path.extension() == ".rcss") styles = true;
		else changedDocuments.insert(path);
	}
	if (!styles && changedDocuments.empty()) return;
	Rml::Factory::ClearStyleSheetCache();
	Rml::Factory::ClearTemplateCache();

	for (auto& [contentPath, entry] : documents)
	{
		const fs::path file = files.root / fs::u8path(contentPath);
		bool reload = false;
		for (const fs::path& changed : changedDocuments)
		{
			std::error_code error;
			if (fs::equivalent(changed, file, error)) reload = true;
		}
		// An .rml that isn't itself a document (a template) may be used by any of them.
		if (!reload && !changedDocuments.empty())
		{
			bool isDocument = false;
			for (const fs::path& changed : changedDocuments)
				for (const auto& [otherPath, other] : documents)
				{
					std::error_code error;
					if (fs::equivalent(changed, files.root / fs::u8path(otherPath), error)) isDocument = true;
				}
			if (!isDocument) reload = true;
		}
		if (reload)
		{
			if (entry.document) entry.document->Close();
			entry.document = context->LoadDocument(contentPath);
			if (!entry.document)
			{
				CHERRY_ERROR("UI: reloading %s failed (fix it and save again)", contentPath.c_str());
				continue;
			}
			if (entry.shown) entry.document->Show();
			CHERRY_LOG("UI: reloaded %s", contentPath.c_str());
		}
		else if (styles && entry.document)
		{
			entry.document->ReloadStyleSheet();
		}
	}
	if (styles) CHERRY_LOG("UI: style sheets reloaded");
}
