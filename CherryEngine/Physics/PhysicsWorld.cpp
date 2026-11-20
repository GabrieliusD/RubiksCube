#include "Physics/PhysicsWorld.h"
#include "Renderer/DebugDraw.h"
#include "Core/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

using namespace DirectX;

namespace
{
	// Object layers: the static world, everything that moves, debris (moves, but passes
	// through characters) and characters' inner bodies.
	namespace Layers
	{
		constexpr JPH::ObjectLayer kStatic = 0;
		constexpr JPH::ObjectLayer kMoving = 1;
		constexpr JPH::ObjectLayer kDebris = 2;
		constexpr JPH::ObjectLayer kCharacter = 3;
		constexpr JPH::uint kCount = 4;
	}
	namespace BroadLayers
	{
		const JPH::BroadPhaseLayer kStatic(0);
		const JPH::BroadPhaseLayer kMoving(1);
		constexpr JPH::uint kCount = 2;
	}

	class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
		{
			if (a == Layers::kStatic && b == Layers::kStatic) return false;
			const bool debrisAndCharacter = (a == Layers::kDebris && b == Layers::kCharacter) || (a == Layers::kCharacter && b == Layers::kDebris);
			return !debrisAndCharacter;
		}
	};

	class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
	{
	public:
		JPH::uint GetNumBroadPhaseLayers() const override { return BroadLayers::kCount; }
		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
		{
			return layer == Layers::kStatic ? BroadLayers::kStatic : BroadLayers::kMoving;
		}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
		{
			return layer == BroadLayers::kStatic ? "Static" : "Moving";
		}
#endif
	};

	class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override
		{
			return layer != Layers::kStatic || broad == BroadLayers::kMoving;
		}
	};

	// Raycasts with dynamicOnly: every layer but the static world.
	class NotStaticFilter final : public JPH::ObjectLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer != Layers::kStatic; }
	};

	JPH::Vec3 ToJolt(const XMFLOAT3& v) { return JPH::Vec3(v.x, v.y, v.z); }
	JPH::Quat ToJolt(const XMFLOAT4& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
	XMFLOAT3 FromJolt(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }
	XMFLOAT4 FromJolt(JPH::QuatArg q) { return { q.GetX(), q.GetY(), q.GetZ(), q.GetW() }; }

	constexpr float kFixedStep = 1.0f / 60.0f;
	constexpr int kMaxStepsPerFrame = 4;

	int gJoltUsers = 0;   // Jolt's factory/allocator are global; set up once
}

struct PhysicsWorld::Impl
{
	std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
	std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
	BroadPhaseLayers broadPhaseLayers;
	ObjectVsBroadPhaseFilter objectVsBroadPhase;
	ObjectPairFilter objectPairs;
	std::unique_ptr<JPH::PhysicsSystem> system;
	bool initialized = false;

	float accumulator = 0.0f;
	float gravity = 20.0f;
	PhysicsStats stats;

	// Dynamic bodies: transforms at the last two fixed steps, for interpolation.
	struct Snapshot
	{
		JPH::RVec3 previousPosition, currentPosition;
		JPH::Quat previousRotation, currentRotation;
	};
	std::unordered_map<uint32_t, Snapshot> dynamicBodies;
	std::unordered_set<uint32_t> allBodies;

	JPH::BodyInterface& Bodies() { return system->GetBodyInterface(); }
	static JPH::BodyID Id(BodyId id) { return JPH::BodyID(id); }

	void TakeSnapshot(bool shiftPrevious)
	{
		JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
		for (auto& [raw, snap] : dynamicBodies)
		{
			if (shiftPrevious)
			{
				snap.previousPosition = snap.currentPosition;
				snap.previousRotation = snap.currentRotation;
			}
			bodies.GetPositionAndRotation(Id(raw), snap.currentPosition, snap.currentRotation);
		}
	}

