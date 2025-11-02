#pragma once
#include <Graphics\RenderTexture.h>

class ViewportWindow
{
public:
	ViewportWindow(RenderTexture* renderTexture) 
	{
		mRenderTexture = renderTexture;
		mWidth = 800;
		mHeight = 581;
	}
	void Draw();
	void HandleWindowResize();
	bool CheckIfNeedsResize();
private:
	int mWidth;
	int mHeight;
	RenderTexture* mRenderTexture;
	bool mResize = false;
};