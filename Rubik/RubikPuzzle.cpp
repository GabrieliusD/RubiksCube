#include "RubikPuzzle.h"
#include <cmath>
#include <cctype>
#include <random>

using namespace DirectX;

RubikPuzzle::RubikPuzzle()
{
	Reset();
}

void RubikPuzzle::Reset()
{
	int i = 0;
	for (int z = -1; z <= 1; ++z)
	for (int y = -1; y <= 1; ++y)
	for (int x = -1; x <= 1; ++x)
	{
		Cubie& c = mCubies[i++];
		c.home[0] = c.position[0] = x;
		c.home[1] = c.position[1] = y;
		c.home[2] = c.position[2] = z;
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 3; ++k)
				c.orientation[r][k] = (r == k) ? 1 : 0;
	}
}

void RubikPuzzle::Axis(Face face, int& axis, int& layer)
{
	switch (face)
	{
	case Face::Right: axis = 0; layer = +1; break;
	case Face::Left:  axis = 0; layer = -1; break;
	case Face::Up:    axis = 1; layer = +1; break;
	case Face::Down:  axis = 1; layer = -1; break;
	case Face::Front: axis = 2; layer = -1; break;   // the default camera looks along +Z at the front
	case Face::Back:  axis = 2; layer = +1; break;
	}
}

XMFLOAT3 RubikPuzzle::Normal(Face face)
{
	int axis, layer;
	Axis(face, axis, layer);
	XMFLOAT3 n{ 0, 0, 0 };
	(&n.x)[axis] = static_cast<float>(layer);
	return n;
}

bool RubikPuzzle::IsInLayer(int cubieIndex, Face face) const
{
	int axis, layer;
	Axis(face, axis, layer);
	return mCubies[cubieIndex].position[axis] == layer;
}

void RubikPuzzle::Turn(Face face, bool clockwise)
{
	int axis, layer;
	Axis(face, axis, layer);

	// DirectXMath is left-handed: a positive angle about an axis is clockwise when
	// viewed from the positive end of that axis, i.e. from outside this face.
	const XMFLOAT3 n = Normal(face);
	const XMMATRIX rotation = XMMatrixRotationAxis(XMLoadFloat3(&n), clockwise ? XM_PIDIV2 : -XM_PIDIV2);
	XMFLOAT4X4 rf;
	XMStoreFloat4x4(&rf, rotation);
	int r[3][3];
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			r[i][j] = static_cast<int>(std::lround(rf.m[i][j]));

	for (Cubie& c : mCubies)
	{
		if (c.position[axis] != layer) continue;

		int p[3];
		for (int j = 0; j < 3; ++j)
			p[j] = c.position[0] * r[0][j] + c.position[1] * r[1][j] + c.position[2] * r[2][j];
		for (int j = 0; j < 3; ++j) c.position[j] = p[j];

		int o[3][3];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				o[i][j] = c.orientation[i][0] * r[0][j] + c.orientation[i][1] * r[1][j] + c.orientation[i][2] * r[2][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				c.orientation[i][j] = o[i][j];
	}
}

bool RubikPuzzle::IsSolved() const
{
	for (const Cubie& c : mCubies)
	{
		const int nonZero = (c.home[0] != 0) + (c.home[1] != 0) + (c.home[2] != 0);
		if (nonZero < 2) continue;   // core and centres: they never move, and a centre's spin isn't visible

		for (int j = 0; j < 3; ++j)
			if (c.position[j] != c.home[j]) return false;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				if (c.orientation[i][j] != (i == j ? 1 : 0)) return false;
	}
	return true;
}

XMFLOAT4 RubikPuzzle::OrientationQuaternion(int cubieIndex) const
{
	const Cubie& c = mCubies[cubieIndex];
	XMFLOAT4X4 m(
		float(c.orientation[0][0]), float(c.orientation[0][1]), float(c.orientation[0][2]), 0,
		float(c.orientation[1][0]), float(c.orientation[1][1]), float(c.orientation[1][2]), 0,
		float(c.orientation[2][0]), float(c.orientation[2][1]), float(c.orientation[2][2]), 0,
		0, 0, 0, 1);
	XMFLOAT4 q;
	XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationMatrix(XMLoadFloat4x4(&m))));
	return q;
}

char RubikPuzzle::Letter(Face face)
{
	switch (face)
	{
	case Face::Right: return 'R';
	case Face::Left:  return 'L';
	case Face::Up:    return 'U';
	case Face::Down:  return 'D';
	case Face::Front: return 'F';
	case Face::Back:  return 'B';
	}
	return '?';
}

bool RubikPuzzle::FromLetter(char letter, Face& face)
{
	switch (std::toupper(static_cast<unsigned char>(letter)))
	{
	case 'R': face = Face::Right; return true;
	case 'L': face = Face::Left;  return true;
	case 'U': face = Face::Up;    return true;
	case 'D': face = Face::Down;  return true;
	case 'F': face = Face::Front; return true;
	case 'B': face = Face::Back;  return true;
	default: return false;
	}
}

RubikPuzzle::Face RubikPuzzle::FromNormal(const XMFLOAT3& n)
{
	const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
	if (ax >= ay && ax >= az) return n.x > 0 ? Face::Right : Face::Left;
	if (ay >= az) return n.y > 0 ? Face::Up : Face::Down;
	return n.z > 0 ? Face::Back : Face::Front;
}

bool RubikPuzzle::ParseMoves(const std::string& moves, std::vector<std::pair<Face, bool>>& out)
{
	size_t i = 0;
	while (i < moves.size())
	{
		const char ch = moves[i];
		if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }

		Face face;
		if (!FromLetter(ch, face)) return false;
		++i;

		// Suffixes in either order: ' (counter-clockwise) and a repeat count.
		bool clockwise = true;
		int count = 1;
		for (int k = 0; k < 2 && i < moves.size(); ++k)
		{
			if (moves[i] == '\'') { clockwise = false; ++i; }
			else if (std::isdigit(static_cast<unsigned char>(moves[i]))) { count = moves[i] - '0'; ++i; }
		}
		for (int n = 0; n < count % 4; ++n)
		{
			out.emplace_back(face, clockwise);
		}
	}
	return true;
}

bool RubikPuzzle::ApplyMoves(const std::string& moves)
{
	std::vector<std::pair<Face, bool>> turns;
	const bool ok = ParseMoves(moves, turns);
	for (const auto& [face, clockwise] : turns)
	{
		Turn(face, clockwise);
	}
	return ok;
}

std::string RubikPuzzle::RandomScramble(int moveCount)
{
	static const char letters[] = { 'R', 'L', 'U', 'D', 'F', 'B' };
	std::random_device device;
	std::mt19937 rng(device());
	std::uniform_int_distribution<int> faceDist(0, 5);
	std::uniform_int_distribution<int> kindDist(0, 2);

	std::string scramble;
	int previous = -1;
	for (int i = 0; i < moveCount; ++i)
	{
		int face;
		do { face = faceDist(rng); } while (face == previous);
		previous = face;

		if (!scramble.empty()) scramble += ' ';
		scramble += letters[face];
		switch (kindDist(rng))
		{
		case 1: scramble += '\''; break;
		case 2: scramble += '2'; break;
		default: break;
		}
	}
	return scramble;
}
