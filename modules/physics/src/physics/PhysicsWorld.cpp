/**
 * @file PhysicsWorld.cpp
 * @author Jackey Cen
 * @brief The Jolt side of PhysicsWorld: bodies, queries and the step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>

#include "CollisionLayers.h"
#include "ContactTracker.h"
#include "HeightFieldData.h"
#include "JoltConversions.h"
#include "JoltRuntime.h"

#include <core/log/Log.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    using detail::FromJolt;
    using detail::PositionFromJolt;
    using detail::ToBodyId;
    using detail::ToJolt;

    namespace
    {
        // Covers resting contact plus Jolt's 2 cm speculative distance.
        constexpr float kWakeMargin = 0.1f;

        JPH::RVec3 OffsetToWorldPoint(const JPH::BodyInterface &bodies, JPH::BodyID id, const glm::vec3 &localOffset)
        {
            const JPH::Vec3 worldOffset = bodies.GetRotation(id) * ToJolt(localOffset);
            return bodies.GetCenterOfMassPosition(id) + worldOffset;
        }

        // Null, logged, for a size Jolt cannot build. A shape other than Sphere is a box.
        const JPH::Shape *MakeShape(const BodyDesc &desc)
        {
            if (desc.mShape == PhysicsShape::Sphere)
            {
                if (desc.mRadius <= 0.0f)
                {
                    MIR_LOG_ERROR("PhysicsWorld::CreateBody: radius must be positive");
                    return nullptr;
                }
                return new JPH::SphereShape(desc.mRadius);
            }

            const glm::vec3 &halfExtent = desc.mHalfExtent;
            if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f || halfExtent.z <= 0.0f)
            {
                MIR_LOG_ERROR("PhysicsWorld::CreateBody: half extent must be positive");
                return nullptr;
            }
            return new JPH::BoxShape(ToJolt(halfExtent));
        }
    }

    struct PhysicsWorld::Impl
    {
        explicit Impl(const PhysicsWorldDesc &desc)
            : mJobSystem(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs), static_cast<JPH::uint>(JPH::cMaxPhysicsBarriers),
                         desc.mWorkerThreads > 0 ? static_cast<int>(desc.mWorkerThreads) : -1),
              mCollisionSteps(desc.mCollisionSteps > 0 ? desc.mCollisionSteps : 1)
        {
            mSystem.Init(desc.mMaxBodies,
                         0, // body mutexes: 0 lets Jolt pick a default
                         desc.mMaxBodyPairs, desc.mMaxContactConstraints, mBroadPhaseLayers, mObjectVsBroadPhase,
                         mObjectLayerPairs);

            mSystem.SetGravity(ToJolt(desc.mGravity));
            mSystem.SetContactListener(&mContacts);
        }

        JPH::BodyInterface &Bodies()
        {
            return mSystem.GetBodyInterface();
        }

        const JPH::BodyInterface &Bodies() const
        {
            return mSystem.GetBodyInterface();
        }

        // Jolt answers a stale id with defaults, the origin among them, which here is a real place.
        std::optional<JPH::BodyID> Live(BodyHandle body) const
        {
            if (!body.IsValid())
                return std::nullopt;
            const JPH::BodyID id = ToBodyId(body);
            if (!Bodies().IsAdded(id))
                return std::nullopt;
            return id;
        }

        BodyHandle CreateBody(const JPH::Shape *shape, WorldPosition position, const glm::quat &rotation,
                              PhysicsMotion motion, bool enhancedInternalEdgeRemoval = false)
        {
            const JPH::RefConst<JPH::Shape> owned(shape);
            const bool dynamic = motion == PhysicsMotion::Dynamic;

            JPH::BodyCreationSettings settings(owned, ToJolt(position), ToJolt(rotation),
                                               dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                               dynamic ? detail::Layers::kMoving : detail::Layers::kNonMoving);
            settings.mEnhancedInternalEdgeRemoval = enhancedInternalEdgeRemoval;

            const JPH::BodyID id = Bodies().CreateAndAddBody(
                settings, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

            if (id.IsInvalid())
            {
                MIR_LOG_ERROR("PhysicsWorld: out of bodies, raise PhysicsWorldDesc::mMaxBodies");
                return {};
            }

            if (!dynamic)
                mBroadPhaseDirty = true;
            else
                Bodies().SetCollisionGroup(id, JPH::CollisionGroup(nullptr, detail::kDynamicGroup, id.GetIndex()));

            return BodyHandle{id.GetIndexAndSequenceNumber()};
        }

        // Mass properties are read while the body is still dynamic; Jolt only answers them then.
        struct Held
        {
            JPH::Vec3 mLinear = JPH::Vec3::sZero();
            JPH::Vec3 mAngular = JPH::Vec3::sZero();
            float mInverseMass = 0.0f;
            JPH::Vec3 mInverseInertia = JPH::Vec3::sZero();
            JPH::Quat mInertiaRotation = JPH::Quat::sIdentity();
        };

        Held *FindHeld(BodyHandle body)
        {
            const auto found = mHeld.find(body.mId);
            return found == mHeld.end() ? nullptr : &found->second;
        }

        const Held *FindHeld(BodyHandle body) const
        {
            const auto found = mHeld.find(body.mId);
            return found == mHeld.end() ? nullptr : &found->second;
        }

        // A held body is kinematic, so an impulse lands in the velocities it keeps.
        void HeldImpulse(JPH::BodyID id, Held &held, JPH::Vec3Arg impulse, JPH::Vec3Arg angularImpulse)
        {
            held.mLinear += impulse * held.mInverseMass;
            const JPH::Mat44 axes = JPH::Mat44::sRotation(Bodies().GetRotation(id) * held.mInertiaRotation);
            held.mAngular += axes.Multiply3x3(held.mInverseInertia * axes.Multiply3x3Transposed(angularImpulse));
        }

        // the first in declaration order to outlive other systems
        detail::JoltRuntimeRef mRuntime;

        JPH::TempAllocatorImpl mTempAllocator{10 * 1024 * 1024};
        JPH::JobSystemThreadPool mJobSystem;

        detail::BroadPhaseLayerInterfaceImpl mBroadPhaseLayers;
        detail::ObjectVsBroadPhaseLayerFilterImpl mObjectVsBroadPhase;
        detail::ObjectLayerPairFilterImpl mObjectLayerPairs;

        // Before mSystem, which keeps a pointer to it.
        detail::ContactTracker mContacts;
        std::vector<CollisionEvent> mCollisionEvents;

        JPH::PhysicsSystem mSystem;

        int mCollisionSteps = 1;
        bool mBroadPhaseDirty = false;
        std::size_t mHeightFieldsBuilt = 0;
        std::unordered_map<uint32_t, HeightFieldInfo> mHeightFieldInfo;
        // Shapes of height field bodies, which share them.
        std::unordered_map<uint32_t, SharedHeightField> mHeightFieldShapes;
        JPH::Ref<detail::ContactRestriction> mRestriction = new detail::ContactRestriction();
        std::unordered_map<uint32_t, Held> mHeld;
    };

    PhysicsWorld::PhysicsWorld(const PhysicsWorldDesc &desc) : mImpl(std::make_unique<Impl>(desc))
    {
    }

    PhysicsWorld::~PhysicsWorld() = default;

    BodyHandle PhysicsWorld::CreateBody(const BodyDesc &desc)
    {
        const JPH::Shape *shape = MakeShape(desc);
        if (shape == nullptr)
            return {};

        return mImpl->CreateBody(shape, desc.mPosition, desc.mRotation, desc.mMotion);
    }

    BodyHandle PhysicsWorld::CreateStaticHeightField(const HeightFieldDesc &desc)
    {
        const SharedHeightField shape = BuildHeightField(desc);
        if (!shape.IsValid())
            return {};
        ++mImpl->mHeightFieldsBuilt;
        return CreateStaticHeightField(shape, desc.mOrigin);
    }

    BodyHandle PhysicsWorld::CreateStaticHeightField(const SharedHeightField &shape, WorldPosition origin)
    {
        if (!shape.IsValid())
            return {};
        // Enhanced internal edge removal keeps planar crossings smooth.
        const BodyHandle body =
            mImpl->CreateBody(shape.mData->mShape, origin, kIdentityRotation, PhysicsMotion::Static, true);
        if (!body.IsValid())
            return body;
        if (const HeightFieldInfo *info = shape.Info())
            mImpl->mHeightFieldInfo.emplace(body.mId, *info);
        mImpl->mHeightFieldShapes.emplace(body.mId, shape);
        return body;
    }

    void PhysicsWorld::DestroyBody(BodyHandle body)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        JPH::BodyInterface &bodies = mImpl->Bodies();

        // Jolt's removal wakes nothing.
        JPH::AABox bounds = bodies.GetTransformedShape(*id).GetWorldSpaceBounds();
        bounds.ExpandBy(JPH::Vec3::sReplicate(kWakeMargin));

        mImpl->mContacts.ForgetBody(*id, bodies, mImpl->mCollisionEvents);

        bodies.RemoveBody(*id);
        bodies.DestroyBody(*id);
        mImpl->mHeightFieldInfo.erase(body.mId);
        mImpl->mHeightFieldShapes.erase(body.mId);
        mImpl->mHeld.erase(body.mId);
        mImpl->mRestriction->mAllowed.erase(id->GetIndex());

        bodies.ActivateBodiesInAABox(bounds, {}, {});
    }

    void PhysicsWorld::RestrictBody(BodyHandle staticBody)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(staticBody);
        if (!id || mImpl->Bodies().GetMotionType(*id) != JPH::EMotionType::Static)
            return;
        mImpl->Bodies().SetCollisionGroup(
            *id, JPH::CollisionGroup(mImpl->mRestriction, detail::kRestrictedGroup, id->GetIndex()));
    }

    void PhysicsWorld::RestrictContacts(BodyHandle dynamicBody, std::span<const BodyHandle> allowed)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(dynamicBody);
        if (!id)
            return;
        std::vector<JPH::uint32> &list = mImpl->mRestriction->mAllowed[id->GetIndex()];
        list.clear();
        for (const BodyHandle body : allowed)
            if (body.IsValid())
                list.push_back(ToBodyId(body).GetIndex());
        std::sort(list.begin(), list.end());
    }

    void PhysicsWorld::ClearContactRestriction(BodyHandle dynamicBody)
    {
        if (const std::optional<JPH::BodyID> id = mImpl->Live(dynamicBody))
            mImpl->mRestriction->mAllowed.erase(id->GetIndex());
    }

    void PhysicsWorld::HoldBody(BodyHandle body)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->FindHeld(body))
            return;
        JPH::BodyInterface &bodies = mImpl->Bodies();
        if (bodies.GetMotionType(*id) != JPH::EMotionType::Dynamic)
            return;
        Impl::Held held;
        bodies.GetLinearAndAngularVelocity(*id, held.mLinear, held.mAngular);
        {
            const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), *id);
            if (!lock.Succeeded())
                return;
            const JPH::MotionProperties &motion = *lock.GetBody().GetMotionProperties();
            held.mInverseMass = motion.GetInverseMass();
            held.mInverseInertia = motion.GetInverseInertiaDiagonal();
            held.mInertiaRotation = motion.GetInertiaRotation();
        }
        mImpl->mHeld.emplace(body.mId, held);
        bodies.SetMotionType(*id, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
        bodies.SetLinearAndAngularVelocity(*id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }

    void PhysicsWorld::ResumeBody(BodyHandle body)
    {
        const Impl::Held *held = mImpl->FindHeld(body);
        if (!held)
            return;
        const Impl::Held kept = *held;
        mImpl->mHeld.erase(body.mId);
        JPH::BodyInterface &bodies = mImpl->Bodies();
        const JPH::BodyID id = ToBodyId(body);
        bodies.SetMotionType(id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
        bodies.SetLinearAndAngularVelocity(id, kept.mLinear, kept.mAngular);
    }

    bool PhysicsWorld::IsBodyHeld(BodyHandle body) const
    {
        return mImpl->FindHeld(body) != nullptr;
    }

    glm::vec3 PhysicsWorld::Gravity() const
    {
        return FromJolt(mImpl->mSystem.GetGravity());
    }

    std::optional<WorldPosition> PhysicsWorld::BodyPosition(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return std::nullopt;

        return PositionFromJolt(mImpl->Bodies().GetPosition(*id));
    }

    glm::quat PhysicsWorld::BodyRotation(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return kIdentityRotation;

        return FromJolt(mImpl->Bodies().GetRotation(*id));
    }

    glm::vec3 PhysicsWorld::BodyLinearVelocity(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return glm::vec3(0.0f);
        if (const Impl::Held *held = mImpl->FindHeld(body))
            return FromJolt(held->mLinear);

        return FromJolt(mImpl->Bodies().GetLinearVelocity(*id));
    }

    glm::vec3 PhysicsWorld::BodyAngularVelocity(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return glm::vec3(0.0f);
        if (const Impl::Held *held = mImpl->FindHeld(body))
            return FromJolt(held->mAngular);

        return FromJolt(mImpl->Bodies().GetAngularVelocity(*id));
    }

    void PhysicsWorld::SetBodyTransform(BodyHandle body, WorldPosition position, const glm::quat &rotation)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        mImpl->Bodies().SetPositionAndRotation(*id, ToJolt(position), ToJolt(rotation), JPH::EActivation::Activate);
    }

    void PhysicsWorld::SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mLinear = ToJolt(velocity);
            return;
        }

        mImpl->Bodies().SetLinearVelocity(*id, ToJolt(velocity));
    }

    void PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mAngular = ToJolt(velocity);
            return;
        }

        mImpl->Bodies().SetAngularVelocity(*id, ToJolt(velocity));
    }

    void PhysicsWorld::AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mLinear += ToJolt(velocity);
            return;
        }

        mImpl->Bodies().AddLinearVelocity(*id, ToJolt(velocity));
    }

    void PhysicsWorld::AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mAngular += ToJolt(velocity);
            return;
        }

        mImpl->Bodies().AddLinearAndAngularVelocity(*id, JPH::Vec3::sZero(), ToJolt(velocity));
    }

    void PhysicsWorld::AddForce(BodyHandle body, const glm::vec3 &force)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->FindHeld(body))
            return;

        mImpl->Bodies().AddForce(*id, ToJolt(force));
    }

    void PhysicsWorld::AddTorque(BodyHandle body, const glm::vec3 &torque)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->FindHeld(body))
            return;

        mImpl->Bodies().AddTorque(*id, ToJolt(torque));
    }

    void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3 &impulse)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(*id, *held, ToJolt(impulse), JPH::Vec3::sZero());
            return;
        }

        mImpl->Bodies().AddImpulse(*id, ToJolt(impulse));
    }

    void PhysicsWorld::AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(*id, *held, JPH::Vec3::sZero(), ToJolt(angularImpulse));
            return;
        }

        mImpl->Bodies().AddAngularImpulse(*id, ToJolt(angularImpulse));
    }

    void PhysicsWorld::AddForceAtOffset(BodyHandle body, const glm::vec3 &force, const glm::vec3 &localOffset)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->FindHeld(body))
            return;

        JPH::BodyInterface &bodies = mImpl->Bodies();
        bodies.AddForce(*id, ToJolt(force), OffsetToWorldPoint(bodies, *id, localOffset));
    }

    void PhysicsWorld::AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse, const glm::vec3 &localOffset)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        JPH::BodyInterface &bodies = mImpl->Bodies();
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            const JPH::Vec3 arm = bodies.GetRotation(*id) * ToJolt(localOffset);
            mImpl->HeldImpulse(*id, *held, ToJolt(impulse), arm.Cross(ToJolt(impulse)));
            return;
        }

        bodies.AddImpulse(*id, ToJolt(impulse), OffsetToWorldPoint(bodies, *id, localOffset));
    }

    void PhysicsWorld::SetBodyEntity(BodyHandle body, Entity entity)
    {
        if (const std::optional<JPH::BodyID> id = mImpl->Live(body))
            mImpl->Bodies().SetUserData(*id, detail::EntityToUserData(entity));
    }

    Entity PhysicsWorld::BodyEntity(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return kNullEntity;

        return detail::EntityFromUserData(mImpl->Bodies().GetUserData(*id));
    }

    std::optional<RayHit> PhysicsWorld::RayCast(WorldPosition origin, const glm::vec3 &direction,
                                                float maxDistance) const
    {
        if (!(maxDistance > 0.0f))
        {
            MIR_LOG_ERROR("PhysicsWorld::RayCast: maxDistance must be positive");
            return std::nullopt;
        }
        const float length = glm::length(direction);
        if (!(length > 0.0f))
        {
            MIR_LOG_ERROR("PhysicsWorld::RayCast: direction must be finite and non-zero");
            return std::nullopt;
        }
        const glm::vec3 span = direction * (maxDistance / length);
        if (!std::isfinite(span.x) || !std::isfinite(span.y) || !std::isfinite(span.z))
        {
            MIR_LOG_ERROR("PhysicsWorld::RayCast: direction and maxDistance must give a finite ray");
            return std::nullopt;
        }

        const JPH::RRayCast ray(ToJolt(origin), ToJolt(span));
        JPH::RayCastResult hit;
        if (!mImpl->mSystem.GetNarrowPhaseQuery().CastRay(ray, hit))
            return std::nullopt;

        // origin + fraction * span: the float product is added in world precision
        const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
        const std::optional<WorldPosition> position = PositionFromJolt(point);
        if (!position)
            return std::nullopt;

        RayHit result;
        result.mBody = BodyHandle{hit.mBodyID.GetIndexAndSequenceNumber()};
        result.mPoint = *position;
        result.mDistance = hit.mFraction * maxDistance;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            result.mNormal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));

        return result;
    }

    std::optional<HeightFieldInfo> PhysicsWorld::HeightFieldInfoOf(BodyHandle body) const
    {
        if (!body.IsValid())
            return std::nullopt;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), ToBodyId(body));
        if (!lock.SucceededAndIsInBroadPhase())
            return std::nullopt;

        const JPH::Shape *shape = lock.GetBody().GetShape();
        if (shape->GetSubType() != JPH::EShapeSubType::HeightField)
            return std::nullopt;

        const auto found = mImpl->mHeightFieldInfo.find(body.mId);
        return found == mImpl->mHeightFieldInfo.end() ? std::nullopt : std::optional(found->second);
    }

    std::size_t PhysicsWorld::HeightFieldsBuilt() const
    {
        return mImpl->mHeightFieldsBuilt;
    }

    std::size_t PhysicsWorld::BodyCount() const
    {
        return mImpl->mSystem.GetNumBodies();
    }

    void PhysicsWorld::Step(float dt)
    {
        if (dt <= 0.0f)
            return;

        if (mImpl->mBroadPhaseDirty)
        {
            mImpl->mSystem.OptimizeBroadPhase();
            mImpl->mBroadPhaseDirty = false;
        }

        const JPH::EPhysicsUpdateError error =
            mImpl->mSystem.Update(dt, mImpl->mCollisionSteps, &mImpl->mTempAllocator, &mImpl->mJobSystem);

        if (error != JPH::EPhysicsUpdateError::None)
        {
            MIR_LOG_WARN("PhysicsWorld::Step: Jolt update error bits {}", static_cast<uint32_t>(error));
        }

        mImpl->mContacts.Flush(mImpl->Bodies(), mImpl->mCollisionEvents);
    }

    std::vector<CollisionEvent> PhysicsWorld::DrainCollisionEvents()
    {
        return std::exchange(mImpl->mCollisionEvents, {});
    }
}
