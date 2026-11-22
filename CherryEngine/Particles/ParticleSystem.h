#pragma once
// Particles: small lit billboards (dust, smoke, sparks) simulated on the CPU and drawn by the
// renderer after translucent meshes.
//
// - ParticleEmitter components (Scene/Components.h) spawn them continuously from their
//   entity, in play and in the editor.
// - Game code adds one-off bursts with ParticleSystem::Burst (e.g. dust when rock breaks).
//
// Each particle carries what it needs (sizes, colours, forces), so the emitter that made it
// can change or go away without affecting it.
#include "CherryAPI.h"
#include "Scene/Components.h"
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class Scene;
class Renderer;

class CHERRY_API ParticleSystem
{
public:
	// Advances every enabled emitter of `scene` (spawning by its rate) and every particle.
	void Update(Scene& scene, float deltaTime);
	// Hands this frame's particles (the bursts', and the emitters' of `scene`) to the renderer
	// (Renderer::Particles).
	void Submit(Scene& scene, Renderer& renderer) const;

	// `count` particles at `position` (plus the settings' spawn box), with the settings'
	// look and motion; `direction` (if not zero) is added to their velocity.
	void Burst(const ParticleEmitter& settings, const DirectX::XMFLOAT3& position, int count,
		const DirectX::XMFLOAT3& direction = { 0, 0, 0 });
	// Removes the bursts (emitters keep theirs; they belong to their scene).
	void Clear();

	size_t Count() const { return mFree.size(); }

	// Spawns one particle of `settings` at the world position `at` (helper shared with emitters).
	static Particle Spawn(const ParticleEmitter& settings, const DirectX::XMFLOAT3& at, uint32_t& rng);
	// Moves particles on and drops the dead ones.
	static void Step(std::vector<Particle>& particles, float deltaTime, uint32_t& rng);

private:
	std::vector<Particle> mFree;   // from bursts
	uint32_t mRng = 0x9E3779B9u;
	float mTime = 0.0f;
};
