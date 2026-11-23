#pragma once
// The cast in the valley (Docs/NPCs.md §2.3, §2.4 and §8): a puppet per person, made of their
// part models (Content/Models/Npcs/<id>/), posed every frame from where NpcTown says they are
// on the outside clock. Close by they turn their heads to you and block your way while they
// stand still; while outside time races they fade into faint, fast figures with echoes (a
// long-exposure photograph), and past a point they're gone.
#include "NpcTown.h"
#include "NpcLog.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <vector>

class NpcWorld
{
public:
	// After the valley and its props stand (their positions are where the places are).
	void Create(Engine& engine, Scene& scene, const Valley& valley);
	void Destroy(Engine& engine, Scene& scene, PhysicsWorld* physics);

	struct View
	{
		double days = 0.0;               // the outside clock
		int era = 1;                     // the era the valley shows
		float timeRate = 0.0f;           // outside seconds per real second where the player is
		DirectX::XMFLOAT3 eye{};         // the camera
		DirectX::XMFLOAT3 feet{};        // the player
		bool hidden = false;             // deep in the mountain: the valley isn't drawn
		const npc::PlayerLog* log = nullptr;   // story flags that change lives
	};
	void Update(Engine& engine, Scene& scene, PhysicsWorld& physics, const View& view, float dt);

	// The Game panel's "People" section, with what each thinks of you (`log`; its debug buttons
	// change their friendship). Returns true with `goTo` (feet) and `goYaw` (facing them) set
	// when a "Go to" button was pressed.
	bool DebugGui(Engine& engine, const View& view, npc::PlayerLog& log, DirectX::XMFLOAT3& goTo, float& goYaw);

	// Who you could talk to: the nearest person drawn in full (not a time-lapse figure) within
	// cfg::kTalkDistance whose head is roughly where you look. -1 if none.
	int TalkTarget(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& look) const;
	// While talking (-1: nobody), they stop and turn to you (people sitting just turn their heads).
	void SetTalking(int npc) { mTalking = npc; }
	int Talking() const { return mTalking; }
	// Someone drawn in full right now (for barks and talking).
	bool Shown(int npc) const;
	const npc::Pose& PoseOf(int npc) const { return mPeople[npc].pose; }
	DirectX::XMFLOAT3 HeadPosition(int npc) const;   // world, the middle of the head
	// For their portrait in the conversation (Renderer::RenderPreview).
	MeshHandle HeadMesh(int npc) const { return mPeople[npc].parts.head; }
	MaterialHandle Material() const { return mMaterial; }

	const npc::Town& Town() const { return mTown; }

private:
	// A person's part meshes and the sizes the joints are placed from (metres, unscaled).
	struct Parts
	{
		MeshHandle head, torso, arm, leg;
		float headH = 0.0f, torsoH = 0.0f, torsoW = 0.0f, torsoD = 0.0f, armW = 0.0f, legH = 0.0f, legW = 0.0f;
		bool valid = false;
	};
	struct Puppet
	{
		Entity root = INVALID_ENTITY, torso = INVALID_ENTITY, head = INVALID_ENTITY;
		Entity arm[2] = { INVALID_ENTITY, INVALID_ENTITY }, leg[2] = { INVALID_ENTITY, INVALID_ENTITY };
	};
	struct Person
	{
		Parts parts;
		std::vector<Puppet> copies;      // [0] the person, then their time-lapse echoes
		npc::Pose pose;
		float shownY = 0.0f;             // the feet's height, easing over the terrain's steps
		bool placed = false;
		float headYaw = 0.0f;            // turned towards you, relative to the body
		float seed = 0.0f;               // varies idle motion between people
		BodyId blocker = kInvalidBody;
		DirectX::XMFLOAT3 blockerAt{};
		float distance = 0.0f;           // from the camera
		bool shown = false;              // drawn this frame
		float scale = 1.0f;              // their height, smaller as children
	};

	Parts LoadParts(Engine& engine, const npc::NpcDef& npc);
	Puppet MakePuppet(Scene& scene, const Parts& parts, MaterialHandle material);
	void PosePuppet(Scene& scene, const Puppet& puppet, const Person& person, const npc::Pose& pose, float y, float headYaw, float dither, bool shadows) const;
	static void SetVisible(Scene& scene, const Puppet& puppet, bool visible);
	void UpdateBlocker(PhysicsWorld& physics, Person& person, const View& view, bool wanted);

	npc::Town mTown;
	std::vector<Person> mPeople;         // in Cast() order
	Entity mGroup = INVALID_ENTITY;      // every puppet hangs under this (one folded row in the hierarchy)
	MaterialHandle mMaterial;
	int mTalking = -1;
	float mMainDither = 1.0f;            // this frame's (time-lapse fades it)
	float mTime = 0.0f;
	bool mShowRoutes = false;
};
