#pragma once
// The valley's light through the day: sun or moon, sky colours, ambient, stars and how dark
// it is (lamps and lit windows). Pure C++ (tested in UndertimeTests); the module copies it
// into the renderer's settings.
#include <DirectXMath.h>

struct DayLight
{
	DirectX::XMFLOAT3 lightDirection{ 0, -1, 0 };   // the way sunlight (or moonlight) travels
	DirectX::XMFLOAT3 lightColor{ 0, 0, 0 };        // linear, strength included
	float diskBrightness = 0.0f;                    // SkySettings::sunIntensity for that light
	DirectX::XMFLOAT3 zenith{ 0, 0, 0 };            // sky, linear
	DirectX::XMFLOAT3 horizon{ 0, 0, 0 };           // also the fog colour
	DirectX::XMFLOAT3 ground{ 0, 0, 0 };
	float ambient = 0.0f;
	float fill = 1.0f;                              // RenderSettings::fillLightScale
	float stars = 0.0f;
	float night = 0.0f;                             // 0 by day, 1 at night: lamps and lit windows
	float sunHeight = 0.0f;                         // sine of the sun's elevation (< 0: below the horizon)
};

// The light at `hour` (0..24) of an outside day.
DayLight DayLightAt(float hour);
// The light averaged over a whole day: what very fast days blur into.
const DayLight& DayLightAverage();
// a + (b - a) * t, field by field (the direction is renormalised).
DayLight Blend(const DayLight& a, const DayLight& b, float t);
// The weather on outside day `days` (since you arrived): the same every run for the same
// day, changing over a day or two. overcast 0 clear .. 1 grey; rain 0 dry .. 1 pouring
// (only when overcast enough); wet: how wet the ground is (it dries out slowly after rain).
struct Weather
{
	float overcast = 0.0f;
	float rain = 0.0f;
	float wet = 0.0f;
};
Weather WeatherAt(double days);
// The weather averaged over many days (what racing days blur into).
Weather WeatherAverage();

// How far to blend towards the average when outside time runs at `rate` outside seconds per
// real second: 0 while a day takes longer than kStrobeSlowDay, 1 from kStrobeFastDay.
float StrobeBlend(float rate);
