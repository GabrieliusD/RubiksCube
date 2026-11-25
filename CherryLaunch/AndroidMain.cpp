// CherryLaunch on Android: the app's native library. NativeActivity starts android_main on
// its own thread (android_native_app_glue); it runs the statically linked game like the
// monolithic Windows exe does.
//
// The engine pulls platform events through Window::PumpMessages, which runs the pump set
// here: lifecycle commands (surface created / destroyed, focus, quit) and touch input, with
// one finger as the mouse pointer and its left button.
//
// Files: the APK's assets hold the game's project file and Content/, plus files.txt (written by
// Tools/Android/Package.ps1: a build stamp, then one path per line). They are unpacked into the
// app's internal data folder when the stamp changes, and ExecutableDir() points there, so the
// engine reads them with ordinary file I/O (as a packaged Windows game reads the folder next to
// its exe).
#include <Core/Engine.h>
#include <Core/Log.h>
#include <Core/Project.h>
#include <Game/GameModule.h>
#include <imgui.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

extern "C" IGameModule* CreateGameModule();

namespace
{
	struct AppState
	{
		android_app* app = nullptr;
		Window* window = nullptr;   // set by the first pump
		bool quit = false;
	};

	void OnCommand(android_app* app, int32_t command)
	{
		AppState& state = *static_cast<AppState*>(app->userData);
		switch (command)
		{
		case APP_CMD_INIT_WINDOW:
		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
			if (state.window && app->window)
			{
				state.window->SetNative(app->window, static_cast<uint32_t>(ANativeWindow_getWidth(app->window)),
					static_cast<uint32_t>(ANativeWindow_getHeight(app->window)));
			}
			break;
		case APP_CMD_TERM_WINDOW:
			if (state.window) state.window->SetNative(nullptr, state.window->Width(), state.window->Height());
			break;
		case APP_CMD_GAINED_FOCUS:
			if (state.window) state.window->SetFocused(true);
			break;
		case APP_CMD_LOST_FOCUS:
			if (state.window) state.window->SetFocused(false);
			break;
		case APP_CMD_DESTROY:
			state.quit = true;
			break;
		default:
			break;
		}
	}

	bool ReadAsset(AAssetManager* assets, const std::string& path, std::string& out)
	{
		AAsset* asset = AAssetManager_open(assets, path.c_str(), AASSET_MODE_STREAMING);
		if (!asset) return false;
		out.clear();
		char buffer[64 * 1024];
		int read = 0;
		while ((read = AAsset_read(asset, buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(read));
		AAsset_close(asset);
		return read == 0;
	}

	// Unpacks the APK's game files into `dir` unless the files there carry the same stamp.
	bool UnpackAssets(AAssetManager* assets, const std::filesystem::path& dir)
	{
		namespace fs = std::filesystem;
		std::string manifest;
		if (!ReadAsset(assets, "files.txt", manifest))
		{
			CHERRY_ERROR("The APK has no files.txt: package it with Tools/Android/Package.ps1");
			return false;
		}
		std::istringstream lines(manifest);
		std::string stamp;
		std::getline(lines, stamp);
		const fs::path stampFile = dir / ".cherry-assets";
		std::string installed;
		{
			std::ifstream in(stampFile);
			std::getline(in, installed);
		}
		if (installed == stamp)
		{
			CHERRY_LOG("Game files up to date (%s)", stamp.c_str());
			return true;
		}

		std::vector<std::string> files;
		for (std::string line; std::getline(lines, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty()) files.push_back(line);
		}
		std::error_code ec;
		fs::remove_all(dir / "Content", ec);   // files a newer build no longer has go too
		std::string data;
		for (const std::string& file : files)
		{
			if (!ReadAsset(assets, file, data))
			{
				CHERRY_ERROR("Missing in the APK: %s", file.c_str());
				return false;
			}
			const fs::path target = dir / fs::path(file);
			fs::create_directories(target.parent_path(), ec);
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			out.write(data.data(), static_cast<std::streamsize>(data.size()));
			if (!out)
			{
				CHERRY_ERROR("Couldn't write %s", target.string().c_str());
				return false;
			}
		}
		std::ofstream(stampFile, std::ios::trunc) << stamp << "\n";
		CHERRY_LOG("Unpacked %zu game files (%s)", files.size(), stamp.c_str());
		return true;
	}

	int32_t OnInput(android_app* app, AInputEvent* event)
	{
		AppState& state = *static_cast<AppState*>(app->userData);
		Input* input = state.window ? state.window->GetInput() : nullptr;
		if (!input || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
		const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
		const float x = AMotionEvent_getX(event, 0), y = AMotionEvent_getY(event, 0);
		const bool down = action == AMOTION_EVENT_ACTION_DOWN;
		const bool up = action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL;
		input->OnPointerMove(x, y);
		if (down) input->OnPointerButton(MouseButton::Left, true);
		if (up) input->OnPointerButton(MouseButton::Left, false);
		// ImGui too (on Windows its Win32 backend reads the window messages itself).
		if (ImGui::GetCurrentContext())
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
			io.AddMousePosEvent(x, y);
			if (down) io.AddMouseButtonEvent(0, true);
			if (up) io.AddMouseButtonEvent(0, false);
		}
		return 1;
	}
}

void android_main(android_app* app)
{
	AppState state;
	state.app = app;
	app->userData = &state;
	app->onAppCmd = OnCommand;
	app->onInputEvent = OnInput;

	const std::filesystem::path dataDir = app->activity->internalDataPath ? app->activity->internalDataPath : ".";
	cherry::SetExecutableDir(dataDir);
	CHERRY_LOG("CherryEngine on Android, data folder %s", dataDir.string().c_str());
	if (!UnpackAssets(app->activity->assetManager, dataDir))
	{
		ANativeActivity_finish(app->activity);
		std::exit(1);
	}

	Window::SetPump([&state](Window& window) {
		if (!state.window)
		{
			state.window = &window;
			if (state.app->window) OnCommand(state.app, APP_CMD_INIT_WINDOW);
		}
		// Block while there is no surface or no focus (the engine sleeps then anyway).
		for (;;)
		{
			int events = 0;
			android_poll_source* source = nullptr;
			const int timeout = window.IsMinimized() && !state.quit ? 50 : 0;
			const int result = ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
			if (result == ALOOPER_POLL_ERROR) return false;
			if (source) source->process(state.app, source);
			if (state.app->destroyRequested) state.quit = true;
			if (result == ALOOPER_POLL_TIMEOUT || state.quit) break;
		}
		return !state.quit;
	});

	std::unique_ptr<IGameModule> game(CreateGameModule());
	Engine engine;
	const int exitCode = engine.Run(*game);
	CHERRY_LOG("Engine exited with %d", exitCode);
	game.reset();

	// The engine keeps process-wide state (type registry, ImGui), so a second android_main in
	// this process (the activity started again) would not start clean: end the process. The
	// activity finishes first if the game quit on its own.
	if (!app->destroyRequested) ANativeActivity_finish(app->activity);
	std::exit(exitCode);
}
