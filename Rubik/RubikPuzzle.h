#pragma once
#include <DirectXMath.h>
#include <array>
#include <string>
#include <vector>

// Pure puzzle state for a 3x3x3 cube; no rendering.
//
// Each of the 27 cubies has a home grid position in {-1,0,1}^3, a current grid
// position and an orientation stored as an exact integer rotation matrix
// (row-vector convention, like DirectXMath: v_world = v_local * orientation).
// Integer state means repeated turns never drift.
class RubikPuzzle
{
public:
	enum class Face { Right, Left, Up, Down, Front, Back };   // +X -X +Y -Y -Z +Z

	struct Cubie
	{
		int home[3];
		int position[3];
		int orientation[3][3];
	};

	RubikPuzzle();

	void Reset();
	// Clockwise as seen when looking at `face` from outside the cube.
	void Turn(Face face, bool clockwise);
	bool IsSolved() const;

	// Parses standard notation, e.g. "R U' F2 B'2 L". Returns false if a token is invalid
	// (valid moves before it are still applied).
	bool ApplyMoves(const std::string& moves);
	static std::string RandomScramble(int moveCount);
	// Splits notation into individual quarter turns (face, clockwise) without applying them.
	static bool ParseMoves(const std::string& moves, std::vector<std::pair<Face, bool>>& out);

	const std::array<Cubie, 27>& Cubies() const { return mCubies; }
	bool IsInLayer(int cubieIndex, Face face) const;
	DirectX::XMFLOAT4 OrientationQuaternion(int cubieIndex) const;

	static DirectX::XMFLOAT3 Normal(Face face);
	static char Letter(Face face);
	static bool FromLetter(char letter, Face& face);
	// Face whose outward normal is closest to `normal`.
	static Face FromNormal(const DirectX::XMFLOAT3& normal);

private:
	static void Axis(Face face, int& axis, int& layer);

	std::array<Cubie, 27> mCubies;
};
