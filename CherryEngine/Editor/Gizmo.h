#pragma once
#include <DirectXMath.h>

// A transform gizmo drawn with ImGui over the viewport: move arrows and planes,
// rotation rings, scale handles. It edits a world-space pose; the caller turns the
// result into the entity's local Transform.
class Gizmo
{
public:
	enum class Mode { Translate, Rotate, Scale };

	struct View
	{
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT3 cameraPosition;
		float originX = 0, originY = 0;   // viewport top-left in ImGui screen pixels
		float width = 1, height = 1;
	};

	struct Pose
	{
		DirectX::XMFLOAT3 position{ 0, 0, 0 };
		DirectX::XMFLOAT4 rotation{ 0, 0, 0, 1 };
		DirectX::XMFLOAT3 scale{ 1, 1, 1 };
	};

	struct Snap
	{
		bool enabled = false;
		float translate = 0.5f;    // m
		float rotate = 15.0f;      // degrees
		float scale = 0.1f;
	};

	// Draws the gizmo for `pose` and handles the mouse. Returns true while the pose was
	// changed this frame. `local`: axes follow the pose's rotation (scale is always local).
	bool Manipulate(const View& view, Mode mode, bool local, const Snap& snap, Pose& pose);

	bool IsHovered() const { return mHovered != Handle::None; }
	bool IsDragging() const { return mActive != Handle::None; }

private:
	enum class Handle { None, X, Y, Z, XY, XZ, YZ, Screen };

	Handle mHovered = Handle::None;
	Handle mActive = Handle::None;
	Pose mStartPose;
	DirectX::XMFLOAT3 mStartPoint{ 0, 0, 0 };   // where the drag grabbed the handle (world)
	DirectX::XMFLOAT3 mAxes[3];                  // drag axes at the start of the drag
	float mStartAngle = 0.0f;
	float mHandleLength = 1.0f;
};
