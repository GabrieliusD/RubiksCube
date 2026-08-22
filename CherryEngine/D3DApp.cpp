#include "D3DApp.h"
#include <WindowsX.h>
#include "GeometryGenerator.h"
#include <iostream>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <Entity.h>
#include <RenderSystem.h>
#include <CameraSystem.h>
#include <Camera.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
		return true;

	auto* app = D3DApp::GetApp();
	if (app)
	{
		return app->MsgProc(hwnd, message, wparam, lparam);
	}

	return DefWindowProc(hwnd, message, wparam, lparam);
}


D3DApp::D3DApp()
{
	assert(mApp == nullptr);
	mApp = this;
}

D3DApp* D3DApp::GetApp()
{
	return mApp;
}
D3DApp* D3DApp::mApp = nullptr;
D3DApp::~D3DApp()
{
	if (mRenderSystem)
	{
		try
		{
			mRenderSystem->FlushCommandQueue();
		}
		catch (...)
		{
		}
	}

	mApp = nullptr;
}

void D3DApp::InitDirectX()
{
	mD3DCore.Initialize();

	InitECS();

	mRenderSystem->CmdListCloseAndExecute();

	// Wait until initialization is complete.
	mRenderSystem->FlushCommandQueue();

	OnAppInitialized();
}

bool D3DApp::InitWindow()
{
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = nullptr; //todo;
	wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";

	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	RECT R = { 0,0,mClientWidth, mClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;
	mhMainWnd = CreateWindow(L"MainWnd", mMainWndCaption.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, nullptr, 0);

	if (!mhMainWnd)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}

	ShowWindow(mhMainWnd, SW_SHOW);
	UpdateWindow(mhMainWnd);
	return true;
}

void D3DApp::InitVrHeadset()
{

}

void D3DApp::InitECS()
{
	auto& coordinator = mScene.GetCoordinator();
	coordinator.Init();

	coordinator.RegisterComponent<Transform>();
	coordinator.RegisterComponent<Renderable>();
	coordinator.RegisterComponent<Camera>();

	mCameraSystem = coordinator.RegisterSystem<CameraSystem>();
	mRenderSystem = coordinator.RegisterSystem<RenderSystem>();

	Signature renderSignature;
	renderSignature.set(coordinator.GetComponentType<Transform>());
	renderSignature.set(coordinator.GetComponentType<Renderable>());

	coordinator.SetSystemSignature<RenderSystem>(renderSignature);

	Signature cameraSignature;
	cameraSignature.set(coordinator.GetComponentType<Transform>());
	cameraSignature.set(coordinator.GetComponentType<Camera>());

	coordinator.SetSystemSignature<CameraSystem>(cameraSignature);

	mCameraSystem->Init();

	RenderSystemParams renderSystemParams;
	renderSystemParams.hwnd = mhMainWnd;

	Entity camera = mCameraSystem->GetCamera();
	renderSystemParams.camera = camera;

	mRenderSystem->Init(renderSystemParams);
}

void D3DApp::InitImgui()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(mhMainWnd);

	auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
	handle.Offset(mImGuiOffset, mCbvSrvDescriptorSize);
	auto gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
	gpuHandle.Offset(mImGuiOffset, mCbvSrvDescriptorSize);
	ImGui_ImplDX12_Init(mDevice, 1,
		DXGI_FORMAT_R8G8B8A8_UNORM, mCbvHeap.Get(),
		handle,
		gpuHandle);
}

void D3DApp::RenderImgui()
{
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
		//ImGui::ShowDemoWindow();

	// 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
	{
		static float f = 0.0f;
		static int counter = 0;

		ImGui::Begin("Rubik Controls", nullptr, 
			ImGuiWindowFlags_MenuBar | 
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus);                          // Create a window called "Hello, world!" and append into it.

		ImGui::Text("[Left Mouse Button] Rotate Camera");
		ImGui::Text("[Right Mouse Button] Select Face");
		ImGui::Text("[Q] Rotate face anticlockwise");               
		ImGui::Text("[E] Rotate face clockwise");      
		if(ImGui::Button("Scramble"))
		{
			std::string randomScramble = mRubikCube.GenerateRandomScrambleString(20);
			std::cout << randomScramble << std::endl;
			mRubikCube.Scramble(randomScramble);
		}
		VictoryScreen(mRubikCube.GetVictory());

		ImGui::End();
	}

	// Rendering
	ImGui::Render();
}

