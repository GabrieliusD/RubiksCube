#include "Growth.h"
#include "Config.h"
#include <algorithm>
#include <cstring>

namespace grow
{
	float TreeGrowth(double days)
	{
		if (days < 0.0) return 1.0f;
		if (days < cfg::kStumpDays) return 0.0f;
		const double t = (days - cfg::kStumpDays) / (cfg::kRegrowDays - cfg::kStumpDays);
		// A sapling starts at a tenth of the size.
		return static_cast<float>(std::clamp(0.1 + 0.9 * t, 0.1, 1.0));
	}

	int CropIndex(const char* seed)
	{
		for (int i = 0; i < static_cast<int>(std::size(cfg::kCrops)); ++i)
			if (std::strcmp(seed, cfg::kCrops[i].seed) == 0) return i;
		return -1;
	}

	CropState CropAt(int crop, double days, double yearDay)
	{
		CropState s;
		if (crop < 0 || crop >= static_cast<int>(std::size(cfg::kCrops))) return s;
		const cfg::Crop& c = cfg::kCrops[crop];
		days = std::max(days, 0.0);
		const bool tree = std::strcmp(c.seed, "apple_sapling") == 0;
		if (tree && days >= c.ripe)
		{
			// Grown: apples in the autumn (from day 250 of the year, for `lasts` days).
			s.growth = 1.0f;
			const double from = 250.0;
			if (yearDay >= from && yearDay < from + c.lasts) { s.stage = CropStage::Ripe; s.ripeIn = from + c.lasts - yearDay; }
			else { s.stage = CropStage::Growing; s.ripeIn = yearDay < from ? from - yearDay : 365.0 - yearDay + from; }
			return s;
		}
		if (days < c.ripe)
		{
			s.stage = CropStage::Growing;
			s.growth = static_cast<float>(days / c.ripe);
			s.ripeIn = c.ripe - days;
		}
		else if (days < c.ripe + c.lasts)
		{
			s.stage = CropStage::Ripe;
			s.growth = 1.0f;
			s.ripeIn = c.ripe + c.lasts - days;
		}
		else
		{
			s.stage = CropStage::Spoiled;
			s.growth = 1.0f;
		}
		return s;
	}
}
