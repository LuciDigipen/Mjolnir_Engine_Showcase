/**
 * @file PhysicsWorld.cpp
 * @author Jackey Cen
 * @brief The Jolt side of PhysicsWorld: bodies, queries and the step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsWorld.h>
#include <physics/ColliderLines.h>

#include "CollisionLayers.h"
#include "ContactTracker.h"
#include "HeightFieldData.h"
#include "JoltConversions.h"
#include "JoltRuntime.h"

#include <core/log/Log.h>
#include <core/log/Assert.h>
#include <core/platform/CrashHandler.h>
#include <jobs/JobExecutor.h>

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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
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

        // A cast hits only the layers asked for, by the object layer's user layer.
        class RayLayerFilter final : public JPH::ObjectLayerFilter
        {
        public:
            explicit RayLayerFilter(uint32_t layers) : mLayers(layers) {}

            bool ShouldCollide(JPH::ObjectLayer layer) const override
            {
                return ((mLayers >> detail::Layers::UserLayer(layer)) & 1u) != 0;
            }

        private:
            uint32_t mLayers;
        };

        // The excluded bodies, the motion types asked for and whether triggers count.
        class RayBodyFilter final : public JPH::BodyFilter
        {
        public:
            explicit RayBodyFilter(const RayFilter &filter) : mFilter(filter)
            {
                for (const BodyHandle body : filter.mExclude)
                    mExcluded.push_back(ToBodyId(body));
            }

            bool ShouldCollide(const JPH::BodyID &id) const override
            {
                return std::find(mExcluded.begin(), mExcluded.end(), id) == mExcluded.end();
            }

            bool ShouldCollideLocked(const JPH::Body &body) const override
            {
                if (body.IsSensor() && !mFilter.mHitTriggers)
                    return false;

                const PhysicsMotion motion = body.GetMotionType() == JPH::EMotionType::Static ? PhysicsMotion::Static
                    : body.GetMotionType() == JPH::EMotionType::Dynamic                       ? PhysicsMotion::Dynamic
                                                                                              : PhysicsMotion::Kinematic;
                return (mFilter.mMotions & MotionBit(motion)) != 0;
            }

        private:
            const RayFilter &mFilter;
            std::vector<JPH::BodyID> mExcluded;
        };

        // Kinematic bodies move like dynamic ones as far as layers and activation go.
        bool Moves(PhysicsMotion motion) { return motion != PhysicsMotion::Static; }

        JPH::EMotionType MotionTypeOf(PhysicsMotion motion)
        {
            return motion == PhysicsMotion::Dynamic ? JPH::EMotionType::Dynamic
                : Moves(motion)                     ? JPH::EMotionType::Kinematic
                                                    : JPH::EMotionType::Static;
        }

        // Jolt stands a capsule along Y, so the other axes turn it.
        glm::quat AxisRotation(PhysicsAxis axis)
        {
            switch (axis)
            {
            case PhysicsAxis::X:
                return glm::angleAxis(-glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
            case PhysicsAxis::Z:
                return glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            default:
                return kIdentityRotation;
            }
        }

        bool Fits(float size)
        {
            return std::isfinite(size) && size > 0.0f;
        }

        // Jolt asserts on a negative damping, and a non-finite one would poison the velocity it scales.
        float DampingOf(float damping)
        {
            return std::isfinite(damping) ? std::max(damping, 0.0f) : 0.0f;
        }

        JPH::EMotionQuality MotionQualityOf(PhysicsMotionQuality quality)
        {
            return quality == PhysicsMotionQuality::LinearCast ? JPH::EMotionQuality::LinearCast
                                                              : JPH::EMotionQuality::Discrete;
        }

        PhysicsMotionQuality MotionQualityOf(JPH::EMotionQuality quality)
        {
            return quality == JPH::EMotionQuality::LinearCast ? PhysicsMotionQuality::LinearCast
                                                             : PhysicsMotionQuality::Discrete;
        }

        // Every size the shape reads Fits; MakeBaseShape refuses the rest.
        bool HasSize(const BodyDesc &desc)
        {
            switch (desc.mShape)
            {
            case PhysicsShape::Sphere:
                return Fits(desc.mRadius);
            case PhysicsShape::Capsule:
                return Fits(desc.mRadius) && Fits(desc.mHalfHeight);
            default:
                return Fits(desc.mHalfExtent.x) && Fits(desc.mHalfExtent.y) && Fits(desc.mHalfExtent.z);
            }
        }

        // halfHeight counts the caps, Jolt wants the cylinder between them, and a body with no
        // cylinder at all is a sphere that Jolt's capsule refuses
        float CylinderHalfHeight(const BodyDesc &desc)
        {
            return std::max(desc.mHalfHeight - desc.mRadius, 1.0e-3f);
        }

        // The size-only shape, in its own frame. Null, logged, for a size Jolt cannot build.
        const JPH::Shape *MakeBaseShape(const BodyDesc &desc)
        {
            const char *hint = "a collider needs a size above 0, so this one is empty until it is fixed";

            if (desc.mShape == PhysicsShape::Sphere)
            {
                if (!Fits(desc.mRadius))
                {
                    MIR_LOG_ERROR("PhysicsWorld: radius must be positive; {}", hint);
                    return nullptr;
                }
                return new JPH::SphereShape(desc.mRadius);
            }

            if (desc.mShape == PhysicsShape::Capsule)
            {
                if (!Fits(desc.mRadius) || !Fits(desc.mHalfHeight))
                {
                    MIR_LOG_ERROR("PhysicsWorld: radius and halfHeight must be positive; {}", hint);
                    return nullptr;
                }
                return new JPH::CapsuleShape(CylinderHalfHeight(desc), desc.mRadius);
            }

            const glm::vec3 &halfExtent = desc.mHalfExtent;
            if (!Fits(halfExtent.x) || !Fits(halfExtent.y) || !Fits(halfExtent.z))
            {
                MIR_LOG_ERROR("PhysicsWorld: half extent must be positive on every axis; {}", hint);
                return nullptr;
            }
            return new JPH::BoxShape(ToJolt(halfExtent));
        }

        // The base shape placed in the body's frame by the capsule axis and the collider offset, then the centre of
        // mass moved away from the shape's centre. A size Jolt cannot build gives a shape with no volume that
        // collides with nothing, so the body keeps its handle and a later edit swaps the real shape in.
        const JPH::Shape *MakeShape(const BodyDesc &desc)
        {
            const JPH::Shape *shape = MakeBaseShape(desc);
            if (shape == nullptr)
                shape = new JPH::EmptyShape();
            else
            {
                const glm::quat rotation = desc.mRotationOffset
                    * (desc.mShape == PhysicsShape::Capsule ? AxisRotation(desc.mAxis) : kIdentityRotation);
                if (rotation != kIdentityRotation || desc.mOffset != glm::vec3(0.0f))
                    shape = new JPH::RotatedTranslatedShape(ToJolt(desc.mOffset), ToJolt(rotation), shape);
            }

            if (desc.mCustomCenterOfMass && desc.mCenterOfMassOffset != glm::vec3(0.0f))
                return new JPH::OffsetCenterOfMassShape(shape, ToJolt(desc.mCenterOfMassOffset));
            return shape;
        }

        bool Finite(const glm::vec3 &value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        // Sylvester's criterion: the symmetric tensor is positive definite when every leading principal minor is.
        bool PositiveDefinite(const glm::vec3 &diagonal, const glm::vec3 &products)
        {
            const double a = diagonal.x, b = diagonal.y, c = diagonal.z;
            const double xy = products.x, xz = products.y, yz = products.z;
            const double det2 = a * b - xy * xy;
            const double det3 = a * b * c - a * yz * yz - b * xz * xz - c * xy * xy - 2.0 * xy * xz * yz;
            return a > 0.0 && b > 0.0 && c > 0.0 && det2 > 0.0 && det3 > 0.0;
        }

        // Off-diagonal entries in the tensor are the negated products of inertia.
        JPH::Mat44 InertiaTensor(const glm::vec3 &diagonal, const glm::vec3 &products)
        {
            const JPH::Vec3 d = ToJolt(diagonal);
            const JPH::Vec3 p = ToJolt(products);
            return JPH::Mat44(
                JPH::Vec4(d.GetX(), -p.GetX(), -p.GetY(), 0.0f),
                JPH::Vec4(-p.GetX(), d.GetY(), -p.GetZ(), 0.0f),
                JPH::Vec4(-p.GetY(), -p.GetZ(), d.GetZ(), 0.0f),
                JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        // The body's mass and inertia from desc: a supplied tensor is absolute, anything else is the shape's scaled
        // to the mass. A tensor that is not finite or not positive definite is logged and refused.
        struct MassOverride
        {
            bool mProvided = false;
            JPH::MassProperties mProperties;
        };

        MassOverride MassOf(const BodyDesc &desc, const JPH::Shape &shape)
        {
            MassOverride result;
            if (!desc.mCustomInertia)
                return result;

            if (!Finite(desc.mInertiaDiagonal) || !Finite(desc.mInertiaProducts)
                || !PositiveDefinite(desc.mInertiaDiagonal, desc.mInertiaProducts))
            {
                MIR_LOG_ERROR("PhysicsWorld: the inertia tensor must be finite and positive definite (a positive "
                              "diagonal and every leading minor above 0); using the shape-derived inertia");
                return result;
            }

            const float mass = desc.mMass > 0.0f ? desc.mMass : shape.GetMassProperties().mMass;
            if (!(mass > 0.0f) || !std::isfinite(mass))
            {
                MIR_LOG_ERROR("PhysicsWorld: a supplied inertia tensor needs a mass above 0; using the shape-derived "
                              "inertia");
                return result;
            }

            result.mProvided = true;
            result.mProperties.mMass = mass;
            result.mProperties.mInertia = InertiaTensor(desc.mInertiaDiagonal, desc.mInertiaProducts);
            return result;
        }

        // Places the shape's center: offset is in the body's own frame.
        std::optional<WorldPosition> ShapeCenter(WorldPosition body, const glm::quat &rotation, const glm::vec3 &offset)
        {
            const std::optional<WorldOffset> moved = WorldOffset::FromMeters(WorldVec3(rotation * offset));
            return moved ? TryTranslateWorld(body, *moved) : std::nullopt;
        }

        PhysicsMotion MotionOf(JPH::EMotionType motion)
        {
            switch (motion)
            {
            case JPH::EMotionType::Dynamic:
                return PhysicsMotion::Dynamic;
            case JPH::EMotionType::Kinematic:
                return PhysicsMotion::Kinematic;
            default:
                return PhysicsMotion::Static;
            }
        }

        // The base shape's sizes into view; false for a shape that is not drawn (empty, or a kind CreateBody never
        // makes). grid holds a height field's thinned samples for view to point at.
        bool ReadShape(const JPH::Shape &shape, ColliderView &view, std::vector<glm::vec3> &grid)
        {
            switch (shape.GetSubType())
            {
            case JPH::EShapeSubType::Box:
                view.mShape = PhysicsShape::Box;
                view.mHalfExtent = FromJolt(static_cast<const JPH::BoxShape &>(shape).GetHalfExtent());
                return true;
            case JPH::EShapeSubType::Sphere:
                view.mShape = PhysicsShape::Sphere;
                view.mRadius = static_cast<const JPH::SphereShape &>(shape).GetRadius();
                return true;
            case JPH::EShapeSubType::Capsule:
                {
                    const auto &capsule = static_cast<const JPH::CapsuleShape &>(shape);
                    view.mShape = PhysicsShape::Capsule;
                    view.mRadius = capsule.GetRadius();
                    view.mCylinderHalfHeight = capsule.GetHalfHeightOfCylinder();
                    return true;
                }
            case JPH::EShapeSubType::HeightField:
                {
                    const auto &field = static_cast<const JPH::HeightFieldShape &>(shape);
                    const uint32_t samples = field.GetSampleCount();
                    if (samples < 2)
                        return false;
                    const uint32_t lines = PhysicsWorld::kColliderGridLines;
                    const uint32_t stride = std::max(1u, (samples - 1 + lines - 1) / lines);
                    const uint32_t size = (samples - 1) / stride + 1;

                    grid.resize(static_cast<size_t>(size) * size);
                    for (uint32_t z = 0; z < size; ++z)
                    {
                        for (uint32_t x = 0; x < size; ++x)
                        {
                            const uint32_t sx = x * stride, sz = z * stride;
                            grid[static_cast<size_t>(z) * size + x] = field.IsNoCollision(sx, sz)
                                ? glm::vec3(std::numeric_limits<float>::quiet_NaN())
                                : FromJolt(field.GetPosition(sx, sz));
                        }
                    }
                    view.mHeightField = true;
                    view.mHeightGrid = grid;
                    view.mGridSize = size;
                    return true;
                }
            default:
                return false;
            }
        }
    }

    std::optional<ColliderView> ColliderViewOf(const BodyDesc &desc)
    {
        // MakeShape builds an empty shape from these, and CreateBody logs it; nothing to outline
        if (!HasSize(desc))
            return std::nullopt;

        const std::optional<WorldPosition> center = ShapeCenter(desc.mPosition, desc.mRotation, desc.mOffset);
        if (!center)
            return std::nullopt;

        ColliderView view;
        view.mShape = desc.mShape;
        view.mPosition = *center;
        view.mRotation = desc.mRotation * desc.mRotationOffset *
            (desc.mShape == PhysicsShape::Capsule ? AxisRotation(desc.mAxis) : kIdentityRotation);
        view.mHalfExtent = desc.mHalfExtent;
        view.mRadius = desc.mRadius;
        view.mCylinderHalfHeight = CylinderHalfHeight(desc);
        view.mMotion = desc.mMotion;
        view.mTrigger = desc.mIsTrigger;
        return view;
    }

    struct PhysicsWorld::Impl
    {
        explicit Impl(const PhysicsWorldDesc &desc)
            : mCollisionSteps(desc.mCollisionSteps > 0 ? desc.mCollisionSteps : 1)
        {
            // armed like the engine's own threads, so a terminate on a job thread reports
            // through the crash handler's terminate route, not the abort fallback
            mJobSystem.SetThreadInitFunction([](int) { ArmCrashHandler(); });
            mJobSystem.Init(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs), static_cast<JPH::uint>(JPH::cMaxPhysicsBarriers),
                desc.mWorkerThreads >= 0 ? desc.mWorkerThreads : static_cast<int>(JobExecutor::DefaultWorkerCount()));

            mSystem.Init(desc.mMaxBodies,
                         0, // body mutexes: 0 lets Jolt pick a default
                         desc.mMaxBodyPairs, desc.mMaxContactConstraints, mBroadPhaseLayers, mObjectVsBroadPhase,
                         mObjectLayerPairs);

            mSystem.SetGravity(ToJolt(desc.mGravity));
            static_assert(noexcept(mContacts.OnContactAdded(std::declval<const JPH::Body &>(),
                              std::declval<const JPH::Body &>(), std::declval<const JPH::ContactManifold &>(),
                              std::declval<JPH::ContactSettings &>())) &&
                              noexcept(mContacts.OnContactPersisted(std::declval<const JPH::Body &>(),
                                  std::declval<const JPH::Body &>(), std::declval<const JPH::ContactManifold &>(),
                                  std::declval<JPH::ContactSettings &>())) &&
                              noexcept(mContacts.OnContactRemoved(std::declval<const JPH::SubShapeIDPair &>())),
                "C callbacks and thread entry points must be noexcept (conventions.md)");
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
                              PhysicsMotion motion, bool enhancedInternalEdgeRemoval = false,
                              const BodyDesc *properties = nullptr)
        {
            const int layer = properties != nullptr ? properties->mLayer : 0;
            if (!IsPhysicsLayer(layer))
            {
                MIR_LOG_ERROR("PhysicsWorld::CreateBody: layer {} is outside 0 to {}", layer, kPhysicsLayerCount - 1);
                return {};
            }

            const JPH::RefConst<JPH::Shape> owned(shape);
            const bool dynamic = motion == PhysicsMotion::Dynamic;
            // Kinematic bodies move, so they live in the moving layer and start awake
            const bool moving = Moves(motion);

            MassOverride customMass;
            JPH::BodyCreationSettings settings(owned, ToJolt(position), ToJolt(rotation), MotionTypeOf(motion),
                                               detail::Layers::Of(layer, moving));
            settings.mEnhancedInternalEdgeRemoval = enhancedInternalEdgeRemoval;
            if (properties != nullptr)
            {
                // so a Static body can become Dynamic or Kinematic later without being rebuilt
                settings.mAllowDynamicOrKinematic = true;
                settings.mIsSensor = properties->mIsTrigger;
                settings.mFriction = properties->mFriction;
                settings.mRestitution = properties->mRestitution;
                settings.mGravityFactor = properties->mGravityFactor;
                settings.mAllowSleeping = properties->mAllowSleeping;
                settings.mLinearDamping = DampingOf(properties->mLinearDamping);
                settings.mAngularDamping = DampingOf(properties->mAngularDamping);
                settings.mApplyGyroscopicForce = properties->mGyroscopicForce;
                settings.mMotionQuality = MotionQualityOf(properties->mMotionQuality);
                // the shape gives the inertia, the mass scales it; given whatever the motion, for a later switch
                customMass = MassOf(*properties, *owned);
                if (customMass.mProvided)
                {
                    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                    settings.mMassPropertiesOverride = customMass.mProperties;
                }
                else if (properties->mMass > 0.0f)
                {
                    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass = properties->mMass;
                }
            }

            const JPH::BodyID id = Bodies().CreateAndAddBody(
                settings, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

            if (id.IsInvalid())
            {
                MIR_LOG_ERROR("PhysicsWorld: out of bodies, raise PhysicsWorldDesc::mMaxBodies");
                return {};
            }

            if (customMass.mProvided)
                mCustomInertiaBodies.insert(id.GetIndexAndSequenceNumber());

            if (!moving)
                mBroadPhaseDirty = true;
            else if (dynamic)
                Bodies().SetCollisionGroup(id, JPH::CollisionGroup(nullptr, detail::kDynamicGroup, id.GetIndex()));

            return BodyHandle{id.GetIndexAndSequenceNumber()};
        }

        // The mass and inertia rules of desc on the body's current shape, without rebuilding it. A supplied tensor
        // replaces the shape's and is absolute; otherwise the shape's inertia is scaled to the mass.
        void ApplyMassProperties(BodyHandle body, JPH::BodyID id, const BodyDesc &desc)
        {
            const JPH::RefConst<JPH::Shape> shape = Bodies().GetShape(id);
            const MassOverride custom = MassOf(desc, *shape);
            // recorded even while the body is Static, so a later switch to Dynamic keeps it
            if (custom.mProvided)
                mCustomInertiaBodies.insert(body.mId);
            else
                mCustomInertiaBodies.erase(body.mId);

            JPH::MassProperties properties = custom.mProperties;
            if (!custom.mProvided)
            {
                properties = shape->GetMassProperties();
                if (desc.mMass > 0.0f && properties.mMass > 0.0f)
                    properties.ScaleToMass(desc.mMass);
            }

            {
                const JPH::BodyLockWrite lock(mSystem.GetBodyLockInterface(), id);
                // a Static body has no motion state to hold them; they are put on when it moves
                if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Dynamic)
                    return;
                lock.GetBody().GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, properties);
            }
            // after the lock is released: the body interface takes it again
            Bodies().ActivateBody(id);
        }

        // What a call moves a body by decides which bodies take it.
        enum class Call
        {
            Force,    // forces and torques: a Dynamic body
            Impulse,  // impulses: a Dynamic body, or a held one, which keeps them as velocity
            Velocity, // velocities: any body that moves, a held one included
        };

        // The body's id when it takes the call, else nullopt with the reason logged (the stale handle, a vector that is
        // not finite, or a motion type that ignores the call), so a false from the caller can be trusted.
        std::optional<JPH::BodyID> Resolve(BodyHandle body, Call call, const char *name,
            std::initializer_list<glm::vec3> vectors) const
        {
            const std::optional<JPH::BodyID> id = Live(body);
            if (!id)
            {
                MIR_LOG_ERROR("PhysicsWorld::{}: the body no longer exists", name);
                return std::nullopt;
            }

            for (const glm::vec3 &vector : vectors)
            {
                if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z))
                {
                    MIR_LOG_ERROR("PhysicsWorld::{}: a vector must be finite, not NaN or infinite", name);
                    return std::nullopt;
                }
            }

            const JPH::EMotionType type = Bodies().GetMotionType(*id);
            const bool held = FindHeld(body) != nullptr;
            switch (call)
            {
            case Call::Force:
                if (type == JPH::EMotionType::Dynamic)
                    return id;
                MIR_LOG_ERROR("PhysicsWorld::{}: {}", name,
                    held ? "a held body takes no force" : "only a Dynamic body takes a force or a torque");
                return std::nullopt;
            case Call::Impulse:
                if (type == JPH::EMotionType::Dynamic || held)
                    return id;
                MIR_LOG_ERROR("PhysicsWorld::{}: only a Dynamic body takes an impulse", name);
                return std::nullopt;
            case Call::Velocity:
                if (type != JPH::EMotionType::Static)
                    return id;
                MIR_LOG_ERROR("PhysicsWorld::{}: a Static body has no velocity", name);
                return std::nullopt;
            }
            return std::nullopt;
        }

        // Where bodies resting on or against this one are, with a margin.
        JPH::AABox WakeBounds(JPH::BodyID id) const
        {
            JPH::AABox bounds = Bodies().GetTransformedShape(id).GetWorldSpaceBounds();
            bounds.ExpandBy(JPH::Vec3::sReplicate(kWakeMargin));
            return bounds;
        }

        // An edit to a body changes what rests on it or against it, and Jolt wakes nothing by itself.
        void WakeAround(JPH::BodyID id) { Bodies().ActivateBodiesInAABox(WakeBounds(id), {}, {}); }

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
        PhysicsLayerTable mLayerTable;
        detail::ObjectLayerPairFilterImpl mObjectLayerPairs{mLayerTable};

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
        // Handle ids of the bodies whose inertia is the supplied tensor rather than the shape's.
        std::unordered_set<uint32_t> mCustomInertiaBodies;
        // Handle ids of the bodies whose pairs get Stay events.
        std::unordered_set<uint32_t> mStayBodies;
    };

    PhysicsWorld::PhysicsWorld(const PhysicsWorldDesc &desc) : mImpl(std::make_unique<Impl>(desc))
    {
    }

    PhysicsWorld::~PhysicsWorld() = default;

    BodyHandle PhysicsWorld::CreateBody(const BodyDesc &desc)
    {
        const JPH::Shape *shape = MakeShape(desc);
        const BodyHandle body = mImpl->CreateBody(shape, desc.mPosition, desc.mRotation, desc.mMotion, false, &desc);
        if (body.IsValid() && desc.mDetectStay)
            mImpl->mStayBodies.insert(body.mId);
        return body;
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
        const JPH::AABox bounds = mImpl->WakeBounds(*id);

        mImpl->mContacts.ForgetBody(*id, bodies, mImpl->mCollisionEvents);

        bodies.RemoveBody(*id);
        bodies.DestroyBody(*id);
        mImpl->mHeightFieldInfo.erase(body.mId);
        mImpl->mHeightFieldShapes.erase(body.mId);
        mImpl->mHeld.erase(body.mId);
        mImpl->mCustomInertiaBodies.erase(body.mId);
        mImpl->mStayBodies.erase(body.mId);
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

    uint32_t PhysicsWorld::WorkerCount() const
    {
        return static_cast<uint32_t>(mImpl->mJobSystem.GetMaxConcurrency() - 1);
    }

    void PhysicsWorld::ReadBodyPoses(std::span<const BodyHandle> bodies, std::span<BodyPoseSnapshot> poses,
        JobExecutor *jobs) const
    {
        if (bodies.size() != poses.size()) throw std::invalid_argument("physics body/pose span sizes differ");
        MIR_ASSERT(!mStepping, "PhysicsWorld: readback must run after the physics step has joined");
        const auto read = [&](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
            {
                poses[i] = {};
                if (!bodies[i].IsValid()) continue;
                const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), ToBodyId(bodies[i]));
                if (!lock.SucceededAndIsInBroadPhase()) continue;
                const auto &body = lock.GetBody();
                poses[i].position = PositionFromJolt(body.GetPosition());
                poses[i].rotation = FromJolt(body.GetRotation());
            }
        };
        // Avoid dispatch for small worlds. Each range holds only one read lock at a time.
        if (jobs && jobs->WorkerCount() > 0 && bodies.size() >= 128)
            jobs->ParallelFor(bodies.size(), 16, read);
        else
            read(0, bodies.size());
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

    void PhysicsWorld::SetBodyMotion(BodyHandle body, PhysicsMotion motion)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->FindHeld(body))
            return;

        const bool dynamic = motion == PhysicsMotion::Dynamic;
        const bool moving = Moves(motion);

        JPH::BodyInterface &bodies = mImpl->Bodies();
        bodies.SetObjectLayer(*id, detail::Layers::Of(detail::Layers::UserLayer(bodies.GetObjectLayer(*id)), moving));
        bodies.SetMotionType(*id, MotionTypeOf(motion), moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        bodies.SetCollisionGroup(*id,
            dynamic ? JPH::CollisionGroup(nullptr, detail::kDynamicGroup, id->GetIndex()) : JPH::CollisionGroup());
        if (!moving)
            mImpl->mBroadPhaseDirty = true;
        mImpl->WakeAround(*id);
    }

    void PhysicsWorld::SetBodyShape(BodyHandle body, const BodyDesc &desc)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        const JPH::RefConst<JPH::Shape> shape(MakeShape(desc));
        mImpl->Bodies().SetShape(*id, shape, true, JPH::EActivation::Activate);
        // the shape's own volume gave the mass back, so the authored one and the supplied tensor are put on again
        mImpl->ApplyMassProperties(body, *id, desc);
        mImpl->mBroadPhaseDirty = true;
        mImpl->WakeAround(*id);
    }

    void PhysicsWorld::SetBodyTrigger(BodyHandle body, bool trigger)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        mImpl->Bodies().SetIsSensor(*id, trigger);
        mImpl->WakeAround(*id);
    }

    void PhysicsWorld::SetBodyLayer(BodyHandle body, int layer)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;
        if (!IsPhysicsLayer(layer))
        {
            MIR_LOG_ERROR("PhysicsWorld::SetBodyLayer: layer {} is outside 0 to {}", layer, kPhysicsLayerCount - 1);
            return;
        }

        JPH::BodyInterface &bodies = mImpl->Bodies();
        bodies.SetObjectLayer(*id, detail::Layers::Of(layer, detail::Layers::IsMoving(bodies.GetObjectLayer(*id))));
        mImpl->WakeAround(*id);
    }

    void PhysicsWorld::SetLayerTable(const PhysicsLayerTable &table)
    {
        mImpl->mLayerTable = table;

        JPH::BodyIDVector ids;
        mImpl->mSystem.GetBodies(ids);
        if (!ids.empty())
            mImpl->Bodies().ActivateBodies(ids.data(), static_cast<int>(ids.size()));
    }

    const PhysicsLayerTable &PhysicsWorld::LayerTable() const
    {
        return mImpl->mLayerTable;
    }

    void PhysicsWorld::SetBodyMaterial(BodyHandle body, float friction, float restitution)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        mImpl->Bodies().SetFriction(*id, friction);
        mImpl->Bodies().SetRestitution(*id, restitution);
        mImpl->WakeAround(*id);
    }

    void PhysicsWorld::SetBodyGravityFactor(BodyHandle body, float factor)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        mImpl->Bodies().SetGravityFactor(*id, factor);
        mImpl->Bodies().ActivateBody(*id);
    }

    void PhysicsWorld::SetBodyMass(BodyHandle body, float mass)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mass <= 0.0f)
            return;

        const bool customInertia = mImpl->mCustomInertiaBodies.find(body.mId) != mImpl->mCustomInertiaBodies.end();
        {
            const JPH::BodyLockWrite lock(mImpl->mSystem.GetBodyLockInterface(), *id);
            // a Static body has no mass of its own to scale until it moves, and Jolt asserts on asking it
            if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Dynamic)
                return;
            JPH::MotionProperties *motion = lock.GetBody().GetMotionProperties();
            // a supplied tensor is absolute, so only the mass follows
            if (customInertia)
                motion->SetInverseMass(1.0f / mass);
            else
                motion->ScaleToMass(mass);
        }
        // after the lock is released: the body interface takes it again
        mImpl->Bodies().ActivateBody(*id);
    }

    void PhysicsWorld::SetBodyMassProperties(BodyHandle body, const BodyDesc &desc)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        mImpl->ApplyMassProperties(body, *id, desc);
    }

    void PhysicsWorld::SetBodyAllowSleeping(BodyHandle body, bool allow)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        {
            const JPH::BodyLockWrite lock(mImpl->mSystem.GetBodyLockInterface(), *id);
            if (!lock.Succeeded())
                return;
            lock.GetBody().SetAllowSleeping(allow);
        }
        // a body asleep stays asleep when sleeping is turned off; the body interface takes the lock again
        if (!allow)
            mImpl->Bodies().ActivateBody(*id);
    }

    void PhysicsWorld::SetBodyDamping(BodyHandle body, float linear, float angular)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        {
            const JPH::BodyLockWrite lock(mImpl->mSystem.GetBodyLockInterface(), *id);
            if (!lock.Succeeded())
                return;
            JPH::Body &joltBody = lock.GetBody();
            // a Static body has no motion state to hold a damping; the authored one is put on when it moves
            if (joltBody.IsStatic())
                return;
            joltBody.GetMotionProperties()->SetLinearDamping(DampingOf(linear));
            joltBody.GetMotionProperties()->SetAngularDamping(DampingOf(angular));
        }
        // after the lock is released: the body interface takes it again
        mImpl->Bodies().ActivateBody(*id);
    }

    float PhysicsWorld::BodyLinearDamping(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return 0.0f;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), *id);
        if (!lock.Succeeded() || lock.GetBody().IsStatic())
            return JPH::BodyCreationSettings().mLinearDamping;
        return lock.GetBody().GetMotionProperties()->GetLinearDamping();
    }

    float PhysicsWorld::BodyAngularDamping(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return 0.0f;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), *id);
        if (!lock.Succeeded() || lock.GetBody().IsStatic())
            return JPH::BodyCreationSettings().mAngularDamping;
        return lock.GetBody().GetMotionProperties()->GetAngularDamping();
    }

    void PhysicsWorld::SetBodyGyroscopicForce(BodyHandle body, bool apply)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return;

        {
            const JPH::BodyLockWrite lock(mImpl->mSystem.GetBodyLockInterface(), *id);
            if (!lock.Succeeded() || !lock.GetBody().IsRigidBody())
                return;
            lock.GetBody().SetApplyGyroscopicForce(apply);
        }
        mImpl->Bodies().ActivateBody(*id);
    }

    bool PhysicsWorld::BodyGyroscopicForce(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id)
            return false;

        const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), *id);
        if (!lock.Succeeded() || !lock.GetBody().IsRigidBody())
            return false;
        return lock.GetBody().GetApplyGyroscopicForce();
    }

    void PhysicsWorld::SetBodyMotionQuality(BodyHandle body, PhysicsMotionQuality quality)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->Bodies().GetMotionType(*id) == JPH::EMotionType::Static)
            return;

        mImpl->Bodies().SetMotionQuality(*id, MotionQualityOf(quality));
        mImpl->Bodies().ActivateBody(*id);
    }

    PhysicsMotionQuality PhysicsWorld::BodyMotionQuality(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->Bodies().GetMotionType(*id) == JPH::EMotionType::Static)
            return PhysicsMotionQuality::Discrete;

        return MotionQualityOf(mImpl->Bodies().GetMotionQuality(*id));
    }

    void PhysicsWorld::WakeBody(BodyHandle body)
    {
        if (const std::optional<JPH::BodyID> id = mImpl->Live(body))
            mImpl->Bodies().ActivateBody(*id);
    }

    void PhysicsWorld::SetBodyDetectsStay(BodyHandle body, bool detects)
    {
        if (!mImpl->Live(body))
            return;

        if (detects)
            mImpl->mStayBodies.insert(body.mId);
        else
            mImpl->mStayBodies.erase(body.mId);
    }

    bool PhysicsWorld::IsBodySleeping(BodyHandle body) const
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        return id && mImpl->Bodies().GetMotionType(*id) != JPH::EMotionType::Static && !mImpl->Bodies().IsActive(*id);
    }

    void PhysicsWorld::MoveBodyKinematic(BodyHandle body, WorldPosition position, const glm::quat &rotation, float dt)
    {
        const std::optional<JPH::BodyID> id = mImpl->Live(body);
        if (!id || mImpl->Bodies().GetMotionType(*id) != JPH::EMotionType::Kinematic || dt <= 0.0f)
            return;

        mImpl->Bodies().MoveKinematic(*id, ToJolt(position), ToJolt(rotation), dt);
    }

    bool PhysicsWorld::SetBodyLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Velocity, "SetBodyLinearVelocity", { velocity });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mLinear = ToJolt(velocity);
            return true;
        }

        mImpl->Bodies().SetLinearVelocity(*id, ToJolt(velocity));
        return true;
    }

    bool PhysicsWorld::SetBodyAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Velocity, "SetBodyAngularVelocity", { velocity });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mAngular = ToJolt(velocity);
            return true;
        }

        mImpl->Bodies().SetAngularVelocity(*id, ToJolt(velocity));
        return true;
    }

    bool PhysicsWorld::AddLinearVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Velocity, "AddLinearVelocity", { velocity });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mLinear += ToJolt(velocity);
            return true;
        }

        mImpl->Bodies().AddLinearVelocity(*id, ToJolt(velocity));
        return true;
    }

    bool PhysicsWorld::AddAngularVelocity(BodyHandle body, const glm::vec3 &velocity)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Velocity, "AddAngularVelocity", { velocity });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            held->mAngular += ToJolt(velocity);
            return true;
        }

        mImpl->Bodies().AddLinearAndAngularVelocity(*id, JPH::Vec3::sZero(), ToJolt(velocity));
        return true;
    }

    bool PhysicsWorld::AddForce(BodyHandle body, const glm::vec3 &force)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Force, "AddForce", { force });
        if (!id)
            return false;

        mImpl->Bodies().AddForce(*id, ToJolt(force));
        return true;
    }

    bool PhysicsWorld::AddTorque(BodyHandle body, const glm::vec3 &torque)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Force, "AddTorque", { torque });
        if (!id)
            return false;

        mImpl->Bodies().AddTorque(*id, ToJolt(torque));
        return true;
    }

    bool PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3 &impulse)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Impulse, "AddImpulse", { impulse });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(*id, *held, ToJolt(impulse), JPH::Vec3::sZero());
            return true;
        }

        mImpl->Bodies().AddImpulse(*id, ToJolt(impulse));
        return true;
    }

    bool PhysicsWorld::AddAngularImpulse(BodyHandle body, const glm::vec3 &angularImpulse)
    {
        const std::optional<JPH::BodyID> id = mImpl->Resolve(body, Impl::Call::Impulse, "AddAngularImpulse", { angularImpulse });
        if (!id)
            return false;
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            mImpl->HeldImpulse(*id, *held, JPH::Vec3::sZero(), ToJolt(angularImpulse));
            return true;
        }

        mImpl->Bodies().AddAngularImpulse(*id, ToJolt(angularImpulse));
        return true;
    }

    bool PhysicsWorld::AddForceAtOffset(BodyHandle body, const glm::vec3 &force, const glm::vec3 &localOffset)
    {
        const std::optional<JPH::BodyID> id =
            mImpl->Resolve(body, Impl::Call::Force, "AddForceAtOffset", { force, localOffset });
        if (!id)
            return false;

        JPH::BodyInterface &bodies = mImpl->Bodies();
        bodies.AddForce(*id, ToJolt(force), OffsetToWorldPoint(bodies, *id, localOffset));
        return true;
    }

    bool PhysicsWorld::AddImpulseAtOffset(BodyHandle body, const glm::vec3 &impulse, const glm::vec3 &localOffset)
    {
        const std::optional<JPH::BodyID> id =
            mImpl->Resolve(body, Impl::Call::Impulse, "AddImpulseAtOffset", { impulse, localOffset });
        if (!id)
            return false;

        JPH::BodyInterface &bodies = mImpl->Bodies();
        if (Impl::Held *held = mImpl->FindHeld(body))
        {
            const JPH::Vec3 arm = bodies.GetRotation(*id) * ToJolt(localOffset);
            mImpl->HeldImpulse(*id, *held, ToJolt(impulse), arm.Cross(ToJolt(impulse)));
            return true;
        }

        bodies.AddImpulse(*id, ToJolt(impulse), OffsetToWorldPoint(bodies, *id, localOffset));
        return true;
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
                                                float maxDistance, const RayFilter &filter) const
    {
        if (filter.mLayers == 0)
        {
            MIR_LOG_ERROR("PhysicsWorld::RayCast: the filter asks for no layer, so nothing can be hit");
            return std::nullopt;
        }
        if ((filter.mMotions & kAllMotions) == 0)
        {
            MIR_LOG_ERROR("PhysicsWorld::RayCast: the filter asks for no motion type, so nothing can be hit");
            return std::nullopt;
        }

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
        const RayLayerFilter layerFilter(filter.mLayers);
        const RayBodyFilter bodyFilter(filter);
        if (!mImpl->mSystem.GetNarrowPhaseQuery().CastRay(ray, hit, {}, layerFilter, bodyFilter))
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

    void PhysicsWorld::ForEachCollider(const std::function<void(const ColliderView &)> &visit) const
    {
        JPH::BodyIDVector ids;
        mImpl->mSystem.GetBodies(ids);

        std::vector<glm::vec3> grid;
        for (const JPH::BodyID id : ids)
        {
            const JPH::BodyLockRead lock(mImpl->mSystem.GetBodyLockInterface(), id);
            if (!lock.Succeeded())
                continue;
            const JPH::Body &body = lock.GetBody();

            const std::optional<WorldPosition> origin = PositionFromJolt(body.GetPosition());
            if (!origin)
                continue;

            ColliderView view;
            view.mMotion = MotionOf(body.GetMotionType());
            view.mTrigger = body.IsSensor();
            view.mSleeping = !body.IsStatic() && !body.IsActive();

            // MakeShape wraps the base shape only when the axis or the collider offset moves it
            view.mPosition = *origin;
            view.mRotation = FromJolt(body.GetRotation());
            const JPH::Shape *shape = body.GetShape();
            // the centre of mass offset leaves the collider where it was
            if (shape->GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass)
                shape = static_cast<const JPH::OffsetCenterOfMassShape &>(*shape).GetInnerShape();
            if (shape->GetSubType() == JPH::EShapeSubType::RotatedTranslated)
            {
                const auto &placed = static_cast<const JPH::RotatedTranslatedShape &>(*shape);
                const std::optional<WorldPosition> center =
                    ShapeCenter(*origin, view.mRotation, FromJolt(placed.GetPosition()));
                if (!center)
                    continue;
                view.mPosition = *center;
                view.mRotation = view.mRotation * FromJolt(placed.GetRotation());
                shape = placed.GetInnerShape();
            }

            if (ReadShape(*shape, view, grid))
                visit(view);
        }
    }

    void PhysicsWorld::Step(float dt)
    {
        if (dt <= 0.0f)
            return;

        struct SteppingScope
        {
            explicit SteppingScope(bool &flag) : mFlag(flag) { mFlag = true; }
            ~SteppingScope() { mFlag = false; }
            bool &mFlag;
        } stepping(mStepping);

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
        std::vector<CollisionEvent> events = std::exchange(mImpl->mCollisionEvents, {});
        if (!mImpl->mStayBodies.empty())
            mImpl->mContacts.AppendStay(mImpl->Bodies(), mImpl->mStayBodies, events);
        return events;
    }
}
