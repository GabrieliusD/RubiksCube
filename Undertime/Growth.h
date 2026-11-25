#pragma once
// Things that grow with outside time (Docs/Hauling.md H6, H7): felled trees coming back and
// crops ripening. Worked out from the day they were felled or planted and the clock alone, so
// dives (and turning time back) need no simulation. Pure C++ (tested in HaulingTests.cpp).

namespace grow
{
	// A felled tree, `days` after it was felled: 0 a stump, then a sapling growing to 1 (full
	// grown) by cfg::kRegrowDays. Negative `days` (time turned back past the felling): 1.
	float TreeGrowth(double days);

	enum class CropStage { Growing, Ripe, Spoiled };
	struct CropState
	{
		CropStage stage = CropStage::Growing;
		float growth = 0.0f;       // 0..1 towards ripe (for drawing)
		double ripeIn = 0.0;       // days until ripe (Growing), or until spoiled (Ripe)
	};
	// Crop `crop` (cfg::kCrops index) planted `days` ago. Apple trees ripen every autumn once
	// grown (yearDay: 0..365 of the outside year now).
	CropState CropAt(int crop, double days, double yearDay);
	int CropIndex(const char* seed);   // -1 if it isn't a seed
}
