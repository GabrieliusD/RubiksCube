#pragma once
#include "CherryAPI.h"
#include <DirectXMath.h>
#include <windows.h>
#include <array>
#include <cstdint>

enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2 };

// Keyboard and mouse state for the game. The engine routes input here only
// while the game owns it (editor viewport hovered/focused, or game mode
// without ImGui capturing it); otherwise queries report nothing pressed.
// Mouse coordinates are in pixels relative to the game view's top-left corner.
//
// Mouse capture (first-person style): SetMouseCaptured(true) hides the cursor,
// confines it to the game view and switches MouseDelta() to raw, unaccelerated
// movement. Capture is only granted while the window has focus, and is released
// automatically when focus is lost or the editor/game mode changes.
class CHERRY_API Input
{
public:
	bool IsKeyDown(int virtualKey) const;
	bool WasKeyPressed(int virtualKey) const;    // went down this frame
	bool WasKeyReleased(int virtualKey) const;   // went up this frame

	bool IsMouseDown(MouseButton button) const;
	bool WasMousePressed(MouseButton button) const;
	bool WasMouseReleased(MouseButton button) const;
	DirectX::XMFLOAT2 MousePosition() const;     // view-relative pixels
	DirectX::XMFLOAT2 MouseDelta() const;        // pixels moved this frame
	float WheelDelta() const;                    // notches this frame

	bool HasMouse() const { return mMouseEnabled; }
	bool HasKeyboard() const { return mKeyboardEnabled; }

	void SetMouseCaptured(bool captured);
	bool IsMouseCaptured() const { return mMouseCaptured; }

	// ---- Engine-facing ----
	void AttachWindow(HWND hwnd);                // enables raw mouse input
	void HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
	void BeginFrame();                           // call before pumping messages
	// View rectangle in window client pixels. While captured, the mouse and keyboard
	// stay routed to the game regardless of the other flags (except text input).
	void SetRouting(bool mouse, bool keyboard, float viewX, float viewY, float viewWidth, float viewHeight, bool textInputActive);
	void Reset();

private:
	std::array<bool, 256> mKeys{};
	std::array<bool, 256> mPrevKeys{};
	std::array<bool, 3> mButtons{};
	std::array<bool, 3> mPrevButtons{};
	DirectX::XMFLOAT2 mMouseClient{ 0, 0 };
	DirectX::XMFLOAT2 mPrevMouseClient{ 0, 0 };
	float mWheel = 0.0f;
	float mViewX = 0.0f, mViewY = 0.0f;
	bool mMouseEnabled = false;
	bool mKeyboardEnabled = false;
	bool mCaptured = false;    // a button went down over the view; keep the mouse until release

	void ApplyCapture(bool captured);
	void ClipToView();

	HWND mHwnd = nullptr;
	bool mMouseCaptured = false;
	DirectX::XMFLOAT2 mRawDelta{ 0, 0 };
	float mViewWidth = 0.0f, mViewHeight = 0.0f;
};
