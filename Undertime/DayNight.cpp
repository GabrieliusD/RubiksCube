#include "DayNight.h"
#include "ColorUtil.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace DirectX;

namespace
{
	constexpr float kPi = 3.14159265f;

	// Smoothstep from e0 to e1 (e1 < e0 runs the other way).
	float Smooth(float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	float Mix(float a, float b, float t) { return a + (b - a) * t; }

	// Towards the sun (or moon) at angle `a` along its path: 0 rising in the east (+x), pi/2
	// highest in the south (-z), pi setting in the west.
	XMFLOAT3 PathPoint(float a, float noonElevationDegrees)
	{
		const float e = XMConvertToRadians(noonElevationDegrees);
		return { std::cos(a), std::sin(a) * std::sin(e), -std::sin(a) * std::cos(e) };
	}
}

DayLight DayLightAt(float hour)
{
	DayLight d;
	const float a = (hour - 6.0f) / 12.0f * kPi;
	const XMFLOAT3 sun = PathPoint(a, cfg::kSunNoonElevation);
	const float h = sun.y;
	d.sunHeight = h;

	// Sky: twilight colours around the horizon crossing, day above, night below.
	const float dayT = Smooth(-0.05f, 0.25f, h);
	const float nightT = Smooth(-0.02f, -0.22f, h);
	auto sky = [&](uint32_t day, uint32_t twilight, uint32_t night)
	{
		return color::Lerp(color::Lerp(color::Linear(twilight), color::Linear(day), dayT), color::Linear(night), nightT);
	};
	d.zenith = sky(cfg::kSkyZenith, cfg::kTwilightZenith, cfg::kNightZenith);
	d.horizon = sky(cfg::kDayFog, cfg::kTwilightHorizon, cfg::kNightHorizon);
	d.ground = color::Lerp(color::Linear(cfg::kSkyGround), color::Linear(cfg::kNightGround), Smooth(0.1f, -0.15f, h));
	d.ambient = Mix(Mix(cfg::kTwilightAmbient, cfg::kDayAmbient, dayT), cfg::kNightAmbient, nightT);
	d.fill = Mix(cfg::kNightFill, 1.0f, Smooth(-0.15f, 0.15f, h));
	d.stars = cfg::kStarIntensity * nightT;
	d.night = Smooth(0.12f, -0.04f, h);

	if (h >= -0.03f)
	{
		// The sun: warm near the horizon, fading out as it sets.
		const XMFLOAT3 c = color::Lerp(color::Linear(cfg::kSunLowColor), color::Linear(cfg::kSunNoonColor), Smooth(0.0f, 0.45f, h));
		d.lightColor = color::Scale(c, cfg::kSunStrength * Smooth(-0.04f, 0.08f, h));
		d.lightDirection = { -sun.x, -sun.y, -sun.z };
		d.diskBrightness = cfg::kSunDiskIntensity;
	}
	else
	{
		// The moon, half a day behind the sun on a lower path.
		const XMFLOAT3 moon = PathPoint(a - kPi, cfg::kSunNoonElevation * 0.8f);
		d.lightColor = color::Scale(color::Linear(cfg::kMoonColor), cfg::kMoonStrength * Smooth(-0.02f, -0.12f, h));
		d.lightDirection = { -moon.x, -moon.y, -moon.z };
		d.diskBrightness = cfg::kMoonDiskBrightness / cfg::kMoonStrength;
	}
	return d;
}

namespace
{
	float WeatherHash(int64_t i)
	{
		uint32_t h = static_cast<uint32_t>(i) * 374761393u + static_cast<uint32_t>(i >> 32) * 668265263u + 911u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
	}

	// Smooth value noise over days: one value per kWeatherDays.
	float WeatherNoise(double days, int salt)
	{
		const double t = days / cfg::kWeatherDays + salt * 1000.0;
		const double cell = std::floor(t);
		const float f = static_cast<float>(t - cell);
		const float u = f * f * (3.0f - 2.0f * f);
		const int64_t i = static_cast<int64_t>(cell);
		return Mix(WeatherHash(i * 2 + salt), WeatherHash((i + 1) * 2 + salt), u);
	}

