#include "RubikModule.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;

CHERRY_IMPLEMENT_GAME_MODULE(RubikModule)

namespace
{
	constexpr float kSpacing = 2.05f;   // cubies are 2 units wide; the gap shows the seams

	// Sticker cells in rubicPallet.dds (4x4 grid of 128px cells).
	struct Cell { int x, y; };
	constexpr Cell kInnerCell{ 3, 3 };  // black

	// One face of a unit cubie: outward normal, and right/up as seen from outside.
	struct FaceDef
	{
		XMFLOAT3 normal, right, up;
		int axis, sign;
		Cell sticker;
	};

	const FaceDef kFaces[6] =
	{
		{ { 1, 0, 0}, { 0, 0, 1}, {0, 1, 0}, 0, +1, {3, 0} },   // +X orange
		{ {-1, 0, 0}, { 0, 0,-1}, {0, 1, 0}, 0, -1, {0, 0} },   // -X red
		{ { 0, 1, 0}, { 1, 0, 0}, {0, 0, 1}, 1, +1, {0, 1} },   // +Y white
		{ { 0,-1, 0}, { 1, 0, 0}, {0, 0,-1}, 1, -1, {2, 0} },   // -Y yellow
		{ { 0, 0, 1}, {-1, 0, 0}, {0, 1, 0}, 2, +1, {1, 1} },   // +Z blue
		{ { 0, 0,-1}, { 1, 0, 0}, {0, 1, 0}, 2, -1, {1, 0} },   // -Z green
	};