	BodyId AddBody(const JPH::BodyCreationSettings& settings, JPH::EActivation activation)
	{
		const JPH::BodyID id = Bodies().CreateAndAddBody(settings, activation);
		if (id.IsInvalid())
		{
			CHERRY_ERROR("Physics: body limit reached");
			return kInvalidBody;
		}
		allBodies.insert(id.GetIndexAndSequenceNumber());
		return id.GetIndexAndSequenceNumber();
	}
};

PhysicsWorld::PhysicsWorld() : mImpl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld()
{
	Shutdown();
}

void PhysicsWorld::Initialize()
{
	Impl& p = *mImpl;
	if (p.initialized) return;

	if (gJoltUsers++ == 0)
	{
		JPH::RegisterDefaultAllocator();
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();
	}

	p.tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);
	const int workers = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
	p.jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);

	p.system = std::make_unique<JPH::PhysicsSystem>();
	p.system->Init(16384, 0, 16384, 16384, p.broadPhaseLayers, p.objectVsBroadPhase, p.objectPairs);
	p.system->SetGravity(JPH::Vec3(0.0f, -p.gravity, 0.0f));
	p.initialized = true;
	CHERRY_LOG("Physics: Jolt %d.%d.%d, %d worker threads", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH, workers);
}

void PhysicsWorld::Shutdown()
{
	Impl& p = *mImpl;
	if (!p.initialized) return;

	JPH::BodyInterface& bodies = p.Bodies();
	for (uint32_t raw : p.allBodies)
	{
		bodies.RemoveBody(Impl::Id(raw));
		bodies.DestroyBody(Impl::Id(raw));
	}
	p.allBodies.clear();
	p.dynamicBodies.clear();
	p.system.reset();
	p.jobSystem.reset();
	p.tempAllocator.reset();
	p.initialized = false;

	if (--gJoltUsers == 0)
	{
		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}
}

void PhysicsWorld::Update(float dt)
{
	Impl& p = *mImpl;
	if (!p.initialized) return;

	const auto start = std::chrono::steady_clock::now();
	p.accumulator += dt;
	int steps = 0;
	while (p.accumulator >= kFixedStep && steps < kMaxStepsPerFrame)
	{
		p.system->Update(kFixedStep, 1, p.tempAllocator.get(), p.jobSystem.get());
		p.TakeSnapshot(true);
		p.accumulator -= kFixedStep;
		++steps;
	}
	if (steps == kMaxStepsPerFrame)
	{
		p.accumulator = std::min(p.accumulator, kFixedStep);   // don't spiral after a hitch
	}

	p.stats.stepsLastFrame = steps;
	p.stats.stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	p.stats.bodies = static_cast<uint32_t>(p.allBodies.size());
	p.stats.activeBodies = p.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

BodyId PhysicsWorld::CreateStaticMesh(const std::vector<XMFLOAT3>& vertices)
{
	Impl& p = *mImpl;
	if (vertices.size() < 3) return kInvalidBody;

	JPH::TriangleList triangles;
	triangles.reserve(vertices.size() / 3);
	for (size_t i = 0; i + 2 < vertices.size(); i += 3)
	{
		// Jolt treats counter-clockwise (right-handed cross product outward) as the front face,
		// which is exactly the (b - a) x (c - a) convention of our input.
		triangles.push_back(JPH::Triangle(
			JPH::Float3(vertices[i].x, vertices[i].y, vertices[i].z),
			JPH::Float3(vertices[i + 1].x, vertices[i + 1].y, vertices[i + 1].z),
			JPH::Float3(vertices[i + 2].x, vertices[i + 2].y, vertices[i + 2].z)));
	}
	JPH::MeshShapeSettings meshSettings(triangles);
	JPH::ShapeSettings::ShapeResult result = meshSettings.Create();
	if (result.HasError())
	{
		CHERRY_ERROR("Physics: mesh shape failed: %s", result.GetError().c_str());
		return kInvalidBody;
	}
	JPH::BodyCreationSettings settings(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::kStatic);
	return p.AddBody(settings, JPH::EActivation::DontActivate);
}

BodyId PhysicsWorld::CreateStaticBox(const XMFLOAT3& center, const XMFLOAT3& halfExtents)
{
	Impl& p = *mImpl;
	JPH::BodyCreationSettings settings(new JPH::BoxShape(ToJolt(halfExtents)), JPH::RVec3(center.x, center.y, center.z),
		JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::kStatic);
	return p.AddBody(settings, JPH::EActivation::DontActivate);
}

BodyId PhysicsWorld::CreateDynamicBoxes(const std::vector<PhysicsBox>& boxes, float density,
	const XMFLOAT3& position, const XMFLOAT4& rotation, const XMFLOAT3& linearVelocity, const XMFLOAT3& angularVelocity, bool ignoreCharacters)
{
	Impl& p = *mImpl;
	if (boxes.empty()) return kInvalidBody;

	JPH::StaticCompoundShapeSettings compound;
	for (const PhysicsBox& box : boxes)
	{
		const float smallest = std::min({ box.halfExtents.x, box.halfExtents.y, box.halfExtents.z });
		auto* shape = new JPH::BoxShapeSettings(ToJolt(box.halfExtents), std::min(0.02f, smallest * 0.5f));
		shape->SetDensity(density);
		compound.AddShape(ToJolt(box.center), JPH::Quat::sIdentity(), shape);
	}
	JPH::ShapeSettings::ShapeResult result = compound.Create();
	if (result.HasError())
	{
		CHERRY_ERROR("Physics: compound shape failed: %s", result.GetError().c_str());
		return kInvalidBody;
	}

	JPH::BodyCreationSettings settings(result.Get(), JPH::RVec3(position.x, position.y, position.z), ToJolt(rotation),
		JPH::EMotionType::Dynamic, ignoreCharacters ? Layers::kDebris : Layers::kMoving);
	settings.mLinearVelocity = ToJolt(linearVelocity);
	settings.mAngularVelocity = ToJolt(angularVelocity);
	settings.mFriction = 0.7f;
	settings.mRestitution = 0.1f;
	settings.mLinearDamping = 0.05f;
	settings.mAngularDamping = 0.1f;
	settings.mMotionQuality = JPH::EMotionQuality::LinearCast;   // small fast chunks shouldn't tunnel

	const BodyId id = p.AddBody(settings, JPH::EActivation::Activate);
	if (id != kInvalidBody)
	{
		Impl::Snapshot& snap = p.dynamicBodies[id];
		p.Bodies().GetPositionAndRotation(Impl::Id(id), snap.currentPosition, snap.currentRotation);
		snap.previousPosition = snap.currentPosition;
		snap.previousRotation = snap.currentRotation;
	}
	return id;
}

void PhysicsWorld::RemoveBody(BodyId body)
{
	Impl& p = *mImpl;
	if (body == kInvalidBody || p.allBodies.erase(body) == 0) return;
	p.dynamicBodies.erase(body);
	p.Bodies().RemoveBody(Impl::Id(body));
	p.Bodies().DestroyBody(Impl::Id(body));
}

bool PhysicsWorld::IsValid(BodyId body) const
{
	return body != kInvalidBody && mImpl->allBodies.count(body) > 0;
}

bool PhysicsWorld::IsSleeping(BodyId body) const
{
	if (!IsValid(body)) return true;
	return !mImpl->Bodies().IsActive(Impl::Id(body));
}

void PhysicsWorld::GetTransform(BodyId body, XMFLOAT3& position, XMFLOAT4& rotation, bool interpolated) const
{
	const Impl& p = *mImpl;
	auto it = p.dynamicBodies.find(body);
	if (it != p.dynamicBodies.end())
	{
		const Impl::Snapshot& s = it->second;
		const float alpha = interpolated ? std::clamp(p.accumulator / kFixedStep, 0.0f, 1.0f) : 1.0f;
		const JPH::RVec3 pos = s.previousPosition + (s.currentPosition - s.previousPosition) * alpha;
		const JPH::Quat rot = s.previousRotation.SLERP(s.currentRotation, alpha).Normalized();
		position = FromJolt(JPH::Vec3(pos));
		rotation = FromJolt(rot);
		return;
	}
	if (!IsValid(body))
	{
		position = { 0, 0, 0 };
		rotation = { 0, 0, 0, 1 };
		return;
	}
	JPH::RVec3 pos;
	JPH::Quat rot;
	mImpl->Bodies().GetPositionAndRotation(Impl::Id(body), pos, rot);
	position = FromJolt(JPH::Vec3(pos));
	rotation = FromJolt(rot);
}

XMFLOAT3 PhysicsWorld::GetLinearVelocity(BodyId body) const
{
	if (!IsValid(body)) return { 0, 0, 0 };
	return FromJolt(mImpl->Bodies().GetLinearVelocity(Impl::Id(body)));
}

void PhysicsWorld::AddImpulse(BodyId body, const XMFLOAT3& impulse, const XMFLOAT3& worldPoint)
{
	if (!IsValid(body)) return;
	mImpl->Bodies().AddImpulse(Impl::Id(body), ToJolt(impulse), JPH::RVec3(worldPoint.x, worldPoint.y, worldPoint.z));
}

std::optional<PhysicsRayHit> PhysicsWorld::Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance, bool dynamicOnly) const
{
	const Impl& p = *mImpl;
	if (!p.initialized) return std::nullopt;

	const JPH::RRayCast ray{ JPH::RVec3(origin.x, origin.y, origin.z), ToJolt(direction) * maxDistance };
	JPH::RayCastResult result;
	const JPH::SpecifiedBroadPhaseLayerFilter movingBroad(BroadLayers::kMoving);
	const NotStaticFilter movingObjects;
	const JPH::BroadPhaseLayerFilter anyBroad;
	const JPH::ObjectLayerFilter anyObject;
	const bool hit = p.system->GetNarrowPhaseQuery().CastRay(ray, result,
		dynamicOnly ? static_cast<const JPH::BroadPhaseLayerFilter&>(movingBroad) : anyBroad,
		dynamicOnly ? static_cast<const JPH::ObjectLayerFilter&>(movingObjects) : anyObject);
	if (!hit) return std::nullopt;

	PhysicsRayHit out;
	out.body = result.mBodyID.GetIndexAndSequenceNumber();
	out.distance = result.mFraction * maxDistance;
	const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
	out.point = FromJolt(JPH::Vec3(point));
	JPH::BodyLockRead lock(p.system->GetBodyLockInterface(), result.mBodyID);
	if (lock.Succeeded())
	{
		const JPH::Body& body = lock.GetBody();
		out.normal = FromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
		out.dynamic = body.IsDynamic();
	}
	return out;
}

void PhysicsWorld::SetGravity(float g)
{
	mImpl->gravity = g;
	if (mImpl->system) mImpl->system->SetGravity(JPH::Vec3(0.0f, -g, 0.0f));
}

float PhysicsWorld::Gravity() const { return mImpl->gravity; }
const PhysicsStats& PhysicsWorld::Stats() const { return mImpl->stats; }

void PhysicsWorld::DrawBounds(DebugDraw& debug) const
{
	const Impl& p = *mImpl;
	if (!p.initialized) return;
	const JPH::BodyInterface& bodies = p.system->GetBodyInterfaceNoLock();
	for (uint32_t raw : p.allBodies)
	{
		const JPH::AABox box = bodies.GetTransformedShape(Impl::Id(raw)).GetWorldSpaceBounds();
		const JPH::Vec3 c = box.GetCenter(), e = box.GetExtent();
		const bool dynamic = p.dynamicBodies.count(raw) > 0;
		const uint32_t color = dynamic
			? (bodies.IsActive(Impl::Id(raw)) ? DebugDraw::Color(1.0f, 0.6f, 0.1f) : DebugDraw::Color(0.4f, 0.6f, 1.0f))
			: DebugDraw::Color(0.3f, 0.9f, 0.4f);
		debug.Box(BoundingBox(FromJolt(c), FromJolt(e)), color);
	}
}

// =============================================================================
// PhysicsCharacter
// =============================================================================

