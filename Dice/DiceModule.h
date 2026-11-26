#pragma once
#include <Game/GameModule.h>
#include <Core/Engine.h>
#include "DiceSim.h"
#include "DiceAudio.h"
#include <RmlUi/Core/DataModelHandle.h>
namespace Rml { class Element; class ElementDocument; }
#include <filesystem>
#include <string>
#include <vector>


// The dice roll feel prototype (PROTOTYPE_EXPORT.md), ported to CherryEngine. The
// simulation is DiceSim (struct of arrays, no entities per die); this module owns the
// view: an orthographic camera, the board, every die drawn as one instanced mesh
// (Renderer::MeshInstances), input and the UI.
class DiceModule : public IGameModule
{
public:
	EngineConfig GetConfig() const override;
	void OnStartup(Engine& engine) override;
	void OnUpdate(Engine& engine, float deltaTime) override;
	void OnImGui(Engine& engine) override;
	void OnShutdown(Engine& engine) override;
	std::string SaveState(Engine& engine) override;
	void LoadState(Engine& engine, const std::string& state) override;

private:
	void BuildBoard(Engine& engine);
	float PixelWorldSize(Engine& engine) const;   // world units one view pixel covers (orthographic)
	void RebuildBoardMeshes(Renderer& renderer);
	void SetupLighting(Renderer& renderer);
	void FitCamera(Engine& engine);
	void WriteInstances(Renderer& renderer);
	void DrawPanel(Engine& engine);
	void ApplyConfig(Engine& engine, const DiceConfig& config);
	std::filesystem::path PresetDir(Engine& engine) const;
	void RefreshPresets(Engine& engine);
	bool LoadPreset(Engine& engine, const std::string& name);
	bool SavePreset(Engine& engine, const std::string& name);
	void TakePopups();
	void StartSession();
	// The HUD (Content/UI/Hud.rml, RmlUi): the fonts, the "dice" data model and the document.
	void CreateHud(Engine& engine);
	// Copies the score, time, FPS, end card and popups into the data model.
	void UpdateHud(Engine& engine);
	struct ImDrawList* SceneOverlay(Engine& engine) const;
	// B7 step 1: where the mouse ray (or the dev pointer) meets the board plane, in the reference's coordinates.
	void PointerOnBoard(Engine& engine, bool& onBoard, double& boardX, double& boardZ);
	void DrawHoverRing(Engine& engine);
	bool ViewProjection(Engine& engine, DirectX::XMMATRIX& viewProj) const;

	dice::DiceSim mSim;
	int mForcedFace = 0;            // dev check: every die dealt with this face up (0 = random)

	TextureHandle mAtlas, mGrid;
	MaterialHandle mDieMaterial, mFeltMaterial, mPlainFeltMaterial, mBaseMaterial, mMarkerMaterial;
	MeshHandle mDieMesh, mFeltMesh, mBaseMesh, mMarkerMesh;
	bool ClassicBoard() const { return mSim.config.layout == Layout::Square && mSim.config.boardShape == BoardShape::Full; }

	float mZoom = 1.0f;

	// Presets (Content/Presets/*.json).
	std::vector<std::string> mPresets;
	bool mPresetsListed = false;
	int mPresetIndex = -1;
	char mPresetName[64] = "";
	std::string mPresetMessage;
	bool mPanelPlaced = false;

	dice::Audio mAudio;

	// What the HUD document shows (bound in CreateHud).
	struct HudView
	{
		std::string score = "0", time = "30.0", startLabel = "Start", fps = "-- fps", total = "0", summary;
		bool ended = false;
	};
	HudView mHud;
	// The popups are the reference's pool: 140 elements in #popups, made and moved from code (a
	// transform given as numbers each frame costs far less than a data binding parsing one).
	// Each popup keeps its element for its life.
	struct PopupSlot
	{
		Rml::Element* element = nullptr;
		double shownAt = -1.0;   // the popup it shows (its start time), -1: hidden
		float opacity = -1.0f;
	};
	std::vector<PopupSlot> mPopupSlots;
	Rml::ElementDocument* mHudDocument = nullptr;   // the document mPopupSlots belong to (a reload makes a new one)
	int mNextPopupSlot = 0;
	void MakePopupPool(Engine& engine);
	Rml::DataModelHandle mHudModel;

	struct ShownPopup
	{
		float x, y, z;
		std::string text;
		uint8_t style;
		double t;
		int slot;   // its element in mPopupSlots
	};
	std::vector<ShownPopup> mPopups;

	// Sparks (B15): a pool of 600 additive points, simulated here, drawn as engine particles.
	static constexpr int kSparks = 600;
	float mSparkPos[kSparks * 3] = {}, mSparkVel[kSparks * 3] = {}, mSparkBase[kSparks * 3] = {}, mSparkLife[kSparks] = {};
	int mSparkNext = 0;
	dice::Random mSparkRand{ 0x5a5a };
	void SpawnSparks(const dice::SparkRequest& request);
	void UpdateSparks(Engine& engine, double dt);
	void ClearSparks();

	// DICE_DEV_POINTER: a pretend mouse for checks without a person.
	bool mDevPointer = false;
	DirectX::XMFLOAT2 mDevFrom{ 0, 0 }, mDevTo{ 0, 0 };
	float mDevSeconds = 1.0f, mDevTime = 0.0f;

	// M10 profiling (DICE_DEV_PROFILE): CPU time of the simulation and of writing the instances,
	// accumulated and logged every 2 s with the frame times and the renderer's stats.
	bool mProfile = false;
	double mProfileTime = 0, mProfileSimMs = 0, mProfileWriteMs = 0, mProfileFrameMaxMs = 0, mProfileGpuMs = 0;
	int mProfileFrames = 0;

	// FPS, updated every 0.5 s (B15 "HUD").
	int mFpsFrames = 0;
	float mFpsTime = 0.0f;
	int mFps = 0;
};
