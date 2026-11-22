#pragma once
// What ValleyProp components do in a scene: props stand on the valley's ground, only those
// of the current era are shown, and while playing the shown ones block the player (boxes
// baked from their .vox model) and their lights shine.
#include "Valley.h"
#include <Core/Engine.h>
#include <Physics/PhysicsWorld.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace era
{
	constexpr int kFirst = 1, kLast = 6;
	// 1 Camp, 2 Settlement, 3 Boomtown, 4 Industry, 5 Abandoned, 6 Gone.
	const char* Name(int era);
}

class ValleyProps
{
public:
	// Moves every snapToGround prop onto the ground (whole blocks). Only writes positions that
	// change, so an edit scene isn't touched needlessly. Returns how many moved.
	static int SnapToGround(Scene& scene, const Valley& valley);

	// Shows the props of `era` and hides the rest. With `physics` (playing), the shown props
	// get collision and lights of hidden ones go dark; without it (editor) only meshes change,
	// so nothing about lights is written into the scene.
	void ApplyEra(Engine& engine, Scene& scene, int era, PhysicsWorld* physics);
	void Clear(PhysicsWorld* physics);
	// While playing: the shown props' lights at `level` x their placed intensity (lamps are
	// dim by day and full at night).
	void SetLightLevel(Scene& scene, float level);
	// The rooms of the shown buildings (props whose model has windows): for the renderer's
	// indoor lighting. Updated by ApplyEra.
	const std::vector<RoomSettings>& Rooms() const { return mRooms; }

	// Flat pads under every prop that collides and flattens its ground, in scene order; a pad
	// whose centre lies on an earlier one takes that one's height (a house in its plot).
	std::vector<GroundPad> Pads(Engine& engine, Scene& scene, Valley& valley);

private:
	const std::vector<PhysicsBox>* BoxesFor(Engine& engine, const std::string& model);
	// A building's room in model space: from its windows (glass in the front and side walls)
	// back to its rear wall, floor to a little above the windows. None without glass.
	struct ModelRoom { bool valid = false; DirectX::XMFLOAT3 min{}, max{}; };
	const ModelRoom& RoomFor(Engine& engine, const std::string& model);
	void UpdateRooms(Scene& scene);

	std::unordered_map<Entity, std::vector<BodyId>> mBodies;    // collision of shown props
	std::unordered_map<Entity, float> mLightIntensity;           // lights as placed
	int mEra = 1;
	float mLightLevel = 1.0f;
	std::unordered_map<std::string, std::vector<PhysicsBox>> mBoxes;   // per .vox model
	std::unordered_map<std::string, ModelRoom> mModelRooms;             // per .vox model
	std::unordered_map<Entity, ModelRoom> mPropRooms;                   // per prop with a room
	std::vector<RoomSettings> mRooms;
};
