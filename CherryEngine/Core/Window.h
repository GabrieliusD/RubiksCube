#pragma once
#include "CherryAPI.h"
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
