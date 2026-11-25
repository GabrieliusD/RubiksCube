// DiceBench: the simulation's cost per frame at 10k / 50k / 100k dice (M10), without the
// engine: DiceSim::Frame (hover + storm + rolls + mid-air + cooldowns) and the instance matrices
// (DieMatrix for every die, as the module's WriteInstances). Not a test; prints a table.
#include "../DiceSim.h"
#include "../DiceVisuals.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dice;

int main()
{
	struct Case { const char* name; int board; CollisionMode mode; double storm; };
	const Case cases[] = {
		{ "10k idle", 100, CollisionMode::Nudge, 0.0 },
		{ "10k storm 10%/s nudge", 100, CollisionMode::Nudge, 0.10 },
		{ "10k storm 10%/s bump", 100, CollisionMode::Bump, 0.10 },
		{ "10k storm 10%/s stack", 100, CollisionMode::Stack, 0.10 },
		{ "50k storm 10%/s bump", 224, CollisionMode::Bump, 0.10 },
		{ "100k idle", 317, CollisionMode::Nudge, 0.0 },
		{ "100k storm 10%/s bump", 317, CollisionMode::Bump, 0.10 },
		{ "100k storm 30%/s bump", 317, CollisionMode::Bump, 0.30 },
	};
	std::printf("%-24s %7s %8s %9s %9s %9s %11s\n", "case", "dice", "rolling", "sim avg", "sim p99", "sim max", "matrices");
	for (const Case& c : cases)
	{
		DiceSim sim;
		sim.rand.Seed(1);
		sim.config.boardSize = c.board;
		sim.config.density = 1.0f;
		sim.config.collisionMode = c.mode;
		sim.config.hoverRadius = 6.0f;
		sim.BuildBoard();
		sim.stormPerSecond = c.storm;
		std::vector<double> simMs;
		double matrixMs = 0;
		long long rollingSum = 0;
		std::vector<DirectX::XMFLOAT4X4> matrices(sim.diceCount);
		const int frames = 600;
		for (int f = 0; f < frames; ++f)
		{
			const double t = f / 60.0, h = sim.N / 2.0 * 0.8;
			const auto a = std::chrono::steady_clock::now();
			sim.Frame(1.0 / 60, true, h * std::sin(t * 0.9), h * std::sin(t * 1.3));
			const auto b = std::chrono::steady_clock::now();
			for (int i = 0; i < sim.diceCount; ++i)
				DieMatrix(&sim.D.q[i * 4], 1.0f, 1.0f, sim.D.px[i], sim.D.py[i] + sim.Half(), sim.D.pz[i], matrices[i]);
			const auto e = std::chrono::steady_clock::now();
			sim.popupsOut.clear(); sim.sparksOut.clear(); sim.soundsOut.clear();
			if (f >= 120)   // after the storm has built up
			{
				simMs.push_back(std::chrono::duration<double, std::milli>(b - a).count());
				matrixMs += std::chrono::duration<double, std::milli>(e - b).count();
				rollingSum += sim.activeCount;
			}
		}
		std::sort(simMs.begin(), simMs.end());
		double avg = 0;
		for (double v : simMs) avg += v;
		avg /= simMs.size();
		std::printf("%-24s %7d %8lld %8.3fms %8.3fms %8.3fms %9.3fms\n", c.name, sim.diceCount, rollingSum / static_cast<long long>(simMs.size()), avg,
			simMs[simMs.size() * 99 / 100], simMs.back(), matrixMs / simMs.size());
	}
	return 0;
}
