#include "NpcTown.h"
#include "Config.h"
#include "DayNight.h"
#include <algorithm>
#include <cmath>
#include <queue>

using namespace DirectX;

namespace npc
{
	namespace
	{
		uint32_t Hash(uint32_t a, uint32_t b, uint32_t c)
		{
			uint32_t h = a * 374761393u + b * 668265263u + c * 2246822519u + 0x9e3779b9u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return h ^ (h >> 16);
		}
		float Hash01(uint32_t a, uint32_t b, uint32_t c) { return (Hash(a, b, c) & 0xFFFFFF) / 16777216.0f; }

		float Distance(const XMFLOAT2& a, const XMFLOAT2& b) { return std::hypot(b.x - a.x, b.y - a.y); }

		// The closest point to p on the segment a-b, as a fraction along it.
		float Closest(const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& p)
		{
			const float dx = b.x - a.x, dz = b.y - a.y;
			const float len2 = dx * dx + dz * dz;
			if (len2 < 1e-8f) return 0.0f;
			return std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dz) / len2, 0.0f, 1.0f);
		}
		XMFLOAT2 Lerp(const XMFLOAT2& a, const XMFLOAT2& b, float t) { return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
	}

	// ---- The trail graph -------------------------------------------------------------------------

	int WaypointGraph::AddNode(float x, float z, float mergeDistance)
	{
		for (size_t i = 0; i < mNodes.size(); ++i)
			if (std::hypot(mNodes[i].x - x, mNodes[i].y - z) < mergeDistance) return static_cast<int>(i);
		mNodes.push_back({ x, z });
		return static_cast<int>(mNodes.size() - 1);
	}

	void WaypointGraph::Build(const float (*segments)[4], size_t count, float linkDistance, const Walkable& walkable)
	{
		mNodes.clear();
		mEdges.clear();
		mWalkable = walkable;
		struct Segment { int a, b; std::vector<std::pair<float, int>> splits; };
		std::vector<Segment> trails;
		for (size_t i = 0; i < count; ++i)
		{
			const int a = AddNode(segments[i][0], segments[i][1], 1.0f);
			const int b = AddNode(segments[i][2], segments[i][3], 1.0f);
			if (a != b) trails.push_back({ a, b, {} });
		}
		const size_t trailNodes = mNodes.size();

		// Trail ends that stop short of another trail: joined to its nearest point (a new node
		// splitting it), or to another trail's end, if it's close and the way is dry.
		std::vector<std::pair<int, int>> links;
		for (size_t n = 0; n < trailNodes; ++n)
		{
			const XMFLOAT2 p = mNodes[n];
			for (size_t s = 0; s < trails.size(); ++s)
			{
				Segment& seg = trails[s];
				if (seg.a == static_cast<int>(n) || seg.b == static_cast<int>(n)) continue;
				const float t = Closest(mNodes[seg.a], mNodes[seg.b], p);
				const XMFLOAT2 c = Lerp(mNodes[seg.a], mNodes[seg.b], t);
				const float d = Distance(p, c);
				if (d > linkDistance || !walkable(p.x, p.y, c.x, c.y)) continue;
				if (t < 0.02f || t > 0.98f)
				{
					const int end = t < 0.5f ? seg.a : seg.b;
					if (end != static_cast<int>(n)) links.push_back({ static_cast<int>(n), end });
				}
				else
				{
					mNodes.push_back(c);
					const int split = static_cast<int>(mNodes.size() - 1);
					seg.splits.push_back({ t, split });
					links.push_back({ static_cast<int>(n), split });
				}
			}
		}
		for (Segment& seg : trails)
		{
			std::sort(seg.splits.begin(), seg.splits.end());
			int previous = seg.a;
			for (const auto& [t, node] : seg.splits)
			{
				mEdges.push_back({ previous, node });
				previous = node;
			}
			mEdges.push_back({ previous, seg.b });
		}
		for (const auto& link : links)
		{
			const bool known = std::any_of(mEdges.begin(), mEdges.end(), [&](const std::pair<int, int>& e)
				{ return (e.first == link.first && e.second == link.second) || (e.first == link.second && e.second == link.first); });
			if (!known) mEdges.push_back(link);
		}
		mAdjacent.assign(mNodes.size(), {});
		for (const auto& [a, b] : mEdges)
		{
			const float length = Distance(mNodes[a], mNodes[b]);
			mAdjacent[a].push_back({ b, length });
			mAdjacent[b].push_back({ a, length });
		}
	}

	void WaypointGraph::AddBridge(float ax, float az, float bx, float bz)
	{
		const int a = AddNode(ax, az, 1.0f), b = AddNode(bx, bz, 1.0f);
		mAdjacent.resize(mNodes.size());
		mEdges.push_back({ a, b });
		mBridges.push_back({ a, b });
		const float length = Distance(mNodes[a], mNodes[b]);
		mAdjacent[a].push_back({ b, length });
		mAdjacent[b].push_back({ a, length });
	}

	bool WaypointGraph::IsBridge(const XMFLOAT2& a, const XMFLOAT2& b) const
	{
		for (const auto& [i, j] : mBridges)
			if ((Distance(a, mNodes[i]) < 0.1f && Distance(b, mNodes[j]) < 0.1f) || (Distance(a, mNodes[j]) < 0.1f && Distance(b, mNodes[i]) < 0.1f)) return true;
		return false;
	}

	int WaypointGraph::Components() const
	{
		std::vector<int> label(mNodes.size(), -1);
		int pieces = 0;
		for (size_t start = 0; start < mNodes.size(); ++start)
		{
			if (label[start] >= 0) continue;
			std::vector<int> stack{ static_cast<int>(start) };
			label[start] = pieces;
			while (!stack.empty())
			{
				const int n = stack.back();
				stack.pop_back();
				for (const auto& [m, length] : mAdjacent[n])
					if (label[m] < 0) { label[m] = pieces; stack.push_back(m); }
			}
			++pieces;
		}
		return pieces;
	}

	WaypointGraph::Hit WaypointGraph::Nearest(float x, float z) const
	{
		Hit best;
		const XMFLOAT2 p{ x, z };
		for (size_t e = 0; e < mEdges.size(); ++e)
		{
			const XMFLOAT2& a = mNodes[mEdges[e].first];
			const XMFLOAT2& b = mNodes[mEdges[e].second];
			const float t = Closest(a, b, p);
			const XMFLOAT2 c = Lerp(a, b, t);
			const float d = Distance(p, c);
			if (d >= best.distance || (mWalkable && !mWalkable(x, z, c.x, c.y))) continue;
			best = { static_cast<int>(e), t, c, d };
		}
		return best;
	}

	std::vector<XMFLOAT2> WaypointGraph::Route(float ax, float az, float bx, float bz) const
	{
		const XMFLOAT2 a{ ax, az }, b{ bx, bz };
		const bool straightOk = !mWalkable || mWalkable(ax, az, bx, bz);
		if ((Distance(a, b) <= cfg::kNpcDirectDistance && straightOk) || mEdges.empty()) return { a, b };
		const Hit ha = Nearest(ax, az), hb = Nearest(bx, bz);
		if (ha.edge < 0 || hb.edge < 0) return { a, b };

		std::vector<XMFLOAT2> out{ a, ha.point };
		if (ha.edge == hb.edge)
		{
			out.push_back(hb.point);
		}
		else
		{
			// Dijkstra from the entry point (joined to its edge's two ends) to the exit point.
			const int count = static_cast<int>(mNodes.size());
			const int start = count, goal = count + 1;
			std::vector<float> cost(count + 2, 1e30f);
			std::vector<int> from(count + 2, -1);
			using Item = std::pair<float, int>;
			std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
			cost[start] = 0.0f;
			open.push({ 0.0f, start });
			const int ea = mEdges[ha.edge].first, eb = mEdges[ha.edge].second;
			const int ga = mEdges[hb.edge].first, gb = mEdges[hb.edge].second;
			auto neighbours = [&](int n, auto&& visit)
			{
				if (n == start)
				{
					visit(ea, Distance(ha.point, mNodes[ea]));
					visit(eb, Distance(ha.point, mNodes[eb]));
					return;
				}
				if (n == goal) return;
				for (const auto& [m, length] : mAdjacent[n]) visit(m, length);
				if (n == ga) visit(goal, Distance(mNodes[ga], hb.point));
				if (n == gb) visit(goal, Distance(mNodes[gb], hb.point));
			};
			while (!open.empty())
			{
				const float c = open.top().first;
				const int n = open.top().second;
				open.pop();
				if (c > cost[n]) continue;
				if (n == goal) break;
				neighbours(n, [&](int m, float length)
				{
					if (cost[n] + length < cost[m])
					{
						cost[m] = cost[n] + length;
						from[m] = n;
						open.push({ cost[m], m });
					}
				});
			}
			if (from[goal] < 0) return { a, b };   // trails that don't meet: walk straight
			std::vector<int> nodes;
			for (int n = from[goal]; n != start && n >= 0; n = from[n]) nodes.push_back(n);
			std::reverse(nodes.begin(), nodes.end());
			for (int n : nodes) out.push_back(mNodes[n]);
			out.push_back(hb.point);
		}
		out.push_back(b);
		// Drop points on top of each other (joining a trail at its end, and so on).
		std::vector<XMFLOAT2> clean;
		for (const XMFLOAT2& p : out)
			if (clean.empty() || Distance(clean.back(), p) > 0.05f) clean.push_back(p);
		if (clean.size() == 1) clean.push_back(b);
		return clean;
	}

	// ---- The town --------------------------------------------------------------------------------

	float Town::GroundY(float x, float z) const
	{
		return std::round(mValley->Height(x, z) / cfg::kVoxelSize) * cfg::kVoxelSize;
	}

	bool Town::Dry(float ax, float az, float bx, float bz) const
	{
		const float length = std::hypot(bx - ax, bz - az);
		const int steps = std::max(1, static_cast<int>(length / 0.5f));
		for (int i = 0; i <= steps; ++i)
		{
			const float t = static_cast<float>(i) / steps;
			float height = 0.0f, water = Valley::kNoWater;
			mValley->Sample(ax + (bx - ax) * t, az + (bz - az) * t, height, water);
			if (water != Valley::kNoWater && water > height + 0.02f) return false;
		}
		return true;
	}

	int Town::PlaceIndex(const char* id) const
	{
		for (size_t i = 0; i < mPlaceNames.size(); ++i)
			if (mPlaceNames[i] == id) return static_cast<int>(i);
		if (mPlaceNames.empty())   // not built: the written places only
		{
			const std::vector<PlaceDef>& defs = Places();
			for (size_t i = 0; i < defs.size(); ++i)
				if (std::string(id) == defs[i].id) return static_cast<int>(i);
		}
		return -1;
	}

	const std::string& Town::PlaceName(int index) const
	{
		static const std::string kNone = "?";
		return index >= 0 && index < static_cast<int>(mPlaceNames.size()) ? mPlaceNames[index] : kNone;
	}

	void Town::Build(const Valley& valley, const PropLookup& lookup)
	{
		mValley = &valley;
		mRoutes.clear();
		const std::vector<PlaceDef>& defs = Places();
		mPlaces.assign(defs.size(), Place{});
		for (size_t i = 0; i < defs.size(); ++i)
		{
			const PlaceDef& def = defs[i];
			Place& place = mPlaces[i];
			if (def.entity)
			{
				XMFLOAT4X4 stored;
				if (!lookup || !lookup(def.entity, stored)) continue;
				const XMMATRIX world = XMLoadFloat4x4(&stored);
				auto toWorld = [&](const std::array<float, 3>& p, bool ground)
				{
					XMFLOAT3 w;
					XMStoreFloat3(&w, XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1.0f), world));
					return RoutePoint{ w.x, w.y, w.z, ground };
				};
				place.at = toWorld(def.at, !def.inside);
				const float local = XMConvertToRadians(def.yawDegrees);
				XMFLOAT3 forward;
				XMStoreFloat3(&forward, XMVector3TransformNormal(XMVectorSet(std::sin(local), 0.0f, std::cos(local), 0.0f), world));
				place.yaw = std::atan2(forward.x, forward.z);
				for (size_t k = 0; k < def.approach.size(); ++k) place.approach.push_back(toWorld(def.approach[k], k == 0));
				place.resolved = true;
			}
			else if (def.riverBank)
			{
				// Out from the river's centre line towards the hint, to the first dry spot.
				float along = 0.0f;
				valley.RiverDistance(def.at[0], def.at[2], &along);
				float cx = 0.0f, cz = 0.0f;
				valley.RiverPointAt(along, cx, cz);
				float dx = def.at[0] - cx, dz = def.at[2] - cz;
				const float length = std::hypot(dx, dz);
				if (length < 1e-3f) continue;
				dx /= length;
				dz /= length;
				for (float t = cfg::kRiverHalfWidth * 0.5f; t < cfg::kRiverHalfWidth + 8.0f; t += 0.25f)
				{
					float height = 0.0f, water = Valley::kNoWater;
					valley.Sample(cx + dx * t, cz + dz * t, height, water);
					if (water != Valley::kNoWater && water > height + 0.02f) continue;
					const float x = cx + dx * (t + 0.35f), z = cz + dz * (t + 0.35f);
					place.at = { x, 0.0f, z, true };
					place.yaw = std::atan2(-dx, -dz);   // facing the water
					place.resolved = true;
					break;
				}
			}
			else
			{
				place.at = { def.at[0], 0.0f, def.at[2], true };
				place.yaw = XMConvertToRadians(def.yawDegrees);
				place.resolved = true;
			}
		}

		mPlaceNames.clear();
		for (const PlaceDef& def : defs) mPlaceNames.push_back(def.id);
		mPlaceCache.clear();

		// Gatherings: a ring of spots around the campfire, the clock tower's square and the
		// graves, everyone facing the middle (slot = cast index).
		const char* const kAnchors[][2] = { { "fire", "Campfire" }, { "square", "Clock Tower (scaffolding)" }, { "graves", "Grave" } };
		for (const auto& anchor : kAnchors)
		{
			XMFLOAT4X4 stored;
			const bool found = lookup && lookup(anchor[1], stored);
			for (int k = 0; k < cfg::kGatherSlots; ++k)
			{
				Place place;
				if (found)
				{
					const float a = XM_2PI * k / cfg::kGatherSlots;
					const float r = cfg::kGatherRadius[std::string(anchor[0]) == "square" ? 1 : 0] + (k % 2) * 1.3f;
					place.at = { stored._41 + std::sin(a) * r, 0.0f, stored._43 + std::cos(a) * r, true };
					place.yaw = std::atan2(-std::sin(a), -std::cos(a));
					place.resolved = true;
				}
				mPlaces.push_back(place);
				mPlaceNames.push_back(std::string("gather.") + anchor[0] + "#" + std::to_string(k));
			}
		}

		mGraph.Build(cfg::kPathSegments, std::size(cfg::kPathSegments), cfg::kNpcPathLink,
			[this](float ax, float az, float bx, float bz) { return Dry(ax, az, bx, bz); });
		for (const auto& b : cfg::kBridges) mGraph.AddBridge(b[0], b[1], b[2], b[3]);
	}

	double Town::WalkDays(float metres)
	{
		return metres / cfg::kNpcWalkSpeed * cfg::kSurfaceRate / 86400.0;
	}


	float Town::JitterLimit(const std::vector<ScheduleBlock>& day, int index)
	{
		// The block at midnight carries on from the night before: it never moves.
		const float hour = day[index].hour;
		if (index == 0 || hour <= 0.0f) return 0.0f;
		const float before = hour - day[index - 1].hour;
		const float after = (index + 1 < static_cast<int>(day.size()) ? day[index + 1].hour : 24.0f) - hour;
		return std::min({ cfg::kNpcJitterHours, before * 0.3f, after * 0.3f });
	}

	std::vector<Town::Block> Town::DayBlocks(size_t npc, long long day, int era, const PlayerLog* log) const
	{
		std::vector<Block> blocks;
		const std::vector<ScheduleBlock>& defs = DayFor(Cast()[npc], era);
		auto place = [&](const char* id) -> int
		{
			const auto found = mPlaceCache.find(id);
			if (found != mPlaceCache.end()) return found->second;
			return mPlaceCache[id] = PlaceIndex(id);
		};
		for (int i = 0; i < static_cast<int>(defs.size()); ++i)
		{
			const ScheduleBlock& def = defs[i];
			const float limit = JitterLimit(defs, i);
			const float jitter = (Hash01(static_cast<uint32_t>(npc) + 1u, static_cast<uint32_t>(day + 100000), static_cast<uint32_t>(i)) * 2.0f - 1.0f) * limit;
			Block b;
			b.index = i;
			b.start = static_cast<double>(day) + (def.hour + jitter) / 24.0;
			b.place = place(def.place);
			b.activity = def.activity;
			if (def.rainPlace && WeatherAt(b.start).rain > cfg::kNpcRainThreshold)
			{
				b.place = place(def.rainPlace);
				b.activity = def.rainActivity;
				b.rain = true;
			}
			blocks.push_back(b);
		}
		if (blocks.empty()) return blocks;

		// Gatherings they go to: there from the start to the end, then back to what they'd be doing.
		for (const TownEvent& g : GatheringsOverlapping(static_cast<double>(day), static_cast<double>(day + 1), log))
		{
			if (!Attends(g, static_cast<int>(npc), log)) continue;
			const int spot = PlaceIndex(("gather." + g.gather + "#" + std::to_string(static_cast<int>(npc) % cfg::kGatherSlots)).c_str());
			if (spot < 0 || !mPlaces[spot].resolved) continue;
			// Set off early enough to be there when it starts.
			Block before = blocks.front(), resume = blocks.front();
			for (const Block& b : blocks)
			{
				if (b.start <= g.day) before = b;
				if (b.start <= g.until) resume = b;
			}
			const double depart = before.place >= 0 && before.place != spot && mPlaces[before.place].resolved
				? g.day - WalkDays(RouteBetween(before.place, spot).length) - 0.25 / 24.0 : g.day;
			resume.start = g.until;
			std::vector<Block> spliced;
			for (const Block& b : blocks)
				if (b.start < depart || b.start >= g.until) spliced.push_back(b);
			Block at;
			at.index = -1;
			at.start = std::max(depart, static_cast<double>(day));
			at.place = spot;
			at.activity = Activity::Stand;
			at.gathering = true;
			spliced.push_back(at);
			spliced.push_back(resume);
			std::stable_sort(spliced.begin(), spliced.end(), [](const Block& a, const Block& b) { return a.start < b.start; });
			blocks = std::move(spliced);
		}
		return blocks;
	}

	bool Town::BlocksAt(size_t npc, double days, int era, const PlayerLog* log, Block& current, Block& previous, Block* before) const
	{
		const long long day = static_cast<long long>(std::floor(days));
		std::vector<Block> blocks = DayBlocks(npc, day, era, log);
		if (blocks.empty()) return false;
		// With the two days before, there's always a block before the one before.
		for (long long back = 1; back <= 2; ++back)
		{
			std::vector<Block> earlier = DayBlocks(npc, day - back, era, log);
			blocks.insert(blocks.begin(), earlier.begin(), earlier.end());
		}
		// One block after another at the same place is one stay (the night goes on past midnight).
		for (size_t k = 1; k < blocks.size();)
			if (blocks[k].place == blocks[k - 1].place && blocks[k].activity == blocks[k - 1].activity) blocks.erase(blocks.begin() + k);
			else ++k;
		int index = static_cast<int>(blocks.size()) - 1;
		while (index > 0 && blocks[index].start > days) --index;
		index = std::max(index, 2);
		current = blocks[index];
		previous = blocks[index - 1];
		if (before) *before = blocks[index - 2];
		return true;
	}

	Town::Route Town::RouteFrom(const RoutePoint& start, int to) const
	{
		Route route;
		const Place& b = mPlaces[to];
		route.points.push_back(start);
		const RoutePoint entry = b.approach.empty() ? b.at : b.approach.front();
		const std::vector<XMFLOAT2> way = mGraph.Route(start.x, start.z, entry.x, entry.z);
		for (size_t i = 1; i + 1 < way.size(); ++i) route.points.push_back({ way[i].x, 0.0f, way[i].y, true });
		for (const RoutePoint& p : b.approach) route.points.push_back(p);
		route.points.push_back(b.at);
		route.along.push_back(0.0f);
		for (size_t i = 1; i < route.points.size(); ++i)
			route.along.push_back(route.along.back() + std::hypot(route.points[i].x - route.points[i - 1].x, route.points[i].z - route.points[i - 1].z));
		route.length = route.along.back();
		return route;
	}

	const Town::Route& Town::RouteBetween(int from, int to) const
	{
		const auto key = std::make_pair(from, to);
		auto found = mRoutes.find(key);
		if (found != mRoutes.end()) return found->second;

		Route route;
		const Place& a = mPlaces[from];
		const Place& b = mPlaces[to];
		// In the same building (their ways in start alike): back only as far as the ways part.
		size_t common = 0;
		auto same = [](const RoutePoint& p, const RoutePoint& q) { return std::fabs(p.x - q.x) + std::fabs(p.z - q.z) < 0.1f; };
		while (common < a.approach.size() && common < b.approach.size() && same(a.approach[common], b.approach[common])) ++common;
		if (common > 0)
		{
			route.points.push_back(a.at);
			for (size_t k = a.approach.size(); k-- > common;) route.points.push_back(a.approach[k]);
			if (common == a.approach.size() && common == b.approach.size()) { /* straight across the room */ }
			else route.points.push_back(a.approach[common - 1]);
			for (size_t k = common; k < b.approach.size(); ++k) route.points.push_back(b.approach[k]);
			route.points.push_back(b.at);
		}
		else
		{
		route.points.push_back(a.at);
		for (auto it = a.approach.rbegin(); it != a.approach.rend(); ++it) route.points.push_back(*it);
		const RoutePoint exit = route.points.back();
		const RoutePoint entry = b.approach.empty() ? b.at : b.approach.front();
		const std::vector<XMFLOAT2> way = mGraph.Route(exit.x, exit.z, entry.x, entry.z);
		for (size_t i = 1; i + 1 < way.size(); ++i) route.points.push_back({ way[i].x, 0.0f, way[i].y, true });
		// Over a footbridge: its ends at deck height (the banks'), the water below.
		for (size_t i = 1; i + 1 < route.points.size(); ++i)
		{
			RoutePoint& p = route.points[i];
			RoutePoint& q = route.points[i + 1];
			if (!mGraph.IsBridge({ p.x, p.z }, { q.x, q.z })) continue;
			if (p.ground) { p.y = GroundY(p.x, p.z) + 0.25f; p.ground = false; }
			if (q.ground) { q.y = GroundY(q.x, q.z) + 0.25f; q.ground = false; }
		}
		for (const RoutePoint& p : b.approach) route.points.push_back(p);
		route.points.push_back(b.at);
		}

		route.along.push_back(0.0f);
		for (size_t i = 1; i < route.points.size(); ++i)
		{
			const RoutePoint& p = route.points[i - 1];
			const RoutePoint& q = route.points[i];
			route.along.push_back(route.along.back() + std::hypot(q.x - p.x, q.z - p.z));
		}
		route.length = route.along.back();
		return mRoutes.emplace(key, std::move(route)).first->second;
	}

	RoutePoint Town::PointAlong(const Route& route, float distance, float* yaw) const
	{
		const size_t n = route.points.size();
		size_t k = 0;
		while (k + 2 < n && route.along[k + 1] <= distance) ++k;
		const RoutePoint& p = route.points[k];
		const RoutePoint& q = route.points[std::min(k + 1, n - 1)];
		const float span = route.along[std::min(k + 1, n - 1)] - route.along[k];
		const float t = span > 1e-5f ? std::clamp((distance - route.along[k]) / span, 0.0f, 1.0f) : 1.0f;
		RoutePoint out;
		out.x = p.x + (q.x - p.x) * t;
		out.z = p.z + (q.z - p.z) * t;
		if (p.ground && q.ground) out.y = GroundY(out.x, out.z);
		else
		{
			const float py = p.ground ? GroundY(p.x, p.z) : p.y;
			const float qy = q.ground ? GroundY(q.x, q.z) : q.y;
			out.y = py + (qy - py) * t;
			out.ground = false;
		}
		if (yaw && (std::fabs(q.x - p.x) + std::fabs(q.z - p.z)) > 1e-4f) *yaw = std::atan2(q.x - p.x, q.z - p.z);
		return out;
	}

	Pose Town::Evaluate(size_t npcIndex, double days, int era, const PlayerLog* log) const
	{
		Pose pose;
		if (!mValley || npcIndex >= Cast().size()) return pose;
		const LifeState life = LifeAt(static_cast<int>(npcIndex), days, log);
		if (!life.present) return pose;

		Block current, previous, before;
		if (!BlocksAt(npcIndex, days, era, log, current, previous, &before)) return pose;
		if (current.place < 0 || !mPlaces[current.place].resolved) return pose;
		pose.present = true;
		pose.age = life.age;
		pose.gathering = current.gathering;
		pose.place = current.place;
		pose.rain = current.rain;
		pose.blockStart = current.start;

		const Place& here = mPlaces[current.place];
		const bool moving = previous.place >= 0 && previous.place != current.place && mPlaces[previous.place].resolved;
		if (moving)
		{
			const auto metres = [](double from, double to) { return static_cast<float>((to - from) * 86400.0 / cfg::kSurfaceRate * cfg::kNpcWalkSpeed); };
			// Called away before they got there (a gathering, the rain): they set off from
			// wherever they'd got to on the way.
			Route detour;
			const Route* path = &RouteBetween(previous.place, current.place);
			if (before.place >= 0 && before.place != previous.place && mPlaces[before.place].resolved)
			{
				const Route& last = RouteBetween(before.place, previous.place);
				const float got = metres(previous.start, current.start);
				if (got < last.length)
				{
					detour = RouteFrom(PointAlong(last, got), current.place);
					path = &detour;
				}
			}
			const Route& route = *path;
			const float walked = metres(current.start, days);
			if (walked < route.length)
			{
				float yaw = here.yaw;
				const RoutePoint p = PointAlong(route, walked, &yaw);
				pose.position = { p.x, p.y, p.z };
				pose.yaw = yaw;
				pose.activity = Activity::Walk;
				pose.walked = walked;
				pose.from = previous.place;
				return pose;
			}
		}
		pose.position = { here.at.x, here.at.ground ? GroundY(here.at.x, here.at.z) : here.at.y, here.at.z };
		pose.yaw = here.yaw;
		pose.activity = current.activity;
		return pose;
	}
}
