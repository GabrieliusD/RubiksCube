#include "Editor/Gizmo.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;

namespace
{
	constexpr float kHandleScreenFraction = 0.12f;   // arrow length relative to the viewport height
	constexpr float kPickPixels = 7.0f;
	constexpr int kRingSegments = 64;
	const ImU32 kAxisColors[3] = { IM_COL32(230, 70, 70, 255), IM_COL32(110, 210, 80, 255), IM_COL32(70, 130, 240, 255) };
	const ImU32 kHotColor = IM_COL32(255, 210, 60, 255);

	struct Projector
	{
		XMMATRIX viewProj;
		const Gizmo::View& view;

		// False when the point is behind the camera.
		bool ToScreen(FXMVECTOR world, ImVec2& out) const
		{
			const XMVECTOR clip = XMVector4Transform(XMVectorSetW(world, 1.0f), viewProj);
			const float w = XMVectorGetW(clip);
			if (w <= 1e-4f) return false;
			const float x = XMVectorGetX(clip) / w, y = XMVectorGetY(clip) / w;
			out = ImVec2(view.originX + (x * 0.5f + 0.5f) * view.width, view.originY + (0.5f - y * 0.5f) * view.height);
			return true;
		}

		// World-space ray through a screen pixel.
		void Ray(const ImVec2& pixel, XMVECTOR& origin, XMVECTOR& direction) const
		{
			const float x = (pixel.x - view.originX) / view.width * 2.0f - 1.0f;
			const float y = 1.0f - (pixel.y - view.originY) / view.height * 2.0f;
			const XMMATRIX inverse = XMMatrixInverse(nullptr, viewProj);
			const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(x, y, 0.0f, 1.0f), inverse);
			const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(x, y, 1.0f, 1.0f), inverse);
			origin = XMLoadFloat3(&view.cameraPosition);
			direction = XMVector3Normalize(farPoint - nearPoint);
		}
	};

	float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
	{
		const float abx = b.x - a.x, aby = b.y - a.y;
		const float length2 = abx * abx + aby * aby;
		float t = length2 > 0.0f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / length2 : 0.0f;
		t = std::clamp(t, 0.0f, 1.0f);
		const float dx = a.x + abx * t - p.x, dy = a.y + aby * t - p.y;
		return std::sqrt(dx * dx + dy * dy);
	}

	// Closest point on the line (origin + s * axis) to a ray; returns s.
	float ClosestOnAxis(FXMVECTOR axisOrigin, FXMVECTOR axis, FXMVECTOR rayOrigin, GXMVECTOR rayDirection)
	{
		const XMVECTOR w0 = axisOrigin - rayOrigin;
		const float a = XMVectorGetX(XMVector3Dot(axis, axis));
		const float b = XMVectorGetX(XMVector3Dot(axis, rayDirection));
		const float c = XMVectorGetX(XMVector3Dot(rayDirection, rayDirection));
		const float d = XMVectorGetX(XMVector3Dot(axis, w0));
		const float e = XMVectorGetX(XMVector3Dot(rayDirection, w0));
		const float denominator = a * c - b * b;
		if (std::fabs(denominator) < 1e-6f) return 0.0f;
		return (b * e - c * d) / denominator;
	}

	bool RayPlane(FXMVECTOR rayOrigin, FXMVECTOR rayDirection, FXMVECTOR planePoint, GXMVECTOR normal, XMVECTOR& hit)
	{
		const float denominator = XMVectorGetX(XMVector3Dot(rayDirection, normal));
		if (std::fabs(denominator) < 1e-5f) return false;
		const float t = XMVectorGetX(XMVector3Dot(planePoint - rayOrigin, normal)) / denominator;
		if (t < 0.0f) return false;
		hit = rayOrigin + rayDirection * t;
		return true;
	}

	float SnapValue(float value, float step) { return step > 0.0f ? std::round(value / step) * step : value; }
}

