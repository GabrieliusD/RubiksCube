#pragma once
// The outside clock: how much time has passed in the valley since you arrived. It runs at
// a normal pace on the surface and faster the deeper you are in the mountain (Design.md,
// "The core rule"). Pure C++, so it's tested in UndertimeTests.
#include <string>
#include <vector>

class OutsideClock
{
public:
	// Outside seconds per real second at `z` metres into the mountain. `inside`: in the cave
	// grid (the mountain top is outside). `zoneStarts`: where each cave zone begins (m), in
	// order; the entrance passage before the first rises from the surface rate to zone 1's,
	// and a seal band counts as the zone before it.
	static float Rate(bool inside, float z, const std::vector<float>& zoneStarts);
	// The zone (0-based) at `z`, -1 in the entrance passage or outside.
	static int Zone(bool inside, float z, const std::vector<float>& zoneStarts);
	// The era (1 Camp .. 6 Gone) after `years` since arrival.
	static int EraAt(double years);
	// Years since arrival when `era` begins (0 for Camp).
	static double EraStartYears(int era);

	void Reset();                                  // the day you arrive, kArrivalHour
	void Advance(float realSeconds, float rate);
	void SkipHours(double hours) { mDays += hours / 24.0; }
	void SetDays(double days) { mDays = days; }

	double Days() const { return mDays; }          // outside days since you arrived (from midnight of day 0)
	double Years() const { return mDays / 365.0; }
	float HourOfDay() const;                       // 0..24
	int DayOfYear() const;                         // 0..364
	int Year() const;                              // the calendar year on the watch
	double PersonalSeconds() const { return mPersonal; }   // your own time
	int Era() const { return EraAt(Years()); }

	// "14:05, 3 June 1871".
	std::string DateText() const;
	// A span of outside time in words: "4 years, 2 months", "3 days, 5 hours", "40 minutes".
	static std::string SpanText(double days);

private:
	double mDays = 0.0;
	double mPersonal = 0.0;
};
