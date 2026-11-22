#include "Clock.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace
{
	constexpr int kZoneCount = static_cast<int>(std::size(cfg::kZoneRate));
	constexpr int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const char* const kMonthNames[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

	float Smooth(float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
}

int OutsideClock::Zone(bool inside, float z, const std::vector<float>& zoneStarts)
{
	if (!inside || zoneStarts.empty() || z < zoneStarts[0]) return -1;
	int zone = 0;
	for (int i = 0; i < static_cast<int>(zoneStarts.size()); ++i)
		if (z >= zoneStarts[i]) zone = i;
	return std::min(zone, kZoneCount - 1);
}

float OutsideClock::Rate(bool inside, float z, const std::vector<float>& zoneStarts)
{
	if (!inside) return cfg::kSurfaceRate;
	const int zone = Zone(inside, z, zoneStarts);
	if (zone >= 0) return cfg::kZoneRate[zone];
	// The entrance passage: from the surface rate at the face to zone 1's where it begins.
	const float start = zoneStarts.empty() ? cfg::kEntranceDepth : zoneStarts[0];
	return cfg::kSurfaceRate + (cfg::kZoneRate[0] - cfg::kSurfaceRate) * Smooth(0.0f, start, z);
}

int OutsideClock::EraAt(double years)
{
	int era = 1;
	for (float start : cfg::kEraStartYears)
		if (years >= start) ++era;
	return era;
}

double OutsideClock::EraStartYears(int era)
{
	if (era <= 1) return 0.0;
	return cfg::kEraStartYears[std::min(era, 6) - 2];
}

void OutsideClock::Reset()
{
	mDays = cfg::kArrivalHour / 24.0;
	mPersonal = 0.0;
}

void OutsideClock::Advance(float realSeconds, float rate)
{
	mDays += static_cast<double>(realSeconds) * rate / 86400.0;
	mPersonal += realSeconds;
}

float OutsideClock::HourOfDay() const
{
	return static_cast<float>((mDays - std::floor(mDays)) * 24.0);
}

int OutsideClock::DayOfYear() const
{
	const long long day = static_cast<long long>(std::floor(mDays)) + cfg::kArrivalDayOfYear;
	return static_cast<int>(day % 365);
}

int OutsideClock::Year() const
{
	const long long day = static_cast<long long>(std::floor(mDays)) + cfg::kArrivalDayOfYear;
	return cfg::kStartYear + static_cast<int>(day / 365);
}

std::string OutsideClock::DateText() const
{
	int day = DayOfYear(), month = 0;
	while (day >= kMonthDays[month]) day -= kMonthDays[month++];
	const float hour = HourOfDay();
	const int h = static_cast<int>(hour), m = static_cast<int>((hour - h) * 60.0f);
	char text[64];
	std::snprintf(text, sizeof(text), "%02d:%02d, %d %s %d", h, m, day + 1, kMonthNames[month], Year());
	return text;
}

std::string OutsideClock::SpanText(double days)
{
	auto part = [](int n, const char* unit)
	{
		return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
	};
	// Rounded to the nearest of the smaller unit, carrying over (never "2 years, 12 months").
	if (days >= 365.0 - 15.2)
	{
		int years = static_cast<int>(days / 365.0);
		int months = static_cast<int>(std::lround((days - years * 365.0) / 30.4));
		if (months >= 12) { ++years; months = 0; }
		return months > 0 ? part(years, "year") + ", " + part(months, "month") : part(years, "year");
	}
	if (days >= 30.4 - 0.5)
	{
		int months = static_cast<int>(days / 30.4);
		int rest = static_cast<int>(std::lround(days - months * 30.4));
		if (rest >= 30) { ++months; rest = 0; }
		return rest > 0 ? part(months, "month") + ", " + part(rest, "day") : part(months, "month");
	}
	if (days >= 1.0 - 1.0 / 48.0)
	{
		int whole = static_cast<int>(days);
		int hours = static_cast<int>(std::lround((days - whole) * 24.0));
		if (hours >= 24) { ++whole; hours = 0; }
		return hours > 0 ? part(whole, "day") + ", " + part(hours, "hour") : part(whole, "day");
	}
	const double hours = days * 24.0;
	if (hours >= 1.0) return part(static_cast<int>(hours), "hour");
	return part(std::max(0, static_cast<int>(hours * 60.0)), "minute");
}
