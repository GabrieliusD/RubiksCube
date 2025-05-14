#include <DirectXMath.h>
#include <Transform.h>

using namespace DirectX;

struct Camera
{
	XMFLOAT4X4 view;
	XMFLOAT4X4 proj;
	XMFLOAT4X4 world;

	void CreateProjection(float fovAngleY, float aspectRatio, float nearZ, float farZ)
	{
		XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ));
	}

	void CreateLookAtView(XMVECTOR pos, XMVECTOR target, XMVECTOR up)
	{
		XMMATRIX viewMatrix = XMMatrixLookAtLH(pos, target, up);
		XMStoreFloat4x4(&view, viewMatrix);
	}

	void CreateViewFromTransform(const Transform& transform)
	{
		XMFLOAT3 position = transform.position;
		XMMATRIX translationMatrix = XMMatrixTranslation(position.x, position.y, position.z);
		XMFLOAT3 rotation = transform.rotation;
		XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYawFromVector(XMVectorSet(rotation.x, rotation.y, rotation.z, 0));
		XMMATRIX viewMatrix = XMMatrixMultiply(translationMatrix, rotationMatrix);
	}
};