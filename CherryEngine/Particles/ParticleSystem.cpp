#include "Particles/ParticleSystem.h"
#include "Scene/Scene.h"
#include "Renderer/Renderer.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
	float Rand01(uint32_t& state)
	{
		state ^= state << 13; state ^= state >> 17; state ^= state << 5;
		return (state & 0xFFFFFF) / 16777216.0f;
	}
	float RandSigned(uint32_t& state) { return Rand01(state) * 2.0f - 1.0f; }

	XMFLOAT4 Lerp(const XMFLOAT4& a, const XMFLOAT4& b, float t)
	{
		return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
	}
}

Particle ParticleSystem::Spawn(const ParticleEmitter& s, const XMFLOAT3& at, uint32_t& rng)
{
	Particle p;
	p.position = { at.x + RandSigned(rng) * s.spawnBox.x, at.y + RandSigned(rng) * s.spawnBox.y, at.z + RandSigned(rng) * s.spawnBox.z };
	p.velocity = { s.velocity.x + RandSigned(rng) * s.velocityJitter.x, s.velocity.y + RandSigned(rng) * s.velocityJitter.y,
		s.velocity.z + RandSigned(rng) * s.velocityJitter.z };
	p.life = std::max(0.05f, s.lifetime * (1.0f + RandSigned(rng) * s.lifetimeJitter));
	const float sizeJitter = 1.0f + RandSigned(rng) * s.sizeJitter;
	p.size0 = s.sizeStart * sizeJitter;
	p.size1 = s.sizeEnd * sizeJitter;
	p.color0 = s.colorStart;
	p.color1 = s.colorEnd;
	p.gravity = s.gravity;
	p.drag = s.drag;
	p.turbulence = s.turbulence;
	p.emissive = s.emissive;
	p.additive = s.additive;
	p.square = s.square;
	p.stretch = s.stretch;
	p.rotation = s.square ? 0.0f : Rand01(rng) * 6.2831853f;
	return p;
}

void ParticleSystem::Step(std::vector<Particle>& particles, float dt, uint32_t& rng)
{
	size_t alive = 0;
	for (size_t i = 0; i < particles.size(); ++i)
	{
		Particle p = particles[i];
		p.age += dt;
		if (p.age >= p.life) continue;
		p.velocity.y -= p.gravity * dt;
		if (p.turbulence > 0.0f)
		{
			p.velocity.x += RandSigned(rng) * p.turbulence * dt;
			p.velocity.y += RandSigned(rng) * p.turbulence * dt;
			p.velocity.z += RandSigned(rng) * p.turbulence * dt;
		}
		const float keep = std::exp(-p.drag * dt);
		p.velocity = { p.velocity.x * keep, p.velocity.y * keep, p.velocity.z * keep };
		p.position = { p.position.x + p.velocity.x * dt, p.position.y + p.velocity.y * dt, p.position.z + p.velocity.z * dt };
		particles[alive++] = p;
	}
	particles.resize(alive);
}

void ParticleSystem::Update(Scene& scene, float dt)
{
	dt = std::clamp(dt, 0.0f, 0.1f);
	mTime += dt;
	if (scene.IsRegistered<ParticleEmitter>())
	{
		scene.Each<ParticleEmitter>([&](Entity entity, ParticleEmitter& emitter)
		{
			if (emitter.rng == 0) emitter.rng = 0x2545F491u ^ (static_cast<uint32_t>(entity) * 2654435761u);
			Step(emitter.particles, dt, emitter.rng);
			if (!emitter.enabled || emitter.rate <= 0.0f) { emitter.carry = 0.0f; return; }
			const XMMATRIX world = scene.Has<Transform>(entity) ? scene.WorldMatrix(entity) : XMMatrixIdentity();
			XMFLOAT3 at;
			XMStoreFloat3(&at, XMVector3TransformCoord(XMLoadFloat3(&emitter.offset), world));
			emitter.carry += emitter.rate * dt;
			const int limit = std::max(emitter.maxParticles, 1);
			while (emitter.carry >= 1.0f)
			{
				emitter.carry -= 1.0f;
				if (static_cast<int>(emitter.particles.size()) >= limit) continue;
				emitter.particles.push_back(Spawn(emitter, at, emitter.rng));
			}
		});
	}
	Step(mFree, dt, mRng);
}

void ParticleSystem::Burst(const ParticleEmitter& settings, const XMFLOAT3& position, int count, const XMFLOAT3& direction)
{
	constexpr size_t kMaxFree = 8192;
	for (int i = 0; i < count && mFree.size() < kMaxFree; ++i)
	{
		Particle p = Spawn(settings, position, mRng);
		p.velocity = { p.velocity.x + direction.x, p.velocity.y + direction.y, p.velocity.z + direction.z };
		mFree.push_back(p);
	}
}

void ParticleSystem::Clear()
{
	mFree.clear();
}

void ParticleSystem::Submit(Scene& scene, Renderer& renderer) const
{
	std::vector<ParticleInstance>& out = renderer.Particles();
	auto add = [&](const Particle& p)
	{
		const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
		ParticleInstance instance;
		instance.position = p.position;
		instance.size = p.size0 + (p.size1 - p.size0) * t;
		instance.color = Lerp(p.color0, p.color1, t);
		instance.emissive = p.emissive;
		instance.additive = p.additive;
		instance.square = p.square;
		instance.rotation = p.rotation;
		instance.velocity = p.velocity;
		instance.stretch = p.stretch;
		if (instance.color.w > 0.003f && instance.size > 0.0f) out.push_back(instance);
	};
	for (const Particle& p : mFree) add(p);
	if (scene.IsRegistered<ParticleEmitter>())
	{
		scene.Each<ParticleEmitter>([&](Entity, ParticleEmitter& emitter)
		{
			for (const Particle& p : emitter.particles) add(p);
		});
	}
}
