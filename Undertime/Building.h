#pragma once
// Building on your claim (Docs/Hauling.md, "Building the claim"): pieces snapped to a grid of
// 1 m cells and 1 m levels, each built up by hand with the hammer from stone blocks, timber
// and the rest. Pure C++ (tested in Tests/HaulingTests.cpp): the piece table, where a piece
// goes, what holds it up, what it costs, and its voxel model.
//
// The grid is the claim's: cell (x, z) is 1 m square from the claim's corner, level y is 1 m
// up from its (flattened) ground. Within a level, a floor is the bottom 0.25 m slab of the
// cell; walls stand on the cell's edges, one block (0.25 m) thick, from the slab's top up 1 m
// (so they meet the next level's floor); everything else fills the cell.
#include <Voxel/VoxelModel.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace build
{
	// Where a piece sits.
	enum class Slot
	{
		Floor,   // the bottom slab of a cell
		Edge,    // a cell's edge (turn 0: the -z edge, 1: +x, 2: +z, 3: -x)
		Cell,    // fills the cell (furniture, stairs, roofs; turned)
	};

	enum class Use { None, Workbench, Crate, FirePit, Bed, Lamp, Door };

	struct PieceDef
	{
		const char* id;
		const char* name;
		const char* group;
		Slot slot;
		std::vector<std::pair<const char*, int>> cost;   // block names / item ids, as econ goods
		int swings = 3;           // hammer swings to build it
		int eraFrom = 1;
		int levels = 1;           // an edge piece two levels tall (the doorway)
		Use use = Use::None;
		bool holdsUp = true;      // other pieces can rest on it (not furniture)
	};

	const std::vector<PieceDef>& Pieces();
	int PieceIndex(const std::string& id);   // -1 if none

	struct Piece
	{
		int type = 0;
		int x = 0, y = 0, z = 0;
		int turn = 0;             // 0..3: quarter turns (an edge piece: which edge)
		int swings = 0;           // swings so far; built when it reaches the piece's swings
		bool open = false;        // a door
		const PieceDef& Def() const { return Pieces()[type]; }
		bool Built() const { return swings >= Def().swings; }
	};

	// The materials taken by swing `n` (0-based) of a piece: the cost spread over its swings.
	std::vector<std::pair<std::string, int>> SwingCost(const PieceDef& def, int n);
	// Everything taken by the first `swings` swings (what taking it down gives back).
	std::vector<std::pair<std::string, int>> PaidFor(const PieceDef& def, int swings);

	class Plan
	{
	public:
		Plan(int sizeX, int sizeZ, int levels) : mSizeX(sizeX), mSizeZ(sizeZ), mLevels(levels) {}
		int SizeX() const { return mSizeX; }
		int SizeZ() const { return mSizeZ; }
		int Levels() const { return mLevels; }
		const std::vector<Piece>& Pieces() const { return mPieces; }
		std::vector<Piece>& Pieces() { return mPieces; }

		// Why `piece` can't go there ("" if it can): outside the claim, the spot is taken, or
		// nothing would hold it up.
		std::string CanPlace(const Piece& piece) const;
		int Place(const Piece& piece);        // index of the new piece (call CanPlace first)
		// Why piece `index` can't be taken down ("" if it can): something rests on it.
		std::string CanRemove(int index) const;
		Piece Remove(int index);
		// The piece whose space holds voxel (vx, vy, vz) of the claim's model, -1 if none.
		int PieceAt(int vx, int vy, int vz) const;
		// Whether every piece is held up (from the ground, through what each rests on).
		bool AllSupported() const;

		// The claim's voxel model: every piece (sites only up to how far they're built if
		// `sites` is set, else built pieces only), at 0.25 m voxels, 4 per cell.
		VoxelModel Model(bool sites) const;
		// One piece's model on its own (the ghost while placing; a site), in a box the size of
		// its cell (edge pieces turned as they stand), and where that box's corner goes in the
		// claim's voxels.
		static VoxelModel PieceModel(const Piece& piece, float builtFraction = 1.0f);
		static std::array<int, 3> PieceOrigin(const Piece& piece);

	private:
		bool Supported(const std::vector<Piece>& pieces, std::vector<char>* result) const;
		bool Overlaps(const Piece& a, const Piece& b) const;
		int mSizeX, mSizeZ, mLevels;
		std::vector<Piece> mPieces;
	};

	constexpr int kVoxelsPerCell = 4;
}