void D3DApp::VictoryScreen(bool Enable)
{
	if (!Enable)
	{
		if (ImGui::IsPopupOpen("Victory!!!"))
			ImGui::CloseCurrentPopup();
		return;
	}
	ImGui::OpenPopup("Victory!!!");
	ImGui::BeginPopupModal("Victory!!!", NULL, ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::Text("You have completed the Rubik cube");
	if (ImGui::Button("Restart"))
	{
		ImGui::CloseCurrentPopup();
		mRubikCube.SetVictory(false);
	}
	ImGui::SameLine(0, 140.0f);
	ImGui::Button("Quit");
	ImGui::EndPopup();
}

int D3DApp::Run()
{
	InitWindow();
	InitDirectX();
	MSG msg = { 0 };
	
	mTimer.Reset();
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			mTimer.Tick();
			if (!mAppPaused)
			{
				try
				{
					mRenderSystem->Update(mTimer.DeltaTime());
				}
				catch (...)
				{
					PostQuitMessage(0);
				}
			}
			else
			{
				Sleep(100);
			}
		}
	}

	if (mRenderSystem)
	{
		try
		{
			mRenderSystem->FlushCommandQueue();
		}
		catch (...)
		{
		}
	}

	mRenderSystem.reset();
	mCameraSystem.reset();
	D3DCore::Shutdown();

	return (int)msg.wParam;
}

LRESULT D3DApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CHAR: //this is just for a program exit besides window's borders/taskbar
		if (wParam == VK_ESCAPE)
		{
			DestroyWindow(hwnd);
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_MOUSEMOVE:
		OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_KEYDOWN:
		OnKeyDown(wParam);
		return 0;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

void D3DApp::CalculateFrameStats()
{
	static int frameCnt = 0;
	static float timeElapsed = 0.0f;

	frameCnt++;

	// Compute averages over one second period.
	if ((mTimer.TotalTime() - timeElapsed) >= 1.0f)
	{
		float fps = (float)frameCnt; // fps = frameCnt / 1
		float mspf = 1000.0f / fps;

		std::wstring fpsStr = std::to_wstring(fps);
		std::wstring mspfStr = std::to_wstring(mspf);

		std::wstring windowText = mMainWndCaption +
			L"    fps: " + fpsStr +
			L"   mspf: " + mspfStr;

		SetWindowText(mhMainWnd, windowText.c_str());

		// Reset for next average.
		frameCnt = 0;
		timeElapsed += 1.0f;
	}
}
void D3DApp::Update(GameTimer& mTimer)
{
	mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
	mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
	mEyePos.y = mRadius * cosf(mPhi);

	// Build the view matrix.
	//XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR pos = XMVectorSet(0, 0, 0, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX view;
	{
		pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);

		view = XMMatrixLookAtLH(pos, target, up);
	}
	XMStoreFloat4x4(&mView, view);

	XMMATRIX world = XMLoadFloat4x4(&mWorld);
	world = XMMatrixRotationAxis(XMVectorSet(0, 1.0f, 0.0f, 1.0f), 0.0005f) * world;
	XMStoreFloat4x4(&mWorld, world);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX worldViewProj = world * view * proj;

	UpdateRubikCubeInstances();

	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	for (int i = 0; mAllRenderObjects.size() != i; i++)
	{
		ObjectConstants objConstants;
		RenderObject& ro = *mAllRenderObjects[i];
		XMMATRIX objWorld = XMLoadFloat4x4(&ro.world);
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(objWorld));
		mObjectConstantsBuffer->CopyData(i, objConstants);
	}

	for (int i = 0; mSkyObjects.size() != i; i++)
	{
		ObjectConstants objConstants;
		RenderObject& ro = *mSkyObjects[i];
		XMMATRIX objWorld = XMLoadFloat4x4(&ro.world);
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(objWorld));
		mObjectConstantsBuffer->CopyData(mAllRenderObjects.size() + i, objConstants);
	}

	if (d3dUtil::IsKeyDown('W'))
	{
		for (int i = 0; i != mAllRenderObjects.size(); i++)
		{
			RenderObject& ro = *mAllRenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.world);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			
			if ( MathHelper::AlmostSame(transStored.z,2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(0, 0.0f, 1.0f, 1.0f), DirectX::XM_PIDIV2 * 0.001f);
				XMStoreFloat4x4(&ro.world, tempWorld);
			}
		}
	}
	if (d3dUtil::IsKeyDown('A'))
	{
		for (int i = 0; i != mAllRenderObjects.size(); i++)
		{
			RenderObject& ro = *mAllRenderObjects[i];
			XMMATRIX tempWorld = XMLoadFloat4x4(&ro.world);
			XMVECTOR trans;
			XMVECTOR scale;
			XMVECTOR rot;
			XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
			XMFLOAT3 transStored;
			XMStoreFloat3(&transStored, trans);
			if (MathHelper::AlmostSame(transStored.x, 2.0f))
			{
				tempWorld = tempWorld * XMMatrixRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f), DirectX::XM_PIDIV2);
				XMStoreFloat4x4(&ro.world, tempWorld);
			}
		}
	}

}