	float RainAt(double days)
	{
		const float overcast = Smooth(cfg::kOvercastStart, cfg::kOvercastFull, WeatherNoise(days, 0));
		return overcast * Smooth(cfg::kRainStart, cfg::kRainFull, WeatherNoise(days, 1));
	}
}

Weather WeatherAt(double days)
{
	Weather w;
	w.overcast = Smooth(cfg::kOvercastStart, cfg::kOvercastFull, WeatherNoise(days, 0));
	w.rain = RainAt(days);
	// Wet: the most recent rain, drying over kDryDays.
	float wet = 0.0f;
	for (int k = 0; k <= 8; ++k)
	{
		const double back = cfg::kDryDays * k / 8.0;
		wet = std::max(wet, std::min(1.0f, RainAt(days - back) * 2.0f) * (1.0f - static_cast<float>(back / cfg::kDryDays)));
	}
	w.wet = wet;
	return w;
}

Weather WeatherAverage()
{
	// Sampled once over a long stretch of days.
	static const Weather average = []
	{
		Weather sum;
		constexpr int kSamples = 400;
		for (int i = 0; i < kSamples; ++i)
		{
			const Weather w = WeatherAt(i * 0.37);
			sum.overcast += w.overcast / kSamples;
			sum.rain += w.rain / kSamples;
			sum.wet += w.wet / kSamples;
		}
		return sum;
	}();
	return average;
}

const DayLight& DayLightAverage()
{
	static const DayLight average = []
	{
		constexpr int kSamples = 96;
		DayLight sum;
		sum.fill = 0.0f;
		for (int i = 0; i < kSamples; ++i)
		{
			const DayLight d = DayLightAt(24.0f * (i + 0.5f) / kSamples);
			auto add = [](XMFLOAT3& a, const XMFLOAT3& b) { a.x += b.x; a.y += b.y; a.z += b.z; };
			add(sum.lightColor, d.lightColor);
			add(sum.zenith, d.zenith);
			add(sum.horizon, d.horizon);
			add(sum.ground, d.ground);
			sum.ambient += d.ambient;
			sum.fill += d.fill;
			sum.stars += d.stars;
			sum.night += d.night;
			sum.sunHeight += d.sunHeight;
		}
		const float n = 1.0f / kSamples;
		DayLight avg = sum;
		avg.lightColor = color::Scale(sum.lightColor, n);
		avg.zenith = color::Scale(sum.zenith, n);
		avg.horizon = color::Scale(sum.horizon, n);
		avg.ground = color::Scale(sum.ground, n);
		avg.ambient *= n;
		avg.fill *= n;
		avg.stars *= n;
		avg.night *= n;
		avg.sunHeight *= n;
		// A blur of days has no sun to point at: light from high in the south, no disk.
		avg.lightDirection = DayLightAt(12.0f).lightDirection;
		avg.diskBrightness = 0.0f;
		return avg;
	}();
	return average;
}

DayLight Blend(const DayLight& a, const DayLight& b, float t)
{
	DayLight d;
	const XMVECTOR dir = XMVectorLerp(XMLoadFloat3(&a.lightDirection), XMLoadFloat3(&b.lightDirection), t);
	XMStoreFloat3(&d.lightDirection, XMVector3Normalize(dir));
	d.lightColor = color::Lerp(a.lightColor, b.lightColor, t);
	d.diskBrightness = Mix(a.diskBrightness, b.diskBrightness, t);
	d.zenith = color::Lerp(a.zenith, b.zenith, t);
	d.horizon = color::Lerp(a.horizon, b.horizon, t);
	d.ground = color::Lerp(a.ground, b.ground, t);
	d.ambient = Mix(a.ambient, b.ambient, t);
	d.fill = Mix(a.fill, b.fill, t);
	d.stars = Mix(a.stars, b.stars, t);
	d.night = Mix(a.night, b.night, t);
	d.sunHeight = Mix(a.sunHeight, b.sunHeight, t);
	return d;
}

float StrobeBlend(float rate)
{
	if (rate <= 0.0f) return 0.0f;
	const float secondsPerDay = 86400.0f / rate;
	return Smooth(cfg::kStrobeSlowDay, cfg::kStrobeFastDay, secondsPerDay);
}
