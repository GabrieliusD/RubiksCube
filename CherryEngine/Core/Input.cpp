#include "Core/Input.h"
#include <windowsx.h>

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
	case WM_KILLFOCUS:
		Reset();
		break;
	default:
		break;
	}
}

void Input::BeginFrame()
{
	mPrevKeys = mKeys;
	mPrevButtons = mButtons;
	mPrevMouseClient = mMouseClient;
	mWheel = 0.0f;
}

void Input::SetRouting(bool mouse, bool keyboard, float viewX, float viewY)
{
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
	mKeyboardEnabled = keyboard;
	mViewX = viewX;
	mViewY = viewY;
}

void Input::Reset()
{
	mKeys.fill(false);
	mButtons.fill(false);
	mCaptured = false;
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
	return { mMouseClient.x - mPrevMouseClient.x, mMouseClient.y - mPrevMouseClient.y };
}

float Input::WheelDelta() const
{
	return mMouseEnabled ? mWheel : 0.0f;
}