bool Gizmo::Manipulate(const View& view, Mode mode, bool local, const Snap& snap, Pose& pose)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImGuiIO& io = ImGui::GetIO();
	const Projector projector{ XMLoadFloat4x4(&view.viewProj), view };

	const XMVECTOR origin = XMLoadFloat3(&pose.position);
	ImVec2 originScreen;
	if (!projector.ToScreen(origin, originScreen))
	{
		mHovered = Handle::None;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) mActive = Handle::None;
		return false;
	}

	// Handle length: a fixed share of the viewport height, whatever the distance.
	const float distance = XMVectorGetX(XMVector3Length(origin - XMLoadFloat3(&view.cameraPosition)));
	XMVECTOR probeOrigin, probeDirection;
	projector.Ray(ImVec2(originScreen.x, originScreen.y), probeOrigin, probeDirection);
	ImVec2 probe;
	float pixelsPerMeter = 1.0f;
	{
		// Measure: project a point 1 m to the side of the origin, perpendicular to the view.
		const XMVECTOR side = XMVector3Normalize(XMVector3Cross(probeDirection, XMVectorSet(0, 1, 0, 0)));
		const XMVECTOR sideSafe = XMVectorGetX(XMVector3LengthSq(side)) > 0.5f ? side : XMVectorSet(1, 0, 0, 0);
		if (projector.ToScreen(origin + sideSafe, probe))
		{
			pixelsPerMeter = std::max(std::hypot(probe.x - originScreen.x, probe.y - originScreen.y), 1e-3f);
		}
		else
		{
			pixelsPerMeter = view.height / std::max(distance, 0.01f);
		}
	}
	const float length = (view.height * kHandleScreenFraction) / pixelsPerMeter;

	// Axes (world, or the pose's own; scale always uses its own).
	XMVECTOR axes[3] = { XMVectorSet(1, 0, 0, 0), XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 0, 1, 0) };
	if (local || mode == Mode::Scale)
	{
		const XMVECTOR rotation = XMLoadFloat4(&pose.rotation);
		for (XMVECTOR& axis : axes) axis = XMVector3Rotate(axis, rotation);
	}
	if (mActive != Handle::None)
	{
		for (int i = 0; i < 3; ++i) axes[i] = XMLoadFloat3(&mAxes[i]);
	}

	const ImVec2 mouse = io.MousePos;
	XMVECTOR rayOrigin, rayDirection;
	projector.Ray(mouse, rayOrigin, rayDirection);

	// ---- Hover test (only when nothing is being dragged) ---------------------------------
	ImVec2 axisEnd[3];
	bool axisVisible[3];
	for (int i = 0; i < 3; ++i) axisVisible[i] = projector.ToScreen(origin + axes[i] * length, axisEnd[i]);

	Handle hovered = Handle::None;
	const bool mouseInView = mouse.x >= view.originX && mouse.y >= view.originY && mouse.x < view.originX + view.width && mouse.y < view.originY + view.height;
	if (mActive == Handle::None && mouseInView)
	{
		float best = kPickPixels;
		if (mode == Mode::Rotate)
		{
			for (int i = 0; i < 3; ++i)
			{
				const XMVECTOR u = axes[(i + 1) % 3], v = axes[(i + 2) % 3];
				ImVec2 previous;
				bool havePrevious = false;
				for (int s = 0; s <= kRingSegments; ++s)
				{
					const float angle = XM_2PI * s / kRingSegments;
					ImVec2 point;
					const bool visible = projector.ToScreen(origin + (u * std::cos(angle) + v * std::sin(angle)) * length, point);
					if (visible && havePrevious)
					{
						const float d = DistanceToSegment(mouse, previous, point);
						if (d < best) { best = d; hovered = static_cast<Handle>(static_cast<int>(Handle::X) + i); }
					}
					previous = point;
					havePrevious = visible;
				}
			}
		}
		else
		{
			for (int i = 0; i < 3; ++i)
			{
				if (!axisVisible[i]) continue;
				const float d = DistanceToSegment(mouse, originScreen, axisEnd[i]);
				if (d < best) { best = d; hovered = static_cast<Handle>(static_cast<int>(Handle::X) + i); }
			}
			if (mode == Mode::Translate)
			{
				// Plane squares between two axes.
				const Handle planes[3] = { Handle::YZ, Handle::XZ, Handle::XY };
				for (int i = 0; i < 3; ++i)
				{
					const XMVECTOR u = axes[(i + 1) % 3], v = axes[(i + 2) % 3];
					XMVECTOR hit;
					if (RayPlane(rayOrigin, rayDirection, origin, axes[i], hit))
					{
						const float a = XMVectorGetX(XMVector3Dot(hit - origin, u)) / length;
						const float b = XMVectorGetX(XMVector3Dot(hit - origin, v)) / length;
						if (a > 0.2f && a < 0.45f && b > 0.2f && b < 0.45f && hovered == Handle::None) hovered = planes[i];
					}
				}
			}
			if (std::hypot(mouse.x - originScreen.x, mouse.y - originScreen.y) < kPickPixels * 1.3f) hovered = Handle::Screen;
		}
	}
	mHovered = hovered;

	// ---- Start / end a drag ----------------------------------------------------------
	if (mActive == Handle::None && hovered != Handle::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		mActive = hovered;
		mStartPose = pose;
		mHandleLength = length;
		for (int i = 0; i < 3; ++i) XMStoreFloat3(&mAxes[i], axes[i]);
		XMVECTOR grab = origin;
		if (mActive == Handle::X || mActive == Handle::Y || mActive == Handle::Z)
		{
			const int i = static_cast<int>(mActive) - static_cast<int>(Handle::X);
			if (mode == Mode::Rotate)
			{
				XMVECTOR hit;
				if (RayPlane(rayOrigin, rayDirection, origin, axes[i], hit)) grab = hit;
				const XMVECTOR u = axes[(i + 1) % 3], v = axes[(i + 2) % 3];
				mStartAngle = std::atan2(XMVectorGetX(XMVector3Dot(grab - origin, v)), XMVectorGetX(XMVector3Dot(grab - origin, u)));
			}
			else
			{
				grab = origin + axes[i] * ClosestOnAxis(origin, axes[i], rayOrigin, rayDirection);
			}
		}
		else
		{
			// Planes: the plane through the origin; screen handle: the plane facing the camera.
			XMVECTOR normal = XMVector3Normalize(XMLoadFloat3(&view.cameraPosition) - origin);
			if (mActive == Handle::YZ) normal = axes[0];
			if (mActive == Handle::XZ) normal = axes[1];
			if (mActive == Handle::XY) normal = axes[2];
			XMVECTOR hit;
			if (RayPlane(rayOrigin, rayDirection, origin, normal, hit)) grab = hit;
		}
		XMStoreFloat3(&mStartPoint, grab);
	}
	if (mActive != Handle::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) mActive = Handle::None;

	// ---- Drag ----------------------------------------------------------------------------
	bool changed = false;
	if (mActive != Handle::None)
	{
		const XMVECTOR startOrigin = XMLoadFloat3(&mStartPose.position);
		const XMVECTOR startPoint = XMLoadFloat3(&mStartPoint);
		const int axisIndex = (mActive >= Handle::X && mActive <= Handle::Z) ? static_cast<int>(mActive) - static_cast<int>(Handle::X) : -1;

		if (mode == Mode::Translate)
		{
			XMVECTOR delta = XMVectorZero();
			if (axisIndex >= 0)
			{
				const float s = ClosestOnAxis(startOrigin, axes[axisIndex], rayOrigin, rayDirection);
				const float s0 = XMVectorGetX(XMVector3Dot(startPoint - startOrigin, axes[axisIndex]));
				float amount = s - s0;
				if (snap.enabled) amount = SnapValue(amount, snap.translate);
				delta = axes[axisIndex] * amount;
			}
			else
			{
				XMVECTOR normal = XMVector3Normalize(XMLoadFloat3(&view.cameraPosition) - startOrigin);
				if (mActive == Handle::YZ) normal = axes[0];
				if (mActive == Handle::XZ) normal = axes[1];
				if (mActive == Handle::XY) normal = axes[2];
				XMVECTOR hit;
				if (RayPlane(rayOrigin, rayDirection, startPoint, normal, hit))
				{
					delta = hit - startPoint;
					if (snap.enabled && mActive != Handle::Screen)
					{
						XMVECTOR snapped = XMVectorZero();
						for (int i = 0; i < 3; ++i)
						{
							snapped += axes[i] * SnapValue(XMVectorGetX(XMVector3Dot(delta, axes[i])), snap.translate);
						}
						delta = snapped;
					}
				}
			}
			XMStoreFloat3(&pose.position, startOrigin + delta);
			changed = true;
		}
		else if (mode == Mode::Rotate && axisIndex >= 0)
		{
			const XMVECTOR axis = axes[axisIndex];
			const XMVECTOR u = axes[(axisIndex + 1) % 3], v = axes[(axisIndex + 2) % 3];
			XMVECTOR hit;
			if (RayPlane(rayOrigin, rayDirection, startOrigin, axis, hit))
			{
				const float angle = std::atan2(XMVectorGetX(XMVector3Dot(hit - startOrigin, v)), XMVectorGetX(XMVector3Dot(hit - startOrigin, u)));
				float delta = angle - mStartAngle;
				if (snap.enabled) delta = XMConvertToRadians(SnapValue(XMConvertToDegrees(delta), snap.rotate));
				const XMVECTOR start = XMLoadFloat4(&mStartPose.rotation);
				XMStoreFloat4(&pose.rotation, XMQuaternionNormalize(XMQuaternionMultiply(start, XMQuaternionRotationAxis(axis, delta))));
				changed = true;

				// Show the swept angle.
				ImVec2 a, b;
				if (projector.ToScreen(startOrigin + XMVector3Normalize(startPoint - startOrigin) * mHandleLength, a)
					&& projector.ToScreen(hit, b))
				{
					draw->AddLine(originScreen, a, IM_COL32(255, 255, 255, 120), 1.5f);
					char text[32];
					std::snprintf(text, sizeof(text), "%.1f deg", XMConvertToDegrees(delta));
					draw->AddText(ImVec2(mouse.x + 14, mouse.y - 6), IM_COL32(255, 255, 255, 230), text);
				}
			}
		}
		else if (mode == Mode::Scale)
		{
			XMFLOAT3 scale = mStartPose.scale;
			if (axisIndex >= 0)
			{
				const float s = ClosestOnAxis(startOrigin, axes[axisIndex], rayOrigin, rayDirection);
				const float s0 = std::max(XMVectorGetX(XMVector3Dot(startPoint - startOrigin, axes[axisIndex])), 1e-3f);
				float factor = s / s0;
				if (snap.enabled) factor = 1.0f + SnapValue(factor - 1.0f, snap.scale);
				(&scale.x)[axisIndex] = (&mStartPose.scale.x)[axisIndex] * std::max(factor, 0.001f);
			}
			else
			{
				// Centre handle: drag right/up to grow, left/down to shrink.
				ImVec2 startScreen;
				projector.ToScreen(startPoint, startScreen);
				float factor = 1.0f + ((mouse.x - startScreen.x) - (mouse.y - startScreen.y)) / 150.0f;
				if (snap.enabled) factor = 1.0f + SnapValue(factor - 1.0f, snap.scale);
				factor = std::max(factor, 0.001f);
				scale = { mStartPose.scale.x * factor, mStartPose.scale.y * factor, mStartPose.scale.z * factor };
			}
			pose.scale = scale;
			changed = true;
		}
	}

	// ---- Draw ------------------------------------------------------------------------------
	const Handle hot = mActive != Handle::None ? mActive : mHovered;
	auto colorFor = [&](Handle handle, int axis) { return hot == handle ? kHotColor : kAxisColors[axis]; };
	const XMVECTOR drawOrigin = XMLoadFloat3(&pose.position);
	projector.ToScreen(drawOrigin, originScreen);

	if (mode == Mode::Rotate)
	{
		for (int i = 0; i < 3; ++i)
		{
			const XMVECTOR u = axes[(i + 1) % 3], v = axes[(i + 2) % 3];
			ImVec2 points[kRingSegments + 1];
			int count = 0;
			for (int s = 0; s <= kRingSegments; ++s)
			{
				const float angle = XM_2PI * s / kRingSegments;
				if (projector.ToScreen(drawOrigin + (u * std::cos(angle) + v * std::sin(angle)) * length, points[count])) ++count;
			}
			const Handle handle = static_cast<Handle>(static_cast<int>(Handle::X) + i);
			draw->AddPolyline(points, count, colorFor(handle, i), ImDrawFlags_None, hot == handle ? 3.5f : 2.0f);
		}
	}
	else
	{
		if (mode == Mode::Translate)
		{
			const Handle planes[3] = { Handle::YZ, Handle::XZ, Handle::XY };
			for (int i = 0; i < 3; ++i)
			{
				const XMVECTOR u = axes[(i + 1) % 3] * length, v = axes[(i + 2) % 3] * length;
				ImVec2 quad[4];
				if (projector.ToScreen(drawOrigin + u * 0.2f + v * 0.2f, quad[0]) && projector.ToScreen(drawOrigin + u * 0.45f + v * 0.2f, quad[1])
					&& projector.ToScreen(drawOrigin + u * 0.45f + v * 0.45f, quad[2]) && projector.ToScreen(drawOrigin + u * 0.2f + v * 0.45f, quad[3]))
				{
					const ImU32 color = hot == planes[i] ? kHotColor : (kAxisColors[i] & 0x00FFFFFF) | 0x70000000;
					draw->AddConvexPolyFilled(quad, 4, color);
				}
			}
		}
		for (int i = 0; i < 3; ++i)
		{
			ImVec2 end;
			if (!projector.ToScreen(drawOrigin + axes[i] * length, end)) continue;
			const Handle handle = static_cast<Handle>(static_cast<int>(Handle::X) + i);
			const ImU32 color = colorFor(handle, i);
			draw->AddLine(originScreen, end, color, hot == handle ? 4.0f : 2.5f);
			if (mode == Mode::Translate)
			{
				draw->AddCircleFilled(end, 5.5f, color);
			}
			else
			{
				draw->AddRectFilled(ImVec2(end.x - 5, end.y - 5), ImVec2(end.x + 5, end.y + 5), color);
			}
		}
		draw->AddCircleFilled(originScreen, 5.0f, hot == Handle::Screen ? kHotColor : IM_COL32(240, 240, 240, 220));
	}
	return changed;
}
