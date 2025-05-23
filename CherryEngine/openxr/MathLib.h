#pragma once
#include <DirectXMath.h>
#include <openxr.h>
class MathLib
{
public:
	static DirectX::XMFLOAT3 XrVectorToXm(XrVector3f Vector)
	{
		return DirectX::XMFLOAT3(Vector.x, Vector.y, Vector.z);
	}

	static DirectX::XMFLOAT4 XrVectorToXm(XrQuaternionf Quat)
	{
		return DirectX::XMFLOAT4(Quat.x, Quat.y, Quat.z, Quat.w);
	}
};

