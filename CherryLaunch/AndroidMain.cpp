// CherryLaunch on Android: the app's native library. NativeActivity starts android_main on
// its own thread (android_native_app_glue); it runs the statically linked game like the
// monolithic Windows exe does.
//
// The engine pulls platform events through Window::PumpMessages, which runs the pump set
// here: lifecycle commands (surface created / destroyed, focus, quit) and touch input, with
// one finger as the mouse pointer and its left button.
//
// Files: ExecutableDir() is the app's internal data folder; the game's Content/ and project
// file are expected there (unpacking them from the APK comes with the platform layer, A1).
#include <Core/Engine.h>
#include <Core/Log.h>
#include <Core/Project.h>
#include <Game/GameModule.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <memory>

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

	int32_t OnInput(android_app* app, AInputEvent* event)
	{
		AppState& state = *static_cast<AppState*>(app->userData);
		Input* input = state.window ? state.window->GetInput() : nullptr;
		if (!input || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
		const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
		input->OnPointerMove(AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));
		if (action == AMOTION_EVENT_ACTION_DOWN) input->OnPointerButton(MouseButton::Left, true);
		if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) input->OnPointerButton(MouseButton::Left, false);
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

	if (app->activity->internalDataPath) cherry::SetExecutableDir(app->activity->internalDataPath);
	CHERRY_LOG("CherryEngine on Android, data folder %s", cherry::ExecutableDir().string().c_str());

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
}
