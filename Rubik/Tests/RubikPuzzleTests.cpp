// Tests for the pure puzzle logic (no engine, no GPU). Run via CTest or directly.
#include "../RubikPuzzle.h"
#include <cstdio>
#include <string>

namespace
{
	int gFailures = 0;

	void Check(bool condition, const char* what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what);
		if (!condition) ++gFailures;
	}

	// Inverse of a move sequence: reverse order, flip direction.
	std::string Inverse(const std::string& moves)
	{
		std::vector<std::pair<RubikPuzzle::Face, bool>> turns;
		RubikPuzzle::ParseMoves(moves, turns);
		std::string result;
		for (auto it = turns.rbegin(); it != turns.rend(); ++it)
		{
			result += RubikPuzzle::Letter(it->first);
			if (it->second) result += '\'';
			result += ' ';
		}
		return result;
	}
}

int main()
{
	using Face = RubikPuzzle::Face;
	const Face faces[] = { Face::Right, Face::Left, Face::Up, Face::Down, Face::Front, Face::Back };

	{
		RubikPuzzle puzzle;
		Check(puzzle.IsSolved(), "new puzzle is solved");
	}
	{
		bool allOk = true;
		for (Face face : faces)
		{
			RubikPuzzle puzzle;
			puzzle.Turn(face, true);
			allOk &= !puzzle.IsSolved();
			for (int i = 0; i < 3; ++i) puzzle.Turn(face, true);
			allOk &= puzzle.IsSolved();
		}
		Check(allOk, "every face: one turn unsolves, four turns solve");
	}
	{
		bool allOk = true;
		for (Face face : faces)
		{
			RubikPuzzle puzzle;
			puzzle.Turn(face, true);
			puzzle.Turn(face, false);
			allOk &= puzzle.IsSolved();
		}
		Check(allOk, "every face: clockwise then counter-clockwise is identity");
	}
	{
		RubikPuzzle puzzle;
		for (int i = 0; i < 6; ++i) puzzle.ApplyMoves("R U R' U'");
		Check(puzzle.IsSolved(), "(R U R' U') x6 returns to solved (sexy move has order 6)");
	}
	{
		RubikPuzzle puzzle;
		puzzle.ApplyMoves("R U R' U'");
		Check(!puzzle.IsSolved(), "(R U R' U') x1 is not solved");
	}
	{
		RubikPuzzle puzzle;
		puzzle.ApplyMoves("F2");
		RubikPuzzle twice;
		twice.ApplyMoves("F F");
		bool same = true;
		for (int i = 0; i < 27; ++i)
			for (int j = 0; j < 3; ++j)
				same &= puzzle.Cubies()[i].position[j] == twice.Cubies()[i].position[j];
		Check(same, "F2 equals F F");
	}
	{
		bool allOk = true;
		for (int run = 0; run < 20; ++run)
		{
			const std::string scramble = RubikPuzzle::RandomScramble(25);
			RubikPuzzle puzzle;
			puzzle.ApplyMoves(scramble);
			const bool scrambled = !puzzle.IsSolved();
			puzzle.ApplyMoves(Inverse(scramble));
			allOk &= scrambled && puzzle.IsSolved();
		}
		Check(allOk, "20 random scrambles are unsolved and their inverses solve them");
	}
	{
		std::vector<std::pair<Face, bool>> turns;
		Check(RubikPuzzle::ParseMoves("R U' F2 B'2 l d", turns) && turns.size() == 8, "notation parser handles ', 2 and lowercase");
		turns.clear();
		Check(!RubikPuzzle::ParseMoves("R X", turns), "notation parser rejects unknown faces");
	}
	{
		// Front is the -Z face; a clockwise F (seen from the front) carries the top
		// front-row edge to the right side.
		RubikPuzzle puzzle;
		int upFront = -1;
		for (int i = 0; i < 27; ++i)
		{
			const auto& c = puzzle.Cubies()[i];
			if (c.home[0] == 0 && c.home[1] == 1 && c.home[2] == -1) upFront = i;
		}
		puzzle.Turn(Face::Front, true);
		const auto& moved = puzzle.Cubies()[upFront];
		Check(moved.position[0] == 1 && moved.position[1] == 0 && moved.position[2] == -1, "F moves the up-front edge to the right-front position");
	}

	std::printf("%s (%d failure%s)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures, gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}
