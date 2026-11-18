#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include <Core/CameraControllers.h>
#include "RubikPuzzle.h"
#include <array>
#include <deque>
#include <optional>

// Placed in a scene: where the puzzle goes and how it plays. Without one, OnStartup
// puts a cube at the origin.
struct RubikCube
{
	float turnTime = 0.18f;        // seconds per player turn
	float cameraDistance = 17.0f;  // orbit camera distance from the cube
};

// The Rubik's cube game, built on CherryEngine as a separate module.
class RubikModule : public IGameModule
{
public:
	EngineConfig GetConfig() const override;
	void RegisterTypes(GameTypes& types) override;
	void OnStartup(Engine& engine) override;
	void OnUpdate(Engine& engine, float deltaTime) override;
	void OnImGui(Engine& engine) override;
	void OnShutdown(Engine& engine) override;
	std::string SaveState(Engine& engine) override;
	void LoadState(Engine& engine, const std::string& state) override;

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
	Entity mCubeEntity = INVALID_ENTITY;      // the RubikCube entity: cubies are its children
	bool mOwnCubeEntity = false;              // made by OnStartup (no RubikCube in the scene)
	std::string mHistory;                     // every turn applied since the last reset ("R U' ...")
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
