#pragma once
// Editor tools for the people (Docs/NPCs.md §10, milestone 5):
//   People inspector: pick someone and a moment (any year, hour, era): their life then (age,
//     role, family), where they are and what they're doing, their whole day with gatherings
//     spliced in, the events of their life, the missions and letters they give; drawn in the
//     viewport too.
//   Dialog view: everything someone can say, as trees with each line's conditions, which
//     greeting, barks and topics a situation picks (you set the friendship, how long you've
//     been away, the hour, the era, flags...), and a conversation to try out there and then.
// Drawn in the editor (NpcToolsSystem: two dockable windows, the town built from the edit
// scene) and inside the Game panel while playing (with the live clock and your log).
#include "NpcTown.h"
#include "Dialog.h"
#include "Missions.h"
#include "Letters.h"
#include "Valley.h"
#include "ValleyProps.h"
#include <Core/Engine.h>
#include <Game/GameModule.h>
#include <memory>

class NpcTools
{
public:
	// What the tools look at.
	struct Data
	{
		const npc::Town* town = nullptr;
		const npc::DialogLibrary* dialogs = nullptr;
		const npc::MissionBook* missions = nullptr;
		const npc::PlayerLog* log = nullptr;   // your log while playing; none in the editor
		double now = -1.0;                     // the clock while playing (-1: none)
		bool playing = false;
	};
	// What the inspector asks the game to do (while playing).
	struct Request
	{
		bool setClock = false;
		double days = 0.0;
		bool goTo = false;
		DirectX::XMFLOAT3 feet{};
		float yaw = 0.0f;
		bool reload = false;                   // read the dialog and mission files again
	};

	void DrawInspector(const Data& data, DebugDraw* debug, Request& request);
	void DrawDialogView(const Data& data, Request& request);

private:
	void DrawLife(const Data& data, int who, double days, int era);
	void DrawDay(const Data& data, int who, double days, int era, DebugDraw* debug);
	void DrawLines(const char* label, const std::vector<npc::Line>& lines, const npc::DialogContext& context, bool pickOne);
	void DrawSteps(const std::vector<npc::Step>& steps, const npc::DialogContext& context, int depth);
	void TryConversation(const Data& data);
	npc::DialogContext SandboxContext() const;
	void ResetSandbox();

	// Inspector
	int mWho = 0;
	float mYears = 0.5f, mHour = 10.0f;
	int mEraOverride = 0;           // 0: the era of that year
	bool mFollowClock = true;       // while playing: the inspector shows now
	bool mDraw = true;              // in the viewport

	// Dialog view
	int mTalker = 0;
	int mTier = static_cast<int>(npc::Tier::Acquaintance);
	bool mMet = true, mRain = false, mDejaVu = false, mHasEverything = true;
	float mAway = 1.0f, mSandboxHour = 12.0f, mSandboxYears = 0.5f;
	char mFlags[256] = "";          // flags set, separated by spaces
	npc::PlayerLog mSandboxLog;
	npc::Conversation mConversation;
	bool mTalking = false;
	bool mChoosingGift = false;
};

// The editor's windows: the tools, with a town of their own built from the edit scene.
class NpcToolsSystem : public ISystem
{
public:
	NpcToolsSystem();
	~NpcToolsSystem() override;
	bool RunsInEditMode() const override { return true; }
	void OnEditUpdate(Engine& engine, float deltaTime) override;
	void OnUnload(Engine& engine) override;

private:
	struct State;
	std::unique_ptr<State> mState;
};
