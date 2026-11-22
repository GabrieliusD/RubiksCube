// Tests for the outside clock (rates by depth, eras, dates) and the light through the day.
#include "../Clock.h"
#include "../DayNight.h"
#include "../Config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
	int gClockFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gClockFailures;
	}

	float Brightness(const DayLight& d)
	{
		return d.lightColor.x + d.lightColor.y + d.lightColor.z + 3.0f * d.ambient;
	}
}

int ClockTests()
{
	// ---- Rates by depth --------------------------------------------------------------------
	{
		const std::vector<float> zones = { 6.0f, 35.0f, 65.0f, 95.0f, 125.0f };
		Check(OutsideClock::Rate(false, 50.0f, zones) == cfg::kSurfaceRate, "clock: outside the mountain (even above the cave) time runs at the surface rate");
		Check(OutsideClock::Rate(true, 0.0f, zones) == cfg::kSurfaceRate, "clock: at the face it's still the surface rate");
		bool rising = true;
		float last = 0.0f;
		for (float z = 0.0f; z < 160.0f; z += 0.5f)
		{
			const float rate = OutsideClock::Rate(true, z, zones);
			rising &= rate >= last;
			last = rate;
		}
		Check(rising, "clock: time never runs slower deeper in");
		Check(OutsideClock::Rate(true, 3.0f, zones) > cfg::kSurfaceRate && OutsideClock::Rate(true, 3.0f, zones) < cfg::kZoneRate[0],
			"clock: the entrance passage rises from the surface rate to zone 1's");
		Check(cfg::kZoneRate[0] > cfg::kSurfaceRate, "clock: even zone 1 runs faster than the surface");
		Check(OutsideClock::Rate(true, 130.0f, zones) == cfg::kZoneRate[4] && OutsideClock::Zone(true, 40.0f, zones) == 1,
			"clock: each zone has its own rate");
	}

	// ---- Time passing, dates, eras -----------------------------------------------------------
	{
		OutsideClock clock;
		clock.Reset();
		Check(std::fabs(clock.HourOfDay() - cfg::kArrivalHour) < 1e-3f && clock.Era() == 1, "clock: you arrive in the morning, in the Camp era");
		clock.Advance(cfg::kDayMinutes * 60.0f, cfg::kSurfaceRate);
		Check(std::fabs(clock.Days() - (1.0 + cfg::kArrivalHour / 24.0)) < 1e-4 && std::fabs(clock.PersonalSeconds() - cfg::kDayMinutes * 60.0) < 1e-3,
			"clock: a surface day takes kDayMinutes of your time");
		clock.Advance(60.0f, cfg::kZoneRate[4]);
		Check(std::fabs(clock.Years() - 1.0) < 0.01, "clock: a minute in zone 5 is a year outside");
		Check(OutsideClock::EraAt(0.0) == 1 && OutsideClock::EraAt(2.5) == 2 && OutsideClock::EraAt(45.0) == 4 && OutsideClock::EraAt(150.0) == 6,
			"clock: eras follow the years since arrival");
		Check(OutsideClock::EraStartYears(OutsideClock::EraAt(12.0)) <= 12.0 && OutsideClock::EraStartYears(OutsideClock::EraAt(12.0) + 1) > 12.0,
			"clock: an era's start year is where EraAt changes");
		OutsideClock date;
		date.SetDays(cfg::kArrivalHour / 24.0);
		std::printf("        (arrival: %s)\n", date.DateText().c_str());
		Check(date.Year() == cfg::kStartYear && date.DateText().find("May") != std::string::npos, "clock: the watch shows the arrival date");
		Check(OutsideClock::SpanText(365.0 * 4 + 65) == "4 years, 2 months" && OutsideClock::SpanText(3.25) == "3 days, 6 hours"
			&& OutsideClock::SpanText(1.0 / 24.0 * 0.5) == "30 minutes" && OutsideClock::SpanText(365.0 * 3 - 0.1) == "3 years"
			&& OutsideClock::SpanText(1.999) == "2 days", "clock: spans of time read naturally (and round up cleanly)");
	}

	// ---- Day and night -------------------------------------------------------------------------
	{
		const DayLight noon = DayLightAt(12.0f), midnight = DayLightAt(0.0f), dusk = DayLightAt(18.3f), morning = DayLightAt(9.0f);
		Check(noon.sunHeight > 0.7f && midnight.sunHeight < -0.7f, "day: the sun is high at noon and below the horizon at midnight");
		Check(noon.lightDirection.z > 0.0f && noon.lightDirection.y < 0.0f, "day: at noon sunlight comes from the south (it lights the cave's face)");
		Check(morning.lightDirection.x < 0.0f, "day: the morning sun is in the east");
		Check(Brightness(noon) > 3.0f * Brightness(midnight) && midnight.night > 0.99f && noon.night < 0.01f, "day: nights are dark and lamps are lit");
		Check(midnight.lightDirection.y < 0.0f && midnight.lightColor.z > midnight.lightColor.x, "day: at night a blue moon lights the valley from above");
		Check(dusk.horizon.x > dusk.horizon.z && midnight.stars > 0.5f && noon.stars == 0.0f, "day: dusk is warm at the horizon; stars only at night");
		// Steps of 3 outside minutes (about 2 real seconds on the surface): sunrise takes
		// many of them, so no single step may change the light much.
		float worst = 0.0f, worstAmbient = 0.0f;
		DayLight last = DayLightAt(0.0f);
		for (float h = 0.05f; h <= 24.0f; h += 0.05f)
		{
			const DayLight d = DayLightAt(h);
			worst = std::max(worst, std::fabs(Brightness(d) - Brightness(last)));
			worstAmbient = std::max(worstAmbient, std::fabs(d.ambient - last.ambient));
			last = d;
		}
		std::printf("        (largest change in 3 minutes: light %.3f, ambient %.4f)\n", worst, worstAmbient);
		Check(worst < 0.12f * Brightness(noon) && worstAmbient < 0.02f, "day: the light changes smoothly through the day (no jumps at sunrise or sunset)");

		const float avg = Brightness(DayLightAverage());
		Check(avg > Brightness(midnight) && avg < Brightness(noon), "day: the average day is between noon and midnight");
		Check(StrobeBlend(cfg::kSurfaceRate) == 0.0f && StrobeBlend(cfg::kZoneRate[0]) == 0.0f && StrobeBlend(cfg::kZoneRate[4]) == 1.0f,
			"day: no blur at normal speeds, full blur when days flash by (no strobing)");
	}
	// ---- Weather ----------------------------------------------------------------------------
	{
		bool inRange = true, sameEachTime = true, rainNeedsCloud = true;
		float most = 0.0f, least = 1.0f, rainy = 0.0f, biggestStep = 0.0f;
		Weather last = WeatherAt(0.0);
		for (int i = 0; i < 4000; ++i)
		{
			const double days = i * 0.01;
			const Weather w = WeatherAt(days);
			inRange &= w.overcast >= 0.0f && w.overcast <= 1.0f && w.rain >= 0.0f && w.rain <= 1.0f && w.wet >= 0.0f && w.wet <= 1.0f;
			sameEachTime &= WeatherAt(days).overcast == w.overcast;
			rainNeedsCloud &= w.rain <= w.overcast + 1e-4f;
			most = std::max(most, w.overcast);
			least = std::min(least, w.overcast);
			rainy += w.rain > 0.3f ? 1.0f : 0.0f;
			biggestStep = std::max(biggestStep, std::fabs(w.overcast - last.overcast));
			last = w;
		}
		Check(inRange && sameEachTime, "weather: values in range, the same for the same day");
		Check(rainNeedsCloud, "weather: it only rains when it's overcast");
		Check(most > 0.9f && least < 0.1f, "weather: both clear and fully overcast days happen");
		Check(rainy > 100.0f && rainy < 2000.0f, "weather: it rains some of the time, not most of it");
		Check(biggestStep < 0.05f, "weather: it changes gradually (no jumps from one quarter hour to the next)");
	}
	return gClockFailures;
}