struct PhysicsCharacter::Impl
{
	PhysicsWorld::Impl* world = nullptr;
	JPH::Ref<JPH::CharacterVirtual> character;
	Settings settings;
};

PhysicsCharacter::PhysicsCharacter(PhysicsWorld& world, const Settings& settings, const XMFLOAT3& feet)
	: mImpl(std::make_unique<Impl>())
{
	mImpl->world = &world.GetImpl();
	mImpl->settings = settings;

	// Capsule standing on the origin (feet at y = 0).
	const float halfCylinder = std::max(0.01f, settings.height * 0.5f - settings.radius);
	JPH::RefConst<JPH::Shape> shape = JPH::RotatedTranslatedShapeSettings(
		JPH::Vec3(0, settings.height * 0.5f, 0), JPH::Quat::sIdentity(),
		new JPH::CapsuleShape(halfCylinder, settings.radius)).Create().Get();

	JPH::CharacterVirtualSettings cs;
	cs.mShape = shape;
	cs.mInnerBodyShape = shape;                 // a real body so falling rock hits the player
	cs.mInnerBodyLayer = Layers::kCharacter;
	cs.mMaxSlopeAngle = JPH::DegreesToRadians(settings.maxSlopeDegrees);
	cs.mMass = settings.mass;
	cs.mMaxStrength = settings.pushStrength;
	cs.mUp = JPH::Vec3::sAxisY();
	cs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -settings.radius);   // contacts below the lower hemisphere support us
	cs.mCharacterPadding = 0.02f;
	cs.mPredictiveContactDistance = 0.1f;
	cs.mEnhancedInternalEdgeRemoval = true;     // smooth walking over voxel seams

	mImpl->character = new JPH::CharacterVirtual(&cs, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(), 0, mImpl->world->system.get());
}

PhysicsCharacter::~PhysicsCharacter() = default;

void PhysicsCharacter::Update(float dt, const XMFLOAT3& horizontal, bool jump, float jumpSpeed)
{
	Impl& c = *mImpl;
	JPH::CharacterVirtual& ch = *c.character;
	const JPH::Vec3 gravity(0.0f, -c.world->gravity, 0.0f);

	ch.UpdateGroundVelocity();
	const JPH::Vec3 groundVelocity = ch.GetGroundVelocity();
	const JPH::Vec3 vertical(0.0f, ch.GetLinearVelocity().GetY(), 0.0f);

	JPH::Vec3 velocity;
	const bool onGround = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
	if (onGround && (vertical.GetY() - groundVelocity.GetY()) < 0.1f)
	{
		velocity = groundVelocity;   // ride moving rock
		if (jump) velocity += JPH::Vec3(0.0f, jumpSpeed, 0.0f);
	}
	else
	{
		velocity = vertical;
	}
	velocity += gravity * dt;
	velocity += JPH::Vec3(horizontal.x, 0.0f, horizontal.z);
	ch.SetLinearVelocity(velocity);

	JPH::CharacterVirtual::ExtendedUpdateSettings update;
	update.mWalkStairsStepUp = JPH::Vec3(0.0f, c.settings.stepHeight, 0.0f);
	update.mStickToFloorStepDown = JPH::Vec3(0.0f, -c.settings.stepHeight, 0.0f);
	ch.ExtendedUpdate(dt, gravity, update,
		c.world->system->GetDefaultBroadPhaseLayerFilter(Layers::kCharacter),
		c.world->system->GetDefaultLayerFilter(Layers::kCharacter),
		{}, {}, *c.world->tempAllocator);
}

XMFLOAT3 PhysicsCharacter::Position() const
{
	return FromJolt(JPH::Vec3(mImpl->character->GetPosition()));
}

XMFLOAT3 PhysicsCharacter::Velocity() const
{
	return FromJolt(mImpl->character->GetLinearVelocity());
}

bool PhysicsCharacter::IsGrounded() const
{
	return mImpl->character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

void PhysicsCharacter::SetPosition(const XMFLOAT3& feet)
{
	mImpl->character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
	mImpl->character->SetLinearVelocity(JPH::Vec3::sZero());
}