void D3DApp::UpdateRubikCubeInstances()
{
	std::vector<Cube*> cubes = mRubikCube.GetAllCubies();
	for (int i = 0; i != mInstances.size(); i++)
	{
		mInstances[i].second = XMLoadFloat4x4(&cubes[i]->world);
	}
}

void D3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		mLastMousePos.x = x;
		mLastMousePos.y = y;

		SetCapture(mhMainWnd);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		Pick(x, y);
	}
}

void D3DApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void D3DApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

		// Update angles based on input to orbit camera around box.
		mTheta += dx;
		mPhi += dy;

		// Restrict the angle mPhi.
		mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		// Make each pixel correspond to 0.005 unit in the scene.
		float dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
		float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

		// Update the camera radius based on input.
		mRadius += dx - dy;

		// Restrict the radius.
		mRadius = MathHelper::Clamp(mRadius, 3.0f, 500.0f);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

void D3DApp::OnKeyDown(WPARAM btnState)
{
	if (d3dUtil::IsKeyDown('Q'))
	{
		mRubikCube.Rotate(RubikCube::Clockwise);
	}
	else
	if (d3dUtil::IsKeyDown('E'))
	{
		mRubikCube.Rotate(RubikCube::Anticlockwise);
	}
	else
	if (d3dUtil::IsKeyDown('G'))
	{
		mRubikCube.CheckWinCondition();
	}
	else 
	if(d3dUtil::IsKeyDown('R'))
	{
		if (mSupportsRaytracing)
		{
			mRaster = !mRaster;
		}
		else
		{
			mRaster = true;
		}
	}
}

void D3DApp::Pick(int sx, int sy)
{
	mRubikCube.DeselectCubes();
	
	XMFLOAT4X4 P = mProj;

	float vx = (+2.0f * sx / mClientWidth - 1.0f) / P(0, 0);
	float vy = (-2.0f * sy / mClientHeight + 1.0f) / P(1, 1);



	XMMATRIX V = XMLoadFloat4x4(&mView);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(V), V);

	RenderObject* closestObject = nullptr;
	float closestDistance = INFINITE;
	float tmin = 0.0f;
	for (auto ri : mOpaqueObjects)
	{
		auto geo = ri->geometry;


		XMMATRIX W = XMLoadFloat4x4(&ri->world);
		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(W), W);

		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);
		XMVECTOR rayOrigin = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
		XMVECTOR rayDir = XMVectorSet(vx, vy, 1.0f, 0.0f);
		rayOrigin = XMVector3TransformCoord(rayOrigin, toLocal);
		rayDir = XMVector3TransformNormal(rayDir, toLocal);

		rayDir = XMVector3Normalize(rayDir);

		
		if (ri->bounds.Intersects(rayOrigin, rayDir, tmin))
		{
			if (tmin < closestDistance)
			{
				closestDistance = tmin;
				closestObject = ri;
			}
		}
		
	}

	if (closestObject)
	{
		std::cout << "Hit cube: " << closestObject->name << std::endl;
		auto cub = static_cast<Cube*>(closestObject);
		mRubikCube.GetAdjecantCubes(cub->idx, cub->idy, cub->idz);
	}
}