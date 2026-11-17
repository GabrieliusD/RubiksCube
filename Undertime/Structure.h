#pragma once
#include "VoxelWorld.h"
#include "Fragments.h"
#include <vector>

// Structural support: after the world changes, finds solid regions next to the change that
// are no longer connected to bedrock (the grid's boundary is all bedrock). Such a region is
// removed from the world and returned as a fragment to drop as a rigid body.
//
// Search is bounded: a region that grows past `budget` voxels is treated as anchored (part
// of the mountain), which keeps every check cheap. As in Teardown, there is no stress model:
// a thin bridge still holds.
struct DetachResult
{
	std::vector<FragmentVoxels> fragments;
	std::vector<VoxelWorld::Box> removed;   // world boxes emptied by the detached regions
	int visited = 0;                        // voxels examined (for the stats display)
};

DetachResult DetachUnsupported(VoxelWorld& world, const VoxelWorld::Box& changed, int budget);
