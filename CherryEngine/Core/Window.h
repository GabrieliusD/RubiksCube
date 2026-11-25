#pragma once
#include "CherryAPI.h"
#if defined(_WIN32)
#include <windows.h>
#include <functional>
#include <string>
#include <cstdint>

// Win32 window. Messages go to ImGui first, then to the registered handler.
class CHERRY_API Window
{
public:
	using MessageHandler = std::function<void(UINT msg, WPARAM wParam, LPARAM lParam)>;
	using ResizeHandler = std::function<void(uint32_t width, uint32_t height)>;

	bool Create(const std::wstring& title, uint32_t width, uint32_t height);
	void Destroy();

	// Processes pending messages. Returns false once the window was closed.
	bool PumpMessages();

	void SetMessageHandler(MessageHandler handler) { mMessageHandler = std::move(handler); }
	void SetResizeHandler(ResizeHandler handler) { mResizeHandler = std::move(handler); }
	void SetTitle(const std::wstring& title);

	HWND Handle() const { return mHwnd; }
	uint32_t Width() const { return mWidth; }
	uint32_t Height() const { return mHeight; }
	bool IsMinimized() const { return mMinimized; }
	float DpiScale() const { return mDpiScale; }   // 1.0 at 96 DPI

private:
	static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	HWND mHwnd = nullptr;
	uint32_t mWidth = 0;
	uint32_t mHeight = 0;
	bool mMinimized = false;
	float mDpiScale = 1.0f;
	bool mQuit = false;
	MessageHandler mMessageHandler;
	ResizeHandler mResizeHandler;
};

#else

#include <functional>
#include <string>
#include <cstdint>

class Input;

// Android: the activity's surface (an ANativeWindow*). The platform layer (Platform/Android)
// owns the event loop: PumpMessages calls the pump it installed, which feeds Input and
// reports size changes, focus and whether the app should keep running.
class CHERRY_API Window
{
public:
	using ResizeHandler = std::function<void(uint32_t width, uint32_t height)>;
	// Processes pending platform events; returns false once the app should quit.
	using Pump = std::function<bool(Window& window)>;

	bool Create(const std::wstring& title, uint32_t width, uint32_t height);
	void Destroy();
	bool PumpMessages();

	void SetResizeHandler(ResizeHandler handler) { mResizeHandler = std::move(handler); }
	void SetTitle(const std::wstring&) {}
	void SetInput(Input* input) { mInput = input; }

	void* Handle() const { return mNative; }
	uint32_t Width() const { return mWidth; }
	uint32_t Height() const { return mHeight; }
	bool IsMinimized() const { return mNative == nullptr || !mFocused; }
	float DpiScale() const { return mDpiScale; }

	// ---- Platform-facing ----
	static void SetPump(Pump pump);
	Input* GetInput() const { return mInput; }
	void SetNative(void* native, uint32_t width, uint32_t height);
	void SetFocused(bool focused) { mFocused = focused; }
	void SetDpiScale(float scale) { mDpiScale = scale; }

private:
	void* mNative = nullptr;
	uint32_t mWidth = 0;
	uint32_t mHeight = 0;
	bool mFocused = true;
	float mDpiScale = 1.0f;
	Input* mInput = nullptr;
	ResizeHandler mResizeHandler;
};

#endif
