#include "ViewportWindow.h"
#include <imgui.h>
#include <imgui_impl_dx12.h>>

void ViewportWindow::Draw()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("Viewport");
	
	mResize = CheckIfNeedsResize();
	if (mResize)
	{
		ImGui::End();
		ImGui::PopStyleVar();
		return;
	}

	ImGui::Image((ImTextureID)mRenderTexture->GetSrvDescriptorHandle().gpu.ptr, ImVec2(mWidth, mHeight));

	ImGui::End();
	ImGui::PopStyleVar();
}

void ViewportWindow::HandleWindowResize()
{
	if (mResize)
	{
		mRenderTexture->SizeResources(mWidth, mHeight);
		mResize = false;
	}
}

bool ViewportWindow::CheckIfNeedsResize()
{
	ImVec2 view = ImGui::GetContentRegionAvail();

	if (view.x != mWidth || view.y != mHeight)
	{
		if (mWidth == 0 || mHeight == 0)
		{
			return false;
		}

		mWidth = view.x;
		mHeight = view.y;

		return true;
	}

	return false;
}