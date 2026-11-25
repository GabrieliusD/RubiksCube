#include "Core/Window.h"

namespace
{
	Window::Pump sPump;
}

void Window::SetPump(Pump pump) { sPump = std::move(pump); }

bool Window::Create(const std::wstring&, uint32_t width, uint32_t height)
{
	// The size comes from the surface once the platform layer has one (SetNative).
	if (mWidth == 0) { mWidth = width; mHeight = height; }
	return true;
}

void Window::Destroy() { mNative = nullptr; }

bool Window::PumpMessages() { return sPump ? sPump(*this) : false; }

void Window::SetNative(void* native, uint32_t width, uint32_t height)
{
	const bool resized = width != mWidth || height != mHeight;
	mNative = native;
	mWidth = width;
	mHeight = height;
	if (resized && native && mResizeHandler) mResizeHandler(width, height);
}
