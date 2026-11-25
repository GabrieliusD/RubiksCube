#include "Core/Input.h"
#include "imgui.h"
#if defined(_WIN32)
#include <windowsx.h>
#endif

void Input::OnPointerMove(float x, float y) { mMouseClient = { x, y }; }
void Input::OnPointerButton(MouseButton button, bool down) { mButtons[static_cast<int>(button)] = down; }
void Input::OnWheel(float notches) { mWheel += notches; }
void Input::OnKey(int vk, bool down) { if (vk >= 0 && vk < 256) mKeys[vk] = down; }

#if !defined(_WIN32)
void Input::AttachWindow(void* nativeWindow) { mWindow = nativeWindow; }
#else
void Input::AttachWindow(void* nativeWindow)
{
	mWindow = nativeWindow;
	const HWND hwnd = static_cast<HWND>(nativeWindow);
	// Raw mouse input gives unaccelerated relative motion for mouse look.
	RAWINPUTDEVICE device{};
	device.usUsagePage = 0x01;   // generic desktop
	device.usUsage = 0x02;       // mouse
	device.hwndTarget = hwnd;
	RegisterRawInputDevices(&device, 1, sizeof(device));
}

void Input::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam < 256) mKeys[wParam] = true;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wParam < 256) mKeys[wParam] = false;
		break;
	case WM_LBUTTONDOWN: mButtons[0] = true; break;
	case WM_LBUTTONUP:   mButtons[0] = false; break;
	case WM_RBUTTONDOWN: mButtons[1] = true; break;
	case WM_RBUTTONUP:   mButtons[1] = false; break;
	case WM_MBUTTONDOWN: mButtons[2] = true; break;
	case WM_MBUTTONUP:   mButtons[2] = false; break;
	case WM_MOUSEMOVE:
		mMouseClient = { static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)) };
		break;
	case WM_MOUSEWHEEL:
		mWheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
		break;
	case WM_INPUT:
	{
		RAWINPUT raw{};
		UINT size = sizeof(raw);
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
			&& raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
		{
			mRawDelta.x += static_cast<float>(raw.data.mouse.lLastX);
			mRawDelta.y += static_cast<float>(raw.data.mouse.lLastY);
		}
		break;
	}
	case WM_KILLFOCUS:
		Reset();
		break;
	default:
		break;
	}
}
#endif

void Input::BeginFrame()
{
	mPrevKeys = mKeys;
	mPrevButtons = mButtons;
	mPrevMouseClient = mMouseClient;
	mWheel = 0.0f;
	mRawDelta = { 0, 0 };
}

void Input::SetRouting(bool mouse, bool keyboard, float viewX, float viewY, float viewWidth, float viewHeight, bool textInputActive)
{
	mViewX = viewX;
	mViewY = viewY;
	mViewWidth = viewWidth;
	mViewHeight = viewHeight;

	// Lose capture if the window lost focus (alt-tab, another window clicked, ...).
#if defined(_WIN32)
	if (mMouseCaptured && GetForegroundWindow() != static_cast<HWND>(mWindow))
#else
	if (false)
#endif
	{
		ApplyCapture(false);
	}
	if (mMouseCaptured)
	{
		ClipToView();   // the view can move or resize (window resize, editor panel)
		mMouseEnabled = true;
		mKeyboardEnabled = !textInputActive;
		return;
	}

	const bool anyDown = mButtons[0] || mButtons[1] || mButtons[2];
	if (mouse && anyDown && !mCaptured)
	{
		mCaptured = true;
	}
	if (!anyDown)
	{
		mCaptured = false;
	}
	mMouseEnabled = mouse || mCaptured;
	mKeyboardEnabled = keyboard && !textInputActive;
}

void Input::SetMouseCaptured(bool captured)
{
	if (captured == mMouseCaptured) return;
	// Only take the cursor while our window is the one the user is using.
#if defined(_WIN32)
	if (captured && (!mWindow || GetForegroundWindow() != static_cast<HWND>(mWindow))) return;
#else
	if (captured && !mWindow) return;
#endif
	ApplyCapture(captured);
}

void Input::ApplyCapture(bool captured)
{
	mMouseCaptured = captured;
	ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
#if defined(_WIN32)
	if (captured)
	{
		while (ShowCursor(FALSE) >= 0) {}
		ClipToView();
		// ImGui must not react to the hidden cursor or change its shape.
		if (io) io->ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
	}
	else
	{
		while (ShowCursor(TRUE) < 0) {}
		ClipCursor(nullptr);
		if (io) io->ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
	}
#else
	(void)io;
#endif
}

void Input::ClipToView()
{
#if defined(_WIN32)
	const HWND hwnd = static_cast<HWND>(mWindow);
	if (!hwnd) return;
	POINT topLeft{ static_cast<LONG>(mViewX), static_cast<LONG>(mViewY) };
	POINT bottomRight{ static_cast<LONG>(mViewX + mViewWidth), static_cast<LONG>(mViewY + mViewHeight) };
	ClientToScreen(hwnd, &topLeft);
	ClientToScreen(hwnd, &bottomRight);
	RECT clip{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
	ClipCursor(&clip);
#endif
}

void Input::Reset()
{
	mKeys.fill(false);
	mButtons.fill(false);
	mCaptured = false;
	if (mMouseCaptured)
	{
		ApplyCapture(false);
	}
}

bool Input::IsKeyDown(int vk) const { return mKeyboardEnabled && vk >= 0 && vk < 256 && mKeys[vk]; }
bool Input::WasKeyPressed(int vk) const { return mKeyboardEnabled && vk >= 0 && vk < 256 && mKeys[vk] && !mPrevKeys[vk]; }
bool Input::WasKeyReleased(int vk) const { return mKeyboardEnabled && vk >= 0 && vk < 256 && !mKeys[vk] && mPrevKeys[vk]; }

bool Input::IsMouseDown(MouseButton b) const { return mMouseEnabled && mButtons[static_cast<int>(b)]; }
bool Input::WasMousePressed(MouseButton b) const { const int i = static_cast<int>(b); return mMouseEnabled && mButtons[i] && !mPrevButtons[i]; }
bool Input::WasMouseReleased(MouseButton b) const { const int i = static_cast<int>(b); return !mButtons[i] && mPrevButtons[i]; }

DirectX::XMFLOAT2 Input::MousePosition() const
{
	return { mMouseClient.x - mViewX, mMouseClient.y - mViewY };
}

DirectX::XMFLOAT2 Input::MouseDelta() const
{
	if (!mMouseEnabled) return { 0, 0 };
	if (mMouseCaptured) return mRawDelta;
	return { mMouseClient.x - mPrevMouseClient.x, mMouseClient.y - mPrevMouseClient.y };
}

float Input::WheelDelta() const
{
	return mMouseEnabled ? mWheel : 0.0f;
}
