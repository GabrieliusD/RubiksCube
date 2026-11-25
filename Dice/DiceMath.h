#pragma once
// Quaternion maths with three.js semantics (the reference implementation's), in the
// reference's right-handed coordinates: Y up, angles in radians, q = (x, y, z, w),
// a.Multiply(b) = a x b (b applied first). Computed in double like JavaScript, stored in
// float like its Float32Arrays. Only the renderer converts to the engine's left-handed
// space (DiceVisuals.h), so every formula here matches PROTOTYPE_EXPORT.md Part C.
#include <cmath>

namespace dice
{
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kEpsilon = 2.220446049250313e-16;   // Number.EPSILON

	struct Vec3
	{
		double x = 0, y = 0, z = 0;
	};

	struct Quat
	{
		double x = 0, y = 0, z = 0, w = 1;

		static Quat AxisAngle(const Vec3& axis, double angle)   // axis must be normalised
		{
			const double s = std::sin(angle / 2);
			return { axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2) };
		}

		// The shortest-arc rotation taking unit vector `from` to unit vector `to`.
		static Quat FromUnitVectors(const Vec3& from, const Vec3& to)
		{
			Quat q;
			double r = from.x * to.x + from.y * to.y + from.z * to.z + 1;
			if (r < kEpsilon)
			{
				r = 0;
				if (std::fabs(from.x) > std::fabs(from.z)) q = { -from.y, from.x, 0, r };
				else q = { 0, -from.z, from.y, r };
			}
			else
				q = { from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z, from.x * to.y - from.y * to.x, r };
			return q.Normalized();
		}

		Quat Multiply(const Quat& b) const
		{
			const Quat& a = *this;
			return { a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y,
				a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
				a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
		}

		Quat Normalized() const
		{
			double l = std::sqrt(x * x + y * y + z * z + w * w);
			if (l == 0) return { 0, 0, 0, 1 };
			l = 1 / l;
			return { x * l, y * l, z * l, w * l };
		}

		// three.js Quaternion.slerp.
		Quat Slerp(const Quat& qb, double t) const
		{
			if (t == 0) return *this;
			if (t == 1) return qb;
			double cosHalfTheta = w * qb.w + x * qb.x + y * qb.y + z * qb.z;
			Quat b = qb;
			if (cosHalfTheta < 0)
			{
				b = { -qb.x, -qb.y, -qb.z, -qb.w };
				cosHalfTheta = -cosHalfTheta;
			}
			if (cosHalfTheta >= 1.0) return *this;
			const double sqrSinHalfTheta = 1.0 - cosHalfTheta * cosHalfTheta;
			if (sqrSinHalfTheta <= kEpsilon)
			{
				const double s = 1 - t;
				return Quat{ s * x + t * b.x, s * y + t * b.y, s * z + t * b.z, s * w + t * b.w }.Normalized();
			}
			const double sinHalfTheta = std::sqrt(sqrSinHalfTheta);
			const double halfTheta = std::atan2(sinHalfTheta, cosHalfTheta);
			const double ratioA = std::sin((1 - t) * halfTheta) / sinHalfTheta, ratioB = std::sin(t * halfTheta) / sinHalfTheta;
			return { x * ratioA + b.x * ratioB, y * ratioA + b.y * ratioB, z * ratioA + b.z * ratioB, w * ratioA + b.w * ratioB };
		}

		Vec3 Rotate(const Vec3& v) const
		{
			// v' = v + 2w (q x v) + 2 q x (q x v)
			const double tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
			return { v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx) };
		}
	};

	inline Quat GetQ(const float* arr, int i) { const float* k = arr + i * 4; return { k[0], k[1], k[2], k[3] }; }
	inline void PutQ(float* arr, int i, const Quat& q)
	{
		float* k = arr + i * 4;
		k[0] = static_cast<float>(q.x); k[1] = static_cast<float>(q.y); k[2] = static_cast<float>(q.z); k[3] = static_cast<float>(q.w);
	}
}