	float SmoothStep(float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	const char* FaceName(RubikPuzzle::Face face)
	{
		switch (face)
		{
		case RubikPuzzle::Face::Right: return "Right (R)";
		case RubikPuzzle::Face::Left:  return "Left (L)";
		case RubikPuzzle::Face::Up:    return "Up (U)";
		case RubikPuzzle::Face::Down:  return "Down (D)";
		case RubikPuzzle::Face::Front: return "Front (F)";
		case RubikPuzzle::Face::Back:  return "Back (B)";
		}
		return "?";
	}
}

EngineConfig RubikModule::GetConfig() const
{
	EngineConfig config;
	config.windowTitle = L"Rubik's Cube - Cherry Engine";
	config.windowWidth = 1600;
	config.windowHeight = 900;
	config.startInEditor = true;
	return config;
}

MeshHandle RubikModule::CreateCubieMesh(Renderer& renderer, const int home[3])
{
	MeshData mesh;
	for (const FaceDef& face : kFaces)
	{
		// Only faces on the outside of the solved cube get a sticker; inner faces are black.
		const Cell cell = (home[face.axis] == face.sign) ? face.sticker : kInnerCell;
		const float u0 = cell.x * 0.25f, v0 = cell.y * 0.25f, u1 = u0 + 0.25f, v1 = v0 + 0.25f;

		const XMVECTOR n = XMLoadFloat3(&face.normal);
		const XMVECTOR r = XMLoadFloat3(&face.right);
		const XMVECTOR u = XMLoadFloat3(&face.up);
		const XMVECTOR corners[4] = { n - r - u, n - r + u, n + r + u, n + r - u };   // clockwise from outside
		const XMFLOAT2 uvs[4] = { {u0, v1}, {u0, v0}, {u1, v0}, {u1, v1} };

		const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
		for (int i = 0; i < 4; ++i)
		{
			Vertex v;
			XMStoreFloat3(&v.position, corners[i]);
			v.normal = face.normal;
			v.uv = uvs[i];
			mesh.vertices.push_back(v);
		}
		mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
	}
	return renderer.CreateMesh(mesh);
}

void RubikModule::OnStartup(Engine& engine)
{
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();

	RenderSettings& settings = renderer.Settings();
	settings.sunDirection = { -0.4f, -0.8f, 0.45f };
	settings.clearColor = { 0.20f, 0.26f, 0.38f, 1.0f };

	mStickerTexture = renderer.LoadTexture(engine.ContentPath(L"Textures/rubicPallet.dds"));

	MaterialDesc material;
	material.albedoTexture = mStickerTexture;
	material.roughness = 0.35f;
	material.fresnelR0 = { 0.05f, 0.05f, 0.05f };
	mMaterial = renderer.CreateMaterial(material);

	material.baseColor = { 0.45f, 0.45f, 0.45f, 1.0f };
	mSelectedMaterial = renderer.CreateMaterial(material);

	const auto& cubies = mPuzzle.Cubies();
	for (int i = 0; i < 27; ++i)
	{
		mCubieMeshes[i] = CreateCubieMesh(renderer, cubies[i].home);
		mCubieEntities[i] = scene.CreateEntity();
		scene.Add(mCubieEntities[i], Transform{});
		scene.Add(mCubieEntities[i], MeshRenderer{ mCubieMeshes[i], mMaterial });
	}

	mOrbit.target = { 0, 0, 0 };
	mOrbit.distance = 17.0f;
	mOrbit.yaw = 0.65f;
	mOrbit.pitch = 0.5f;
	mOrbit.Apply(scene.Get<Transform>(engine.MainCamera()));

	SyncEntities(engine);
}

void RubikModule::OnShutdown(Engine& engine)
{
	Renderer& renderer = engine.GetRenderer();
	Scene& scene = engine.GetScene();
	for (int i = 0; i < 27; ++i)
	{
		scene.DestroyEntity(mCubieEntities[i]);
		renderer.DestroyMesh(mCubieMeshes[i]);
	}
	renderer.DestroyMaterial(mMaterial);
	renderer.DestroyMaterial(mSelectedMaterial);
	renderer.DestroyTexture(mStickerTexture);
}

void RubikModule::QueueMove(Face face, bool clockwise, bool byPlayer)
{
	mQueue.push_back({ face, clockwise, byPlayer ? mTurnDuration : 0.06f, byPlayer });
}

void RubikModule::Scramble(const std::string& moves)
{
	std::vector<std::pair<Face, bool>> turns;
	RubikPuzzle::ParseMoves(moves, turns);
	for (const auto& [face, clockwise] : turns)
	{
		QueueMove(face, clockwise, false);
	}
	mLastScramble = moves;
	mScrambled = true;
	mMoveCount = 0;
	mSolveTime = 0.0f;
}

void RubikModule::ResetPuzzle()
{
	mQueue.clear();
	mTurning = false;
	mTurnTime = 0.0f;
	mPuzzle.Reset();
	mScrambled = false;
	mMoveCount = 0;
	mSolveTime = 0.0f;
}

void RubikModule::HandleInput(Engine& engine)
{
	Input& input = engine.GetInput();
	Scene& scene = engine.GetScene();

	mOrbit.Update(input, scene.Get<Transform>(engine.MainCamera()));

	// Right-click picks the face under the cursor.
	if (input.WasMousePressed(MouseButton::Right))
	{
		mSelected.reset();
		const Ray ray = engine.ScreenPointToRay(input.MousePosition());
		if (auto hit = engine.Raycast(ray))
		{
			if (std::find(mCubieEntities.begin(), mCubieEntities.end(), hit->entity) != mCubieEntities.end())
			{
				mSelected = RubikPuzzle::FromNormal(hit->normal);
			}
		}
	}
	if (input.WasKeyPressed(VK_ESCAPE))
	{
		mSelected.reset();
	}

	if (mSelected)
	{
		if (input.WasKeyPressed('E')) QueueMove(*mSelected, true, true);
		if (input.WasKeyPressed('Q')) QueueMove(*mSelected, false, true);
	}

	// Direct notation keys: R L U D F B (Shift = counter-clockwise).
	const bool shift = input.IsKeyDown(VK_SHIFT);
	for (char letter : { 'R', 'L', 'U', 'D', 'F', 'B' })
	{
		if (input.WasKeyPressed(letter))
		{
			Face face;
			RubikPuzzle::FromLetter(letter, face);
			mSelected = face;
			QueueMove(face, !shift, true);
		}
	}
}

void RubikModule::UpdateTurn(float deltaTime)
{
	if (!mTurning && !mQueue.empty())
	{
		mTurning = true;
		mTurnTime = 0.0f;
	}
	if (!mTurning) return;

	const Move& move = mQueue.front();
	mTurnTime += deltaTime;
	if (mTurnTime < move.duration) return;

	mPuzzle.Turn(move.face, move.clockwise);
	const bool byPlayer = move.byPlayer;
	mQueue.pop_front();
	mTurning = false;

	if (byPlayer)
	{
		++mMoveCount;
		if (mScrambled && mPuzzle.IsSolved())
		{
			mScrambled = false;
			mOpenVictory = true;
		}
	}
}

void RubikModule::SyncEntities(Engine& engine)
{
	Scene& scene = engine.GetScene();
	const Move* move = mTurning ? &mQueue.front() : nullptr;

	XMVECTOR turnRotation = XMQuaternionIdentity();
	if (move)
	{
		const XMFLOAT3 axis = RubikPuzzle::Normal(move->face);
		const float angle = (move->clockwise ? XM_PIDIV2 : -XM_PIDIV2) * SmoothStep(mTurnTime / move->duration);
		turnRotation = XMQuaternionRotationAxis(XMLoadFloat3(&axis), angle);
	}

	const auto& cubies = mPuzzle.Cubies();
	for (int i = 0; i < 27; ++i)
	{
		Transform& transform = scene.Get<Transform>(mCubieEntities[i]);
		XMVECTOR position = XMVectorSet(cubies[i].position[0] * kSpacing, cubies[i].position[1] * kSpacing, cubies[i].position[2] * kSpacing, 0);
		const XMFLOAT4 q = mPuzzle.OrientationQuaternion(i);
		XMVECTOR rotation = XMLoadFloat4(&q);

		if (move && mPuzzle.IsInLayer(i, move->face))
		{
			position = XMVector3Rotate(position, turnRotation);
			rotation = XMQuaternionMultiply(rotation, turnRotation);   // orientation, then the turn
		}
		XMStoreFloat3(&transform.position, position);
		XMStoreFloat4(&transform.rotation, rotation);

		MeshRenderer& renderer = scene.Get<MeshRenderer>(mCubieEntities[i]);
		const bool selected = mSelected && mPuzzle.IsInLayer(i, *mSelected);
		renderer.material = selected ? mSelectedMaterial : mMaterial;
	}

	if (mSelected)
	{
		const XMFLOAT3 n = RubikPuzzle::Normal(*mSelected);
		const float half = 1.5f * kSpacing + 0.05f;
		BoundingBox layer({ n.x * kSpacing, n.y * kSpacing, n.z * kSpacing },
			{ n.x != 0 ? kSpacing * 0.5f + 0.05f : half, n.y != 0 ? kSpacing * 0.5f + 0.05f : half, n.z != 0 ? kSpacing * 0.5f + 0.05f : half });
		engine.GetRenderer().Debug().Box(layer, DebugDraw::Color(1.0f, 0.85f, 0.2f));
	}
}

void RubikModule::OnUpdate(Engine& engine, float deltaTime)
{
	HandleInput(engine);
	UpdateTurn(deltaTime);
	if (mScrambled && mMoveCount > 0)
	{
		mSolveTime += deltaTime;
	}
	SyncEntities(engine);
}

void RubikModule::OnImGui(Engine& /*engine*/)
{
	ImGui::Text("Rubik's Cube");
	ImGui::Separator();
	if (ImGui::Button("Scramble"))
	{
		ResetPuzzle();
		Scramble(RubikPuzzle::RandomScramble(25));
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset"))
	{
		ResetPuzzle();
	}

	ImGui::Text("Moves: %d   Time: %.1fs", mMoveCount, mSolveTime);
	ImGui::Text("State: %s", mPuzzle.IsSolved() ? "solved" : (mScrambled ? "scrambled" : "unsolved"));
	ImGui::Text("Selected: %s", mSelected ? FaceName(*mSelected) : "none (right-click a face)");
	ImGui::SliderFloat("Turn time", &mTurnDuration, 0.05f, 0.6f, "%.2f s");

	ImGui::InputText("##moves", mScrambleInput, sizeof(mScrambleInput));
	ImGui::SameLine();
	if (ImGui::Button("Apply"))
	{
		std::vector<std::pair<Face, bool>> turns;
		if (RubikPuzzle::ParseMoves(mScrambleInput, turns))
		{
			for (const auto& [face, clockwise] : turns) QueueMove(face, clockwise, true);
		}
	}
	if (!mLastScramble.empty())
	{
		ImGui::TextWrapped("Scramble: %s", mLastScramble.c_str());
	}

	if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::BulletText("Left drag: orbit camera, wheel: zoom");
		ImGui::BulletText("Right click: select face");
		ImGui::BulletText("E / Q: turn selected face clockwise / counter-clockwise");
		ImGui::BulletText("R L U D F B: turn that face (Shift = counter-clockwise)");
		ImGui::BulletText("Esc: clear selection");
	}

	if (mOpenVictory)
	{
		ImGui::OpenPopup("Solved!");
		mOpenVictory = false;
	}
	if (ImGui::BeginPopupModal("Solved!", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("You solved the cube in %d moves (%.1f s).", mMoveCount, mSolveTime);
		if (ImGui::Button("Scramble again"))
		{
			ResetPuzzle();
			Scramble(RubikPuzzle::RandomScramble(25));
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Close"))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
