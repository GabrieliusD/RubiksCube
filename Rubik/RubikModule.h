#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include <Core/CameraControllers.h>
#include "RubikPuzzle.h"
#include <array>
#include <deque>
#include <optional>

// The Rubik's cube game, built on CherryEngine as a separate module.
class RubikModule : public IGameModule
{
public:
	EngineConfig GetConfig() const override;
	void OnStartup(Engine& engine) override;
	void OnUpdate(Engine& engine, float deltaTime) override;
	void OnImGui(Engine& engine) override;
	void OnShutdown(Engine& engine) override;

private:
	using Face = RubikPuzzle::Face;

	struct Move
	{
		Face face;
		bool clockwise;
		float duration;
		bool byPlayer;
	};

	MeshHandle CreateCubieMesh(Renderer& renderer, const int home[3]);
	void HandleInput(Engine& engine);
	void QueueMove(Face face, bool clockwise, bool byPlayer);
	void Scramble(const std::string& moves);
	void ResetPuzzle();
	void UpdateTurn(float deltaTime);
	void SyncEntities(Engine& engine);

	RubikPuzzle mPuzzle;
	std::array<Entity, 27> mCubieEntities{};
	std::array<MeshHandle, 27> mCubieMeshes{};
	TextureHandle mStickerTexture;
	MaterialHandle mMaterial;
	MaterialHandle mSelectedMaterial;
	OrbitCameraController mOrbit;

	std::deque<Move> mQueue;
	bool mTurning = false;
	float mTurnTime = 0.0f;
	std::optional<Face> mSelected;

	float mTurnDuration = 0.18f;
	int mMoveCount = 0;
	float mSolveTime = 0.0f;
	bool mScrambled = false;
	bool mOpenVictory = false;
	std::string mLastScramble;
	char mScrambleInput[128] = "R U R' U'";
};
