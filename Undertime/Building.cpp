#include "Building.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace build
{
	const std::vector<PieceDef>& Pieces()
	{
		static const std::vector<PieceDef> pieces = {
			{ "foundation", "Stone foundation", "Floors", Slot::Floor, { { "Stone", 16 } }, 6 },
			{ "plank_floor", "Plank floor", "Floors", Slot::Floor, { { "timber", 2 } }, 3 },
			{ "farm_plot", "Farm plot", "Floors", Slot::Floor, {}, 2, 1, 1, Use::None, false },
			{ "stone_wall", "Stone wall", "Walls", Slot::Edge, { { "Stone", 16 } }, 6 },
			{ "plank_wall", "Plank wall", "Walls", Slot::Edge, { { "timber", 3 } }, 3 },
			{ "window_hole", "Wall with a window hole", "Walls", Slot::Edge, { { "timber", 3 } }, 3 },
			{ "window", "Glass window", "Walls", Slot::Edge, { { "timber", 3 }, { "glass", 1 } }, 4, 2 },
			{ "half_wall", "Half wall", "Walls", Slot::Edge, { { "timber", 2 } }, 2, 1, 1, Use::None, false },
			{ "doorway", "Doorway (2 m)", "Walls", Slot::Edge, { { "timber", 4 } }, 4, 1, 2 },
			{ "door", "Door (2 m, opens)", "Walls", Slot::Edge, { { "timber", 6 } }, 5, 1, 2, Use::Door },
			{ "roof", "Roof slope", "Roofs", Slot::Cell, { { "timber", 3 } }, 3 },
			{ "roof_ridge", "Roof ridge", "Roofs", Slot::Cell, { { "timber", 3 } }, 3 },
			{ "stairs", "Stairs", "Roofs", Slot::Cell, { { "timber", 4 } }, 4 },
			{ "fence", "Fence", "Fences", Slot::Edge, { { "timber", 1 } }, 1, 1, 1, Use::None, false },
			{ "gate", "Gate", "Fences", Slot::Edge, { { "timber", 2 } }, 2, 1, 1, Use::Door, false },
			{ "workbench", "Workbench", "Furniture", Slot::Cell, { { "timber", 6 } }, 4, 1, 1, Use::Workbench, false },
			{ "crate", "Storage crate", "Furniture", Slot::Cell, { { "timber", 4 } }, 3, 1, 1, Use::Crate, false },
			{ "fire_pit", "Fire pit", "Furniture", Slot::Cell, { { "Stone", 12 } }, 4, 1, 1, Use::FirePit, false },
			{ "bed", "Bed", "Furniture", Slot::Cell, { { "timber", 6 }, { "leather", 1 } }, 4, 1, 1, Use::Bed, false },
			{ "table", "Table", "Furniture", Slot::Cell, { { "timber", 4 } }, 3, 1, 1, Use::None, false },
			{ "stove", "Stove and chimney", "Furniture", Slot::Cell, { { "Stone", 20 }, { "Copper", 4 } }, 6, 2, 1, Use::FirePit, false },
			{ "lamp", "Lamp", "Furniture", Slot::Cell, { { "timber", 1 }, { "oil", 1 } }, 1, 1, 1, Use::Lamp, false },
		};
		return pieces;
	}

	int PieceIndex(const std::string& id)
	{
		const auto& all = Pieces();
		for (size_t i = 0; i < all.size(); ++i)
			if (id == all[i].id) return static_cast<int>(i);
		return -1;
	}

	std::vector<std::pair<std::string, int>> SwingCost(const PieceDef& def, int n)
	{
		// Each material spread evenly: swing n takes floor((n+1)c/s) - floor(nc/s).
		std::vector<std::pair<std::string, int>> out;
		for (const auto& [id, count] : def.cost)
		{
			const int take = (n + 1) * count / def.swings - n * count / def.swings;
			if (take > 0) out.push_back({ id, take });
		}
		return out;
	}

	std::vector<std::pair<std::string, int>> PaidFor(const PieceDef& def, int swings)
	{
		std::vector<std::pair<std::string, int>> out;
		const int done = std::clamp(swings, 0, def.swings);
		for (const auto& [id, count] : def.cost)
		{
			const int paid = done * count / def.swings;
			if (paid > 0) out.push_back({ id, paid });
		}
		return out;
	}

	// ---- Where pieces are --------------------------------------------------------------

	namespace
	{
		// An edge as (axis, boundary x, boundary z): axis 0 runs along x on the z = bz line,
		// axis 1 runs along z on the x = bx line.
		struct EdgeKey { int axis, bx, bz; };
		EdgeKey EdgeOf(const Piece& p)
		{
			switch (p.turn & 3)
			{
			case 0: return { 0, p.x, p.z };
			case 2: return { 0, p.x, p.z + 1 };
			case 3: return { 1, p.x, p.z };
			default: return { 1, p.x + 1, p.z };
			}
		}
		bool SameEdge(const EdgeKey& a, const EdgeKey& b) { return a.axis == b.axis && a.bx == b.bx && a.bz == b.bz; }
		// The cells either side of an edge.
		void EdgeCells(const EdgeKey& e, int cells[2][2])
		{
			if (e.axis == 0) { cells[0][0] = e.bx; cells[0][1] = e.bz - 1; cells[1][0] = e.bx; cells[1][1] = e.bz; }
			else { cells[0][0] = e.bx - 1; cells[0][1] = e.bz; cells[1][0] = e.bx; cells[1][1] = e.bz; }
		}
		bool IsRoof(const Piece& p) { const std::string id = p.Def().id; return id == "roof" || id == "roof_ridge"; }
	}

	bool Plan::Overlaps(const Piece& a, const Piece& b) const
	{
		const PieceDef& da = a.Def();
		const PieceDef& db = b.Def();
		const int a0 = a.y, a1 = a.y + da.levels, b0 = b.y, b1 = b.y + db.levels;
		if (a1 <= b0 || b1 <= a0) return false;
		if (da.slot != db.slot) return false;
		if (da.slot == Slot::Edge) return SameEdge(EdgeOf(a), EdgeOf(b));
		return a.x == b.x && a.z == b.z;
	}

	bool Plan::Supported(const std::vector<Piece>& pieces, std::vector<char>* result) const
	{
		const size_t n = pieces.size();
		// How far a piece hangs out from what holds it up (floors 2 cells, roofs 3).
		std::vector<int> reach(n, INT_MAX);
		for (size_t i = 0; i < n; ++i)
			if (pieces[i].y == 0) reach[i] = 0;   // on the ground
		auto holds = [&](size_t j) { return reach[j] != INT_MAX && pieces[j].Def().holdsUp; };
		for (bool changed = true; changed;)
		{
			changed = false;
			for (size_t i = 0; i < n; ++i)
			{
				if (reach[i] == 0) continue;
				const Piece& p = pieces[i];
				const PieceDef& d = p.Def();
				int best = reach[i];
				for (size_t j = 0; j < n; ++j)
				{
					if (j == i || !holds(j)) continue;
					const Piece& q = pieces[j];
					const PieceDef& e = q.Def();
					const int qTop = q.y + e.levels;   // the level just above it
					if (d.slot == Slot::Floor || d.slot == Slot::Cell)
					{
						// On a wall of its cell, one level down.
						if (e.slot == Slot::Edge && qTop == p.y)
						{
							int cells[2][2];
							EdgeCells(EdgeOf(q), cells);
							for (auto& c : cells) if (c[0] == p.x && c[1] == p.z) best = 0;
						}
						// A cell piece on its cell's floor.
						if (d.slot == Slot::Cell && e.slot == Slot::Floor && q.y == p.y && q.x == p.x && q.z == p.z) best = std::min(best, 0);
						// Hanging out from the same kind next to it.
						const bool sameKind = (d.slot == Slot::Floor && e.slot == Slot::Floor) || (IsRoof(p) && IsRoof(q));
						const int limit = d.slot == Slot::Floor ? 2 : 3;
						if (sameKind && q.y == p.y && std::abs(q.x - p.x) + std::abs(q.z - p.z) == 1 && reach[j] + 1 <= limit) best = std::min(best, reach[j] + 1);
					}
					else   // an edge
					{
						// On the same edge, a level down.
						if (e.slot == Slot::Edge && qTop == p.y && SameEdge(EdgeOf(p), EdgeOf(q))) best = 0;
						// On a floor beside it, at its level.
						if (e.slot == Slot::Floor && q.y == p.y)
						{
							int cells[2][2];
							EdgeCells(EdgeOf(p), cells);
							for (auto& c : cells) if (c[0] == q.x && c[1] == q.z) best = 0;
						}
					}
				}
				if (best < reach[i]) { reach[i] = best; changed = true; }
			}
		}
		bool all = true;
		if (result) result->assign(n, 0);
		for (size_t i = 0; i < n; ++i)
		{
			const bool ok = reach[i] != INT_MAX;
			if (result) (*result)[i] = ok ? 1 : 0;
			all = all && ok;
		}
		return all;
	}

	std::string Plan::CanPlace(const Piece& piece) const
	{
		if (piece.type < 0 || piece.type >= static_cast<int>(build::Pieces().size())) return "no such piece";
		const PieceDef& d = piece.Def();
		const bool edge = d.slot == Slot::Edge;
		if (edge)
		{
			const EdgeKey e = EdgeOf(piece);
			if (e.bx < 0 || e.bz < 0 || e.bx > mSizeX || e.bz > mSizeZ || (e.axis == 0 && e.bx >= mSizeX) || (e.axis == 1 && e.bz >= mSizeZ)) return "outside your claim";
		}
		else if (piece.x < 0 || piece.z < 0 || piece.x >= mSizeX || piece.z >= mSizeZ) return "outside your claim";
		if (piece.y < 0 || piece.y + d.levels > mLevels) return "too high";
		for (const Piece& p : mPieces)
			if (Overlaps(p, piece)) return std::string("there's a ") + p.Def().name + " there";
		std::vector<Piece> with = mPieces;
		with.push_back(piece);
		std::vector<char> ok;
		Supported(with, &ok);
		if (!ok.back()) return "nothing would hold it up";
		return {};
	}

	int Plan::Place(const Piece& piece)
	{
		mPieces.push_back(piece);
		return static_cast<int>(mPieces.size()) - 1;
	}

	std::string Plan::CanRemove(int index) const
	{
		if (index < 0 || index >= static_cast<int>(mPieces.size())) return "nothing there";
		std::vector<Piece> without = mPieces;
		without.erase(without.begin() + index);
		std::vector<char> ok;
		Supported(without, &ok);
		for (size_t i = 0; i < without.size(); ++i)
			if (!ok[i]) return std::string("the ") + without[i].Def().name + " rests on it";
		return {};
	}

	Piece Plan::Remove(int index)
	{
		const Piece p = mPieces[index];
		mPieces.erase(mPieces.begin() + index);
		return p;
	}

	bool Plan::AllSupported() const { return Supported(mPieces, nullptr); }

	// ---- Models ------------------------------------------------------------------------

	namespace
	{
		struct Palette
		{
			VoxelModel& m;
			int used = 0;
			uint8_t operator()(uint32_t srgb, float glow = 0.0f, float glass = 0.0f)
			{
				const DirectX::XMFLOAT4 c{ ((srgb >> 16) & 0xff) / 255.0f, ((srgb >> 8) & 0xff) / 255.0f, (srgb & 0xff) / 255.0f, 1.0f };
				for (int i = 1; i <= used; ++i)
					if (m.palette[i].x == c.x && m.palette[i].y == c.y && m.palette[i].z == c.z && m.materials[i].emissive == glow && m.materials[i].glass == glass) return static_cast<uint8_t>(i);
				if (used >= 255) return 255;
				++used;
				m.palette[used] = c;
				m.materials[used].emissive = glow;
				m.materials[used].glass = glass;
				return static_cast<uint8_t>(used);
			}
		};
		void Fill(VoxelModel& m, int x0, int y0, int z0, int x1, int y1, int z1, uint8_t c)
		{
			for (int z = z0; z < z1; ++z)
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x) m.Set(x, y, z, c);
		}
		uint8_t Grain(const std::vector<uint8_t>& colours, int x, int y, int z)
		{
			uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			return colours[h % colours.size()];
		}
		void Grained(VoxelModel& m, int x0, int y0, int z0, int x1, int y1, int z1, const std::vector<uint8_t>& colours)
		{
			for (int z = z0; z < z1; ++z)
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x) m.Set(x, y, z, Grain(colours, x, y, z));
		}

		// A cell piece, facing +z at turn 0 (4 x 5 x 4: y 0 is the floor slab's layer).
		VoxelModel CellModel(const std::string& id)
		{
			VoxelModel m;
			m.Resize({ 4, 5, 4 });
			Palette pal{ m };
			const std::vector<uint8_t> wood{ pal(0x8a6038), pal(0xa47a4c), pal(0x7c5634) };
			const uint8_t dark = pal(0x6a4628), iron = pal(0x4a4a50);
			const std::vector<uint8_t> stone{ pal(0x7a6e60), pal(0x6a5e50), pal(0x857766) };
			if (id == "roof")
			{
				// Steps rising towards -z, two voxels deep so it's closed.
				for (int k = 0; k < 4; ++k) Grained(m, 0, 4 - k, k, 4, 5 - k, k + 1, { pal(0x7a4a32), pal(0x8a5638), pal(0x6a4030) }), Grained(m, 0, 3 - k, k, 4, 4 - k, k + 1, wood);
			}
			else if (id == "roof_ridge")
			{
				for (int k = 0; k < 2; ++k)
				{
					Grained(m, 0, 3 + k, k, 4, 4 + k, k + 1, { pal(0x7a4a32), pal(0x8a5638) });
					Grained(m, 0, 3 + k, 3 - k, 4, 4 + k, 4 - k, { pal(0x7a4a32), pal(0x8a5638) });
				}
				Fill(m, 0, 4, 1, 4, 5, 3, pal(0x6a4030));
			}
			else if (id == "stairs")
			{
				for (int k = 0; k < 4; ++k) Grained(m, 0, 1, k, 4, 2 + k, k + 1, wood);
			}
			else if (id == "workbench")
			{
				Grained(m, 0, 3, 1, 4, 4, 4, wood);
				Fill(m, 0, 1, 1, 1, 3, 2, dark); Fill(m, 3, 1, 1, 4, 3, 2, dark); Fill(m, 0, 1, 3, 1, 3, 4, dark); Fill(m, 3, 1, 3, 4, 3, 4, dark);
				Fill(m, 1, 4, 3, 2, 5, 4, iron);   // a vice
			}
			else if (id == "crate")
			{
				Grained(m, 0, 1, 0, 4, 4, 4, wood);
				Fill(m, 0, 1, 0, 4, 2, 1, dark); Fill(m, 0, 3, 0, 4, 4, 1, dark);
			}
			else if (id == "fire_pit")
			{
				Grained(m, 0, 1, 0, 4, 2, 4, stone);
				Fill(m, 1, 1, 1, 3, 2, 3, pal(0x2a2018));
				Fill(m, 1, 2, 1, 3, 3, 3, pal(0xff9a40, 4.0f));   // embers (glow 3+: always)
			}
			else if (id == "bed")
			{
				Grained(m, 0, 1, 0, 4, 2, 4, wood);
				Fill(m, 0, 2, 0, 4, 3, 4, pal(0x8a3a30));
				Fill(m, 0, 2, 3, 4, 3, 4, pal(0xe8e0d0));   // pillow at +z
				Fill(m, 0, 2, 3, 4, 4, 4, dark);
			}
			else if (id == "table")
			{
				Grained(m, 0, 3, 0, 4, 4, 4, wood);
				Fill(m, 0, 1, 0, 1, 3, 1, dark); Fill(m, 3, 1, 0, 4, 3, 1, dark); Fill(m, 0, 1, 3, 1, 3, 4, dark); Fill(m, 3, 1, 3, 4, 3, 4, dark);
			}
			else if (id == "stove")
			{
				Grained(m, 0, 1, 0, 4, 3, 4, stone);
				Fill(m, 1, 1, 3, 3, 2, 4, pal(0xff8a30, 4.0f));
				Grained(m, 1, 3, 0, 3, 5, 2, stone);   // the chimney's foot (it goes up through the roof as you build)
			}
			else if (id == "lamp")
			{
				Fill(m, 1, 4, 1, 3, 5, 3, dark);
				Fill(m, 1, 3, 1, 3, 4, 3, pal(0xffd080, 2.0f));   // lights up at night
			}
			return m;
		}

		VoxelModel FloorModel(const std::string& id)
		{
			VoxelModel m;
			m.Resize({ 4, 1, 4 });
			Palette pal{ m };
			if (id == "foundation") Grained(m, 0, 0, 0, 4, 1, 4, { pal(0x7a6e60), pal(0x6a5e50), pal(0x857766), pal(0x726656) });
			else if (id == "farm_plot")
			{
				Grained(m, 0, 0, 0, 4, 1, 4, { pal(0x5a4030), pal(0x4a3424) });
				for (int x = 0; x < 4; x += 2) Fill(m, x, 0, 0, x + 1, 1, 4, pal(0x3e2a1c));   // furrows
			}
			else Grained(m, 0, 0, 0, 4, 1, 4, { pal(0x9a7046), pal(0x8a6038), pal(0xa47a4c) });
			return m;
		}

		// An edge piece along x (axis 0), `levels` tall: 4 x 4L x 1.
		VoxelModel EdgeModel(const std::string& id, int levels)
		{
			VoxelModel m;
			const int h = 4 * levels;
			m.Resize({ 4, static_cast<uint32_t>(h), 1 });
			Palette pal{ m };
			const std::vector<uint8_t> wood{ pal(0x8a6038), pal(0xa47a4c), pal(0x7c5634) };
			const uint8_t dark = pal(0x6a4628);
			if (id == "stone_wall") Grained(m, 0, 0, 0, 4, h, 1, { pal(0x7a6e60), pal(0x6a5e50), pal(0x857766), pal(0x726656) });
			else if (id == "plank_wall") Grained(m, 0, 0, 0, 4, h, 1, wood);
			else if (id == "half_wall") Grained(m, 0, 0, 0, 4, 2, 1, wood);
			else if (id == "window_hole" || id == "window")
			{
				Grained(m, 0, 0, 0, 4, h, 1, wood);
				Fill(m, 1, 1, 0, 3, 3, 1, id == "window" ? pal(0xa8c8d8, 0.0f, 0.7f) : 0);
			}
			else if (id == "doorway" || id == "door")
			{
				Fill(m, 0, 0, 0, 1, h, 1, dark);
				Fill(m, 3, 0, 0, 4, h, 1, dark);
				Grained(m, 0, h - 1, 0, 4, h, 1, wood);
				if (id == "door") Grained(m, 1, 0, 0, 3, h - 1, 1, { pal(0x6e4a2a), pal(0x7a5432) });
			}
			else if (id == "fence")
			{
				Fill(m, 0, 0, 0, 1, 3, 1, dark);
				Grained(m, 0, 1, 0, 4, 2, 1, wood);
				Grained(m, 0, 2, 0, 4, 3, 1, wood);
			}
			else if (id == "gate")
			{
				Fill(m, 0, 0, 0, 1, 3, 1, dark);
				Fill(m, 3, 0, 0, 4, 3, 1, dark);
				Grained(m, 1, 1, 0, 3, 3, 1, wood);
			}
			return m;
		}

		VoxelModel Turned(const VoxelModel& in, int turn)
		{
			turn &= 3;
			if (turn == 0) return in;
			VoxelModel out;
			const int sx = static_cast<int>(in.size.x), sy = static_cast<int>(in.size.y), sz = static_cast<int>(in.size.z);
			out.Resize(turn % 2 ? DirectX::XMUINT3{ in.size.z, in.size.y, in.size.x } : in.size);
			out.palette = in.palette;
			out.materials = in.materials;
			for (int z = 0; z < sz; ++z)
				for (int y = 0; y < sy; ++y)
					for (int x = 0; x < sx; ++x)
					{
						const uint8_t v = in.At(x, y, z);
						if (!v) continue;
						int nx = x, nz = z;
						if (turn == 1) { nx = sz - 1 - z; nz = x; }
						else if (turn == 2) { nx = sx - 1 - x; nz = sz - 1 - z; }
						else { nx = z; nz = sx - 1 - x; }
						out.Set(nx, y, nz, v);
					}
			return out;
		}
	}

	VoxelModel Plan::PieceModel(const Piece& piece, float builtFraction)
	{
		const PieceDef& d = piece.Def();
		VoxelModel m;
		if (d.slot == Slot::Floor) m = FloorModel(d.id);
		else if (d.slot == Slot::Cell) m = Turned(CellModel(d.id), piece.turn);
		else
		{
			m = EdgeModel(d.id, d.levels);
			if (EdgeOf(piece).axis == 1) m = Turned(m, 1);
		}
		if (builtFraction < 1.0f)
		{
			// A site: only the bottom part so far (at least a layer once it's started).
			const int sy = static_cast<int>(m.size.y);
			const int keep = std::clamp(static_cast<int>(std::ceil(sy * builtFraction)), builtFraction > 0.0f ? 1 : 0, sy);
			for (int z = 0; z < static_cast<int>(m.size.z); ++z)
				for (int y = keep; y < sy; ++y)
					for (int x = 0; x < static_cast<int>(m.size.x); ++x) m.Set(x, y, z, 0);
		}
		return m;
	}

	std::array<int, 3> Plan::PieceOrigin(const Piece& piece)
	{
		const PieceDef& d = piece.Def();
		const int c = kVoxelsPerCell;
		if (d.slot == Slot::Floor) return { piece.x * c, piece.y * c, piece.z * c };
		if (d.slot == Slot::Cell) return { piece.x * c, piece.y * c, piece.z * c };
		const EdgeKey e = EdgeOf(piece);
		// Walls stand on the slab (one voxel up); along x on the z = bz line, or along z on x = bx.
		if (e.axis == 0) return { e.bx * c, piece.y * c + 1, e.bz * c };
		return { e.bx * c, piece.y * c + 1, e.bz * c };
	}

	int Plan::PieceAt(int vx, int vy, int vz) const
	{
		for (size_t i = 0; i < mPieces.size(); ++i)
		{
			const VoxelModel m = PieceModel(mPieces[i]);
			const std::array<int, 3> o = PieceOrigin(mPieces[i]);
			if (vx >= o[0] && vy >= o[1] && vz >= o[2] && vx < o[0] + static_cast<int>(m.size.x) && vy < o[1] + static_cast<int>(m.size.y) && vz < o[2] + static_cast<int>(m.size.z))
				return static_cast<int>(i);
		}
		return -1;
	}

	VoxelModel Plan::Model(bool sites) const
	{
		const int c = kVoxelsPerCell;
		VoxelModel out;
		out.Resize({ static_cast<uint32_t>(mSizeX * c + 1), static_cast<uint32_t>(mLevels * c + 2), static_cast<uint32_t>(mSizeZ * c + 1) });
		Palette pal{ out };
		static const int doorway = PieceIndex("doorway");
		for (const Piece& piece : mPieces)
		{
			if (!piece.Built() && !sites) continue;
			Piece p = piece;
			if (p.open)
			{
				// An open door leaves its frame; an open gate, nothing.
				if (std::string(p.Def().id) != "door") continue;
				p.type = doorway;
				p.swings = p.Def().swings;
			}
			const VoxelModel m = PieceModel(p, p.Built() ? 1.0f : static_cast<float>(p.swings) / p.Def().swings);
			const std::array<int, 3> o = PieceOrigin(p);
			for (int z = 0; z < static_cast<int>(m.size.z); ++z)
				for (int y = 0; y < static_cast<int>(m.size.y); ++y)
					for (int x = 0; x < static_cast<int>(m.size.x); ++x)
					{
						const uint8_t v = m.At(x, y, z);
						if (!v) continue;
						const DirectX::XMFLOAT4& col = m.palette[v];
						const uint32_t srgb = (static_cast<uint32_t>(std::lround(col.x * 255.0f)) << 16) | (static_cast<uint32_t>(std::lround(col.y * 255.0f)) << 8) | static_cast<uint32_t>(std::lround(col.z * 255.0f));
						out.Set(o[0] + x, o[1] + y, o[2] + z, pal(srgb, m.materials[v].emissive, m.materials[v].glass));
					}
		}
		return out;
	}
}
