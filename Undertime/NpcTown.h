#pragma once
// Where the cast is at any moment (Docs/NPCs.md §1 and §2). It's computed from the outside
// clock, never stepped frame by frame, so a dive of years, the ] key or turning time back just
// asks again for a new date. The same date always gives the same answer. Pure C++ (tested in
// UndertimeTests); NpcWorld draws the result.
#include "Npcs.h"
#include "Life.h"
#include "Valley.h"
#include <DirectXMath.h>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace npc
{
	// A point on a route: on the ground (its height follows the terrain) or at a fixed height
	// (a floor inside a building).
	struct RoutePoint
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		bool ground = true;
	};

	// A resolved PlaceDef, in world space.
	struct Place
	{
		RoutePoint at;
		float yaw = 0.0f;                    // radians, 0 = facing +z
		std::vector<RoutePoint> approach;    // from outside the doorway inwards (empty: walk straight there)
		bool resolved = false;               // false: its prop isn't in the scene, or no dry bank was found
	};

	// The valley's trails as a graph people walk along (cfg::kPathSegments, with ends that come
	// close joined up over dry land).
	class WaypointGraph
	{
	public:
		using Walkable = std::function<bool(float ax, float az, float bx, float bz)>;
		void Build(const float (*segments)[4], size_t count, float linkDistance, const Walkable& walkable);

		// From a to b: straight if they're close and nothing's in the way, else onto the
		// nearest trail, along the trails, and off at the point nearest b. Starts at a, ends at b.
		std::vector<DirectX::XMFLOAT2> Route(float ax, float az, float bx, float bz) const;

		// A footbridge: joined over the water (cfg::kBridges); routes cross it at deck height.
		void AddBridge(float ax, float az, float bx, float bz);
		bool IsBridge(const DirectX::XMFLOAT2& a, const DirectX::XMFLOAT2& b) const;

		const std::vector<DirectX::XMFLOAT2>& Nodes() const { return mNodes; }
		const std::vector<std::pair<int, int>>& Edges() const { return mEdges; }
		int Components() const;   // separate pieces (1: every trail reaches every other)

	private:
		struct Hit { int edge = -1; float t = 0.0f; DirectX::XMFLOAT2 point{}; float distance = 1e30f; };
		Hit Nearest(float x, float z) const;
		int AddNode(float x, float z, float mergeDistance);

		std::vector<DirectX::XMFLOAT2> mNodes;
		std::vector<std::pair<int, int>> mEdges;
		std::vector<std::vector<std::pair<int, float>>> mAdjacent;   // node -> (node, length)
		std::vector<std::pair<int, int>> mBridges;
		Walkable mWalkable;
	};

	// What someone is doing at a moment.
	struct Pose
	{
		bool present = false;              // lives in the valley in this era (and their places exist)
		DirectX::XMFLOAT3 position{};      // feet; y on the blocky ground or the floor (NpcWorld smooths steps)
		float yaw = 0.0f;                  // radians, 0 = facing +z
		Activity activity = Activity::Stand;
		float walked = 0.0f;               // metres along the route so far (the walk cycle's phase)
		int place = -1;                    // index into Places(): where they are, or are going
		int from = -1;                     // ... where they're coming from (walking), else -1
		bool rain = false;                 // at their rain place
		double blockStart = 0.0;           // outside days: when the current block began
		double age = 30.0;                 // years (children are drawn smaller)
		bool gathering = false;            // at (or going to) a wedding, a funeral, Founders' Day
	};

	class Town
	{
	public:
		// A prop's world matrix by its EntityInfo name; false if there's no such prop.
		using PropLookup = std::function<bool(const char* entity, DirectX::XMFLOAT4X4& world)>;

		// Resolves every place and builds the trail graph. Ground heights come from `valley`
		// (with its building pads), which must outlive the Town.
		void Build(const Valley& valley, const PropLookup& lookup);
		bool Built() const { return mValley != nullptr; }

		// Where cast member `npc` (an index into Cast()) is and what they're doing at outside
		// time `days`, while the valley shows era `era`. `log` (may be null): the story flags
		// that change lives.
		Pose Evaluate(size_t npc, double days, int era, const PlayerLog* log = nullptr) const;

		size_t PlaceCount() const { return mPlaces.size(); }
		const Place& PlaceAt(size_t index) const { return mPlaces[index]; }
		int PlaceIndex(const char* id) const;   // -1 if unknown (also "gather.<fire|square|graves>#<slot>")
		const std::string& PlaceName(int index) const;
		const WaypointGraph& Graph() const { return mGraph; }

		// The top of the blocks the terrain (and the props standing on it) is built at.
		float GroundY(float x, float z) const;
		// Whether the straight line a-b stays on dry land.
		bool Dry(float ax, float az, float bx, float bz) const;

		struct Route
		{
			std::vector<RoutePoint> points;
			std::vector<float> along;   // metres from the start to each point (horizontal)
			float length = 0.0f;
		};
		// The way from one place to another (built on first use, then kept).
		const Route& RouteBetween(int from, int to) const;
		// The way from a point on the ground (someone called away mid-walk) to a place.
		Route RouteFrom(const RoutePoint& start, int to) const;
		// A point `distance` metres along a route, and the direction it's heading.
		RoutePoint PointAlong(const Route& route, float distance, float* yaw = nullptr) const;

		// One block of someone's day as it happens on a particular day.
		struct Block
		{
			int index = 0;              // into their day (-1: a gathering)
			double start = 0.0;         // outside days (jittered a little per day)
			int place = -1;
			Activity activity = Activity::Stand;
			bool rain = false;
			bool gathering = false;
		};
		// Someone's blocks on day `day` in `era`: their day, with gatherings they go to spliced in.
		std::vector<Block> DayBlocks(size_t npc, long long day, int era, const PlayerLog* log) const;
		// The block running at `days`, the one before it and (optionally) the one before that
		// (false: no day in that era).
		bool BlocksAt(size_t npc, double days, int era, const PlayerLog* log, Block& current, Block& previous, Block* before = nullptr) const;
		// Outside days a walk of `metres` takes (at the walking speed on the surface).
		static double WalkDays(float metres);
		// How many hours block `index` of a day may start early or late (it varies by day, never
		// past a third of the way to the blocks either side, so their order never changes).
		static float JitterLimit(const std::vector<ScheduleBlock>& day, int index);

	private:
		const Valley* mValley = nullptr;
		std::vector<Place> mPlaces;            // in Places() order, then the gathering places
		std::vector<std::string> mPlaceNames;
		mutable std::map<const void*, int> mPlaceCache;   // schedule strings -> place
		WaypointGraph mGraph;
		mutable std::map<std::pair<int, int>, Route> mRoutes;
	};
}
