#include "Core/Window.h"
#include "imgui.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	const wchar_t* kWindowClass = L"CherryEngineWindow";
}

bool Window::Create(const std::wstring& title, uint32_t width, uint32_t height)
{
	HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = &Window::StaticWndProc;
	wc.hInstance = instance;
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kWindowClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		return false;
	}

	// Render at native resolution on high-DPI displays instead of being bitmap-stretched.
	// `width`/`height` are in 96-DPI units and scaled to the monitor's DPI.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const UINT dpi = GetDpiForSystem();
	mDpiScale = dpi / 96.0f;

	RECT rect{ 0, 0, static_cast<LONG>(width * mDpiScale), static_cast<LONG>(height * mDpiScale) };
	AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
	mHwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
		nullptr, nullptr, instance, this);
	if (!mHwnd)
	{
		return false;
	}

	RECT client{};
	GetClientRect(mHwnd, &client);
	mWidth = static_cast<uint32_t>(client.right - client.left);
	mHeight = static_cast<uint32_t>(client.bottom - client.top);

	ShowWindow(mHwnd, SW_SHOW);
	UpdateWindow(mHwnd);
	return true;
}

void Window::Destroy()
{
	if (mHwnd)
	{
		DestroyWindow(mHwnd);
		mHwnd = nullptr;
	}
	UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

void Window::SetTitle(const std::wstring& title)
{
	SetWindowTextW(mHwnd, title.c_str());
}

bool Window::PumpMessages()
{
	MSG msg{};
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			mQuit = true;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return !mQuit;
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_NCCREATE)
	{
		auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (self)
	{
		return self->WndProc(hwnd, msg, wParam, lParam);
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
	{
		return 1;
	}

	if (mMessageHandler)
	{
		mMessageHandler(msg, wParam, lParam);
	}

	switch (msg)
	{
	case WM_SIZE:
		mMinimized = wParam == SIZE_MINIMIZED;
		if (!mMinimized)
		{
			mWidth = LOWORD(lParam);
			mHeight = HIWORD(lParam);
			if (mResizeHandler && mWidth > 0 && mHeight > 0)
			{
				mResizeHandler(mWidth, mHeight);
			}
		}
		return 0;
	case WM_DPICHANGED:
	{
		mDpiScale = HIWORD(wParam) / 96.0f;
		const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
		SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
			suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_GETMINMAXINFO:
		reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { 320, 240 };
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;   // no menu beep on Alt
		break;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		mHwnd = nullptr;
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}
