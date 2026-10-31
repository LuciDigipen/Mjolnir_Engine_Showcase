/**
 * @file SnapToTerrain.cpp
 * @author Jackey Cen
 * @brief One undoable Snap to Terrain: source-precision samples, a fitted plane and a pending session.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/SnapToTerrain.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>
#include <editor/Selection.h>
#include <editor/history/AuthoredEdit.h>
#include <editor/history/ComponentCommands.h>
#include <renderer/components/MeshBounds.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr double kMinScale = 1e-6;
        constexpr std::size_t kMaxDiagnosticLength = 240;
        constexpr double kHeadingEpsilon = 1e-12;
        constexpr double kVerticalNormal = 1e-4;

        bool Finite(const glm::dvec3 &value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool Finite(const glm::vec3 &value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool Finite(const glm::quat &value)
        {
            return std::isfinite(value.w) && std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }

        std::string Bound(std::string text)
        {
            if (text.size() > kMaxDiagnosticLength)
                text.resize(kMaxDiagnosticLength);
            return text;
        }

        glm::mat3 Linear(const glm::quat &rotation, const glm::vec3 &scale)
        {
            glm::mat3 linear = glm::mat3_cast(rotation);
            linear[0] *= scale.x;
            linear[1] *= scale.y;
            linear[2] *= scale.z;
            return linear;
        }

        double BottomY(const SnapToTerrainFacts &facts, const glm::mat3 &linear)
        {
            // A negative transformed local-Y direction makes local max Y the lower face.
            return linear[1].y < 0.0f ? facts.mLocalMax.y : facts.mLocalMin.y;
        }

        glm::dvec3 BottomCenter(const SnapToTerrainFacts &facts, const glm::mat3 &linear)
        {
            if (!facts.mHasBounds)
                return glm::dvec3(0.0);
            return {(facts.mLocalMin.x + facts.mLocalMax.x) * 0.5, BottomY(facts, linear),
                    (facts.mLocalMin.z + facts.mLocalMax.z) * 0.5};
        }

        std::optional<WorldPosition> OffsetWorld(WorldPosition origin, const glm::dvec3 &offset)
        {
            return WorldPosition::FromDoubleMeters(glm::dvec3(origin.Meters()) + offset);
        }

        struct Rig
        {
            glm::quat mCurrentWorldRotation{1.0f, 0.0f, 0.0f, 0.0f};
            glm::quat mParentRotation{1.0f, 0.0f, 0.0f, 0.0f};
            glm::vec3 mScale{1.0f};
            WorldPosition mOrigin;
            WorldPosition mParentPosition;
            glm::mat3 mParentLinear{1.0f};
            SnapToTerrainFacts mFacts;
        };

        bool ValidParentRotation(const glm::mat3 &linear, std::string &reason, glm::quat &rotation)
        {
            std::string detail;
            const std::optional<glm::quat> parent = TrySnapParentRotation(linear, &detail);
            if (!parent)
            {
                reason = "The parent's transform cannot carry a world rotation: " + detail;
                return false;
            }
            rotation = *parent;
            return true;
        }

        bool ReadRig(World &world, Entity entity, Rig &rig, std::string &reason)
        {
            const Transform *transform = world.GetComponent<Transform>(entity);
            if (transform == nullptr)
            {
                reason = "The entity has no Transform.";
                return false;
            }
            if (!Finite(transform->Rotation()) || !Finite(transform->Scale()) ||
                std::abs(transform->Scale().x) < kMinScale || std::abs(transform->Scale().y) < kMinScale ||
                std::abs(transform->Scale().z) < kMinScale)
            {
                reason = "The entity's rotation or scale is not finite and nonzero.";
                return false;
            }

            const Entity parent = ParentOf(world, entity);
            glm::quat parentRotation{1.0f, 0.0f, 0.0f, 0.0f};
            if (!parent.IsNull())
            {
                if (!world.IsAlive(parent) || world.GetComponent<Transform>(parent) == nullptr)
                {
                    reason = "The entity's parent is not available.";
                    return false;
                }
                const WorldPose parentPose = ResolveWorld(world, parent);
                if (!ValidParentRotation(parentPose.linear, reason, parentRotation))
                    return false;
                rig.mParentPosition = parentPose.position;
                rig.mParentLinear = parentPose.linear;
            }
            rig.mParentRotation = parentRotation;

            const glm::quat localRotation = glm::normalize(transform->Rotation());
            rig.mCurrentWorldRotation = glm::normalize(parentRotation * localRotation);
            if (!Finite(rig.mCurrentWorldRotation) || glm::length(rig.mCurrentWorldRotation) < 0.5f)
            {
                reason = "The entity's world rotation is not finite.";
                return false;
            }
            float parentScale = 1.0f;
            if (!parent.IsNull())
                parentScale = glm::length(glm::vec3(rig.mParentLinear[0]));
            rig.mScale = transform->Scale() * parentScale;
            rig.mOrigin = ResolveWorld(world, entity).position;
            if (!Finite(glm::dvec3(rig.mOrigin.Meters())))
            {
                reason = "The entity is not at a finite world position.";
                return false;
            }

            if (const MeshBounds *bounds = world.GetComponent<MeshBounds>(entity))
            {
                const glm::dvec3 min(bounds->mLocalMin);
                const glm::dvec3 max(bounds->mLocalMax);
                if (!Finite(min) || !Finite(max) || min.x > max.x || min.y > max.y || min.z > max.z)
                {
                    reason = "The entity's mesh bounds are invalid.";
                    return false;
                }
                rig.mFacts.mHasBounds = true;
                rig.mFacts.mLocalMin = min;
                rig.mFacts.mLocalMax = max;
            }
            return true;
        }
    }

    std::optional<SnapToTerrainPlane> FitSnapToTerrainPlane(std::span<const glm::dvec3> points)
    {
        if (points.size() < 3)
            return std::nullopt;

        glm::dvec3 centroid(0.0);
        for (const glm::dvec3 &point : points)
            centroid += point;
        centroid /= static_cast<double>(points.size());
        if (!Finite(centroid))
            return std::nullopt;

        double sxx = 0.0, sxz = 0.0, szz = 0.0, sxy = 0.0, szy = 0.0;
        for (const glm::dvec3 &point : points)
        {
            const glm::dvec3 d = point - centroid;
            sxx += d.x * d.x;
            sxz += d.x * d.z;
            szz += d.z * d.z;
            sxy += d.x * d.y;
            szy += d.z * d.y;
        }

        const double spread = std::max(sxx, szz);
        const double determinant = sxx * szz - sxz * sxz;
        if (!(spread > 1e-12) || determinant <= 1e-12 * spread * spread)
            return std::nullopt;

        const double a = (sxy * szz - szy * sxz) / determinant;
        const double b = (szy * sxx - sxy * sxz) / determinant;
        glm::dvec3 normal(-a, 1.0, -b);
        const double length = glm::length(normal);
        if (!(length > 0.0) || !std::isfinite(length))
            return std::nullopt;
        normal /= length;

        double residual = 0.0;
        for (const glm::dvec3 &point : points)
            residual = std::max(residual, std::abs(glm::dot(point - centroid, normal)));
        if (!std::isfinite(residual))
            return std::nullopt;

        return SnapToTerrainPlane{centroid, normal, residual};
    }

    std::optional<glm::quat> SnapToTerrainWorldRotation(const glm::quat &currentWorldRotation,
                                                        const glm::dvec3 &normal)
    {
        const double length = glm::length(normal);
        if (!(length > 0.0) || !std::isfinite(length) || !Finite(currentWorldRotation))
            return std::nullopt;

        const glm::dvec3 up = normal / length;
        if (up.y <= kVerticalNormal)
            return std::nullopt;

        glm::vec3 forward = currentWorldRotation * glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 heading(forward.x, 0.0f, forward.z);
        if (glm::dot(heading, heading) < kHeadingEpsilon)
        {
            forward = currentWorldRotation * glm::vec3(1.0f, 0.0f, 0.0f);
            heading = {forward.x, 0.0f, forward.z};
        }
        if (glm::dot(heading, heading) < kHeadingEpsilon)
            heading = {0.0f, 0.0f, 1.0f};

        // Keep XZ parallel to the old heading, solving only Y to lie in the terrain plane.
        // Rotating a yaw by the shortest up-to-normal tilt would also rotate its XZ heading.
        glm::dvec3 alignedForward(heading.x,
            -(up.x * heading.x + up.z * heading.z) / up.y, heading.z);
        alignedForward = glm::normalize(alignedForward);
        const glm::dvec3 right = glm::normalize(glm::cross(up, alignedForward));
        const glm::mat3 basis{glm::vec3(right), glm::vec3(up), glm::vec3(alignedForward)};
        const glm::quat result = glm::normalize(glm::quat_cast(basis));
        if (!Finite(result))
            return std::nullopt;
        return result;
    }

    std::optional<glm::quat> TrySnapParentRotation(const glm::mat3 &linear, std::string *reason)
    {
        const glm::vec3 x(linear[0]);
        const glm::vec3 y(linear[1]);
        const glm::vec3 z(linear[2]);
        const float sx = glm::length(x);
        const float sy = glm::length(y);
        const float sz = glm::length(z);
        const float minScale = std::min({sx, sy, sz});
        const float maxScale = std::max({sx, sy, sz});
        if (!Finite(x) || !Finite(y) || !Finite(z) || !(minScale > 1e-6f))
        {
            if (reason)
                *reason = "it is singular.";
            return std::nullopt;
        }
        if (maxScale - minScale > 1e-4f * maxScale)
        {
            if (reason)
                *reason = "its scale is not uniform.";
            return std::nullopt;
        }
        const float shear = std::max({std::abs(glm::dot(x, y)), std::abs(glm::dot(x, z)),
                                      std::abs(glm::dot(y, z))});
        if (shear > 1e-4f * minScale * minScale)
        {
            if (reason)
                *reason = "it is sheared.";
            return std::nullopt;
        }
        if (!(glm::determinant(linear) > 0.0f))
        {
            if (reason)
                *reason = "it is mirrored.";
            return std::nullopt;
        }

        const glm::mat3 rotation(x / sx, y / sy, z / sz);
        const glm::quat result = glm::normalize(glm::quat_cast(rotation));
        if (!Finite(result))
        {
            if (reason)
                *reason = "its rotation is not finite.";
            return std::nullopt;
        }
        return result;
    }

    std::vector<glm::dvec3> SnapToTerrainFootprint(WorldPosition origin, const glm::mat3 &linear,
                                                   const SnapToTerrainFacts &facts, double sourceSpacing)
    {
        std::vector<glm::dvec3> points;
        const glm::dvec3 worldOrigin(origin.Meters());
        const auto toWorld = [&](const glm::dvec3 &local)
        {
            return worldOrigin + glm::dvec3(linear * glm::vec3(local));
        };

        if (!facts.mHasBounds)
        {
            if (!std::isfinite(sourceSpacing) || sourceSpacing <= 0.0)
                return points;
            points.push_back(worldOrigin);
            points.push_back(worldOrigin + glm::dvec3(sourceSpacing, 0.0, 0.0));
            points.push_back(worldOrigin - glm::dvec3(sourceSpacing, 0.0, 0.0));
            points.push_back(worldOrigin + glm::dvec3(0.0, 0.0, sourceSpacing));
            points.push_back(worldOrigin - glm::dvec3(0.0, 0.0, sourceSpacing));
            return points;
        }

        const glm::dvec3 min = facts.mLocalMin;
        const glm::dvec3 max = facts.mLocalMax;
        const double bottomY = BottomY(facts, linear);
        const glm::dvec3 centre = BottomCenter(facts, linear);
        const std::array<glm::dvec3, 4> corners{
            glm::dvec3(min.x, bottomY, min.z), glm::dvec3(max.x, bottomY, min.z),
            glm::dvec3(max.x, bottomY, max.z), glm::dvec3(min.x, bottomY, max.z)};
        const std::array<glm::dvec3, 4> worldCorners{toWorld(corners[0]), toWorld(corners[1]), toWorld(corners[2]),
                                                     toWorld(corners[3])};
        double minX = worldCorners[0].x, maxX = worldCorners[0].x, minZ = worldCorners[0].z,
               maxZ = worldCorners[0].z;
        for (const glm::dvec3 &corner : worldCorners)
        {
            minX = std::min(minX, corner.x);
            maxX = std::max(maxX, corner.x);
            minZ = std::min(minZ, corner.z);
            maxZ = std::max(maxZ, corner.z);
        }
        const double span = std::max(maxX - minX, maxZ - minZ);

        points.push_back(toWorld(centre));
        if (std::isfinite(sourceSpacing) && sourceSpacing > 0.0 && span > 4.0 * sourceSpacing)
        {
            const double midX = (min.x + max.x) * 0.5;
            const double midZ = (min.z + max.z) * 0.5;
            for (const double z : {min.z, midZ, max.z})
                for (const double x : {min.x, midX, max.x})
                {
                    if (x == midX && z == midZ)
                        continue;
                    points.push_back(toWorld(glm::dvec3(x, bottomY, z)));
                }
            return points;
        }

        for (const glm::dvec3 &corner : corners)
            points.push_back(toWorld(corner));
        return points;
    }

    SnapToTerrainAction SnapToTerrainActionFor(World &world, DocumentId document, const GroundQuery *ground)
    {
        SnapToTerrainAction action;
        if (document != kSceneDocument)
        {
            action.mReason = "Prefab pages have no scene terrain; snap the instance from the Scene tab.";
            return action;
        }

        const EngineModeState *mode = world.TryResource<EngineModeState>();
        if (mode != nullptr && mode->mMode != EngineMode::Edit)
        {
            action.mReason = "Snap to Terrain is available in Edit mode only.";
            return action;
        }
        if (ground == nullptr || !ground->Bound())
        {
            action.mReason = "The scene has no terrain to snap to.";
            return action;
        }

        const Selection *selection = world.TryResource<Selection>();
        if (selection == nullptr || selection->mSelected.size() != 1)
        {
            action.mReason = "Select one entity.";
            return action;
        }
        const Entity entity = selection->mSelected.front();
        if (!world.IsAlive(entity) || world.GetComponent<Transform>(entity) == nullptr)
        {
            action.mReason = "Select one entity.";
            return action;
        }

        Rig rig;
        std::string reason;
        if (!ReadRig(world, entity, rig, reason))
        {
            action.mReason = std::move(reason);
            return action;
        }

        const glm::mat3 linear = Linear(rig.mCurrentWorldRotation, rig.mScale);
        const glm::dvec3 centre = glm::dvec3(rig.mOrigin.Meters()) +
                                  glm::dvec3(linear * glm::vec3(BottomCenter(rig.mFacts, linear)));
        const std::optional<WorldPosition> centrePosition = WorldPosition::FromDoubleMeters(centre);
        if (!centrePosition)
        {
            action.mReason = "The selection's footprint is outside the world.";
            return action;
        }

        const GroundQueryCoverage coverage = ground->Probe(*centrePosition);
        if (!coverage.mCovered)
        {
            action.mReason = coverage.mDiagnostic.empty() ? "No terrain covers the selection." : coverage.mDiagnostic;
            return action;
        }
        if (!coverage.mSourceResident)
            action.mStatus = coverage.mDiagnostic;

        action.mEnabled = true;
        return action;
    }

    SnapToTerrainAction SnapToTerrainSession::Begin(World &world, DocumentId document, GroundQuery &ground)
    {
        SnapToTerrainAction action = SnapToTerrainActionFor(world, document, &ground);
        if (!action.mEnabled)
        {
            Cancel(action.mReason);
            return action;
        }

        const Selection &selection = world.Resource<Selection>();
        const Entity entity = selection.mSelected.front();

        mState = State::Idle;
        mEdit = {};
        mMessage.clear();
        mElapsed = 0.0f;
        mRound = 0;
        mGroundIdentity = 0;
        mBindings = {};

        Rig rig;
        std::string reason;
        if (!ReadRig(world, entity, rig, reason))
        {
            Fail(std::move(reason));
            return action;
        }

        mBindings.mWorld = &world;
        mBindings.mDocument = document;
        mBindings.mEntity = entity;
        mBindings.mParent = ParentOf(world, entity);
        mBindings.mTransformVersion = world.GetComponent<Transform>(entity)->Version();
        if (!mBindings.mParent.IsNull())
        {
            mBindings.mHasParent = true;
            if (const Transform *parent = world.GetComponent<Transform>(mBindings.mParent))
                mBindings.mParentTransformVersion = parent->Version();
            if (const WorldTransform *parent = world.GetComponent<WorldTransform>(mBindings.mParent))
                mBindings.mParentWorldVersion = parent->Version();
        }
        mBindings.mGroundRevision = ground.Revision();
        mBindings.mFacts = rig.mFacts;
        mBindings.mOrigin = rig.mOrigin;
        mBindings.mCurrentWorldRotation = rig.mCurrentWorldRotation;
        mBindings.mParentRotation = rig.mParentRotation;
        mBindings.mScale = rig.mScale;
        mBindings.mParentPosition = rig.mParentPosition;
        mBindings.mParentLinear = rig.mParentLinear;

        mState = State::Waiting;
        Advance(ground);
        return action;
    }

    void SnapToTerrainSession::Cancel(std::string reason)
    {
        if (mState != State::Waiting && mState != State::Ready)
            return;
        Fail(std::move(reason));
    }

    void SnapToTerrainSession::Update(World &world, DocumentId document, GroundQuery &ground, float dt, bool editing)
    {
        if (mState != State::Waiting)
            return;
        if (!StillBound(world, document, ground, editing))
            return;

        mElapsed += dt;
        if (mElapsed > kTimeoutSeconds)
        {
            Fail("Snap to Terrain timed out waiting for the terrain source data.");
            return;
        }
        Advance(ground);
    }

    bool SnapToTerrainSession::StillBound(const World &world, DocumentId document, const GroundQuery &ground,
                                          bool editing)
    {
        if (mBindings.mWorld != &world || document != mBindings.mDocument)
        {
            Fail("The active document changed.");
            return false;
        }
        if (!editing)
        {
            Fail("Play started.");
            return false;
        }
        if (!ground.Bound() || ground.Revision() != mBindings.mGroundRevision)
        {
            Fail("The terrain changed.");
            return false;
        }

        const Selection *selection = world.TryResource<Selection>();
        if (selection == nullptr || selection->mSelected.size() != 1 ||
            selection->mSelected.front() != mBindings.mEntity)
        {
            Fail("The selection changed.");
            return false;
        }
        if (!world.IsAlive(mBindings.mEntity))
        {
            Fail("The entity was deleted.");
            return false;
        }
        const Transform *transform = world.GetComponent<Transform>(mBindings.mEntity);
        if (transform == nullptr || transform->Version() != mBindings.mTransformVersion)
        {
            Fail("The entity's Transform changed.");
            return false;
        }
        if (ParentOf(world, mBindings.mEntity) != mBindings.mParent)
        {
            Fail("The entity's parent changed.");
            return false;
        }
        if (mBindings.mHasParent)
        {
            if (!world.IsAlive(mBindings.mParent))
            {
                Fail("The entity's parent was deleted.");
                return false;
            }
            const Transform *parentTransform = world.GetComponent<Transform>(mBindings.mParent);
            if (parentTransform == nullptr || parentTransform->Version() != mBindings.mParentTransformVersion)
            {
                Fail("The entity's parent Transform changed.");
                return false;
            }
            if (const WorldTransform *parent = world.GetComponent<WorldTransform>(mBindings.mParent);
                parent != nullptr && parent->Version() != mBindings.mParentWorldVersion)
            {
                Fail("The entity's parent pose changed.");
                return false;
            }
        }
        return true;
    }

    SnapToTerrainSession::Sampled SnapToTerrainSession::Sample(WorldPosition origin, const glm::mat3 &linear,
                                                               const GroundQuery &ground) const
    {
        Sampled sampled;
        const glm::dvec3 bottomCentre = BottomCenter(mBindings.mFacts, linear);
        const glm::dvec3 centreD = glm::dvec3(origin.Meters()) + glm::dvec3(linear * glm::vec3(bottomCentre));
        const std::optional<WorldPosition> centre = WorldPosition::FromDoubleMeters(centreD);
        if (!centre)
        {
            sampled.mState = Sampled::State::Failed;
            sampled.mMessage = "The footprint centre is outside the world.";
            return sampled;
        }

        GroundQueryResult first = ground.Sample(*centre);
        if (first.status == GroundQueryStatus::Failed)
        {
            sampled.mState = Sampled::State::Failed;
            sampled.mMessage = first.diagnostic.empty() ? "The terrain height query failed." : first.diagnostic;
            return sampled;
        }
        if (first.status == GroundQueryStatus::OutOfBounds)
        {
            sampled.mState = Sampled::State::Failed;
            sampled.mMessage = "No terrain covers the selection's footprint.";
            return sampled;
        }

        sampled.mSourceSpacing = first.sourceSpacing;
        sampled.mIdentity = first.identity;
        if (!std::isfinite(first.sourceSpacing) || first.sourceSpacing <= 0.0)
        {
            sampled.mState = Sampled::State::Failed;
            sampled.mMessage = "The terrain did not report a source spacing.";
            return sampled;
        }

        const std::vector<glm::dvec3> points =
            SnapToTerrainFootprint(origin, linear, mBindings.mFacts, first.sourceSpacing);
        bool loading = first.status == GroundQueryStatus::Loading;
        if (first.IsReady())
            sampled.mSurface.push_back(glm::dvec3(first.surface->Meters()));

        for (std::size_t i = 1; i < points.size(); ++i)
        {
            const std::optional<WorldPosition> point = WorldPosition::FromDoubleMeters(points[i]);
            if (!point)
            {
                sampled.mState = Sampled::State::Failed;
                sampled.mMessage = "A footprint sample is outside the world.";
                return sampled;
            }
            const GroundQueryResult result = ground.Sample(*point);
            if (result.status == GroundQueryStatus::Failed)
            {
                sampled.mState = Sampled::State::Failed;
                sampled.mMessage =
                    result.diagnostic.empty() ? "The terrain height query failed." : result.diagnostic;
                return sampled;
            }
            if (result.status == GroundQueryStatus::OutOfBounds)
            {
                sampled.mState = Sampled::State::Failed;
                sampled.mMessage = "No terrain covers the selection's footprint.";
                return sampled;
            }
            if (result.identity != 0 && first.identity != 0 && result.identity != first.identity)
            {
                sampled.mState = Sampled::State::Failed;
                sampled.mMessage = "The footprint spans more than one terrain release.";
                return sampled;
            }
            if (result.status == GroundQueryStatus::Loading)
                loading = true;
            else if (result.IsReady())
                sampled.mSurface.push_back(glm::dvec3(result.surface->Meters()));
        }

        sampled.mState = loading ? Sampled::State::Loading : Sampled::State::Ready;
        if (loading)
            sampled.mMessage = "Waiting for the terrain source data.";
        return sampled;
    }

    void SnapToTerrainSession::Advance(GroundQuery &ground)
    {
        if (mRound == 0)
        {
            const glm::mat3 linear = Linear(mBindings.mCurrentWorldRotation, mBindings.mScale);
            const Sampled first = Sample(mBindings.mOrigin, linear, ground);
            if (first.mState == Sampled::State::Loading)
            {
                mMessage = first.mMessage;
                return;
            }
            if (first.mState == Sampled::State::Failed)
            {
                Fail(first.mMessage);
                return;
            }

            const std::optional<SnapToTerrainPlane> plane = FitSnapToTerrainPlane(first.mSurface);
            if (!plane)
            {
                Fail("The sampled terrain is too uneven or too small to fit a plane.");
                return;
            }
            const std::optional<glm::quat> rotation =
                SnapToTerrainWorldRotation(mBindings.mCurrentWorldRotation, plane->mNormal);
            if (!rotation)
            {
                Fail("The terrain normal cannot carry the current heading.");
                return;
            }

            const glm::mat3 snappedLinear = Linear(*rotation, mBindings.mScale);
            const glm::dvec3 offset =
                glm::dvec3(snappedLinear * glm::vec3(BottomCenter(mBindings.mFacts, snappedLinear)));
            const std::optional<WorldPosition> candidate =
                WorldPosition::FromDoubleMeters(plane->mPoint - offset);
            if (!candidate)
            {
                Fail("The snapped position is outside the world.");
                return;
            }

            mCandidatePosition = glm::dvec3(candidate->Meters());
            mCandidateRotation = *rotation;
            mGroundIdentity = first.mIdentity;
            mEdit.mSourceSpacing = first.mSourceSpacing;
            ++mRound;
        }

        if (mRound == 1)
        {
            const glm::mat3 linear = Linear(mCandidateRotation, mBindings.mScale);
            const std::optional<WorldPosition> candidate = WorldPosition::FromDoubleMeters(mCandidatePosition);
            if (!candidate)
            {
                Fail("The snapped position is outside the world.");
                return;
            }

            const Sampled second = Sample(*candidate, linear, ground);
            if (second.mState == Sampled::State::Loading)
            {
                mMessage = second.mMessage;
                return;
            }
            if (second.mState == Sampled::State::Failed)
            {
                Fail(second.mMessage);
                return;
            }
            if (mGroundIdentity != 0 && second.mIdentity != 0 && second.mIdentity != mGroundIdentity)
            {
                Fail("The terrain release changed while snapping.");
                return;
            }

            const std::optional<SnapToTerrainPlane> plane = FitSnapToTerrainPlane(second.mSurface);
            if (!plane)
            {
                Fail("The sampled terrain is too uneven or too small to fit a plane.");
                return;
            }
            const std::optional<glm::quat> rotation =
                SnapToTerrainWorldRotation(mBindings.mCurrentWorldRotation, plane->mNormal);
            if (!rotation)
            {
                Fail("The terrain normal cannot carry the current heading.");
                return;
            }

            const glm::mat3 snappedLinear = Linear(*rotation, mBindings.mScale);
            const glm::dvec3 offset =
                glm::dvec3(snappedLinear * glm::vec3(BottomCenter(mBindings.mFacts, snappedLinear)));
            const std::optional<WorldPosition> worldPosition =
                WorldPosition::FromDoubleMeters(plane->mPoint - offset);
            if (!worldPosition)
            {
                Fail("The snapped position is outside the world.");
                return;
            }

            if (mBindings.mHasParent)
            {
                const std::optional<ParentLocalTranslation> local =
                    TryWorldToParentLocal(WorldPose{mBindings.mParentPosition, mBindings.mParentLinear},
                                          *worldPosition);
                if (!local)
                {
                    Fail("The snapped position cannot be expressed in the parent's space.");
                    return;
                }
                mEdit.mPosition = local->Meters();
                mEdit.mRotation = glm::normalize(glm::inverse(mBindings.mParentRotation) * *rotation);
            }
            else
            {
                mEdit.mPosition = worldPosition->Meters();
                mEdit.mRotation = *rotation;
            }

            if (!Finite(mEdit.mRotation) || !std::isfinite(mEdit.mPosition.x) || !std::isfinite(mEdit.mPosition.y) ||
                !std::isfinite(mEdit.mPosition.z))
            {
                Fail("The snapped pose is not finite.");
                return;
            }

            mEdit.mSamples = second.mSurface.size();
            mEdit.mResidual = plane->mResidual;
            mEdit.mSourceSpacing = second.mSourceSpacing;
            mState = State::Ready;
            mMessage = "The selection is snapped to the terrain.";
        }
    }

    std::optional<SnapToTerrainEdit> SnapToTerrainSession::TakeEdit(World &world, DocumentId document,
                                                                    const GroundQuery &ground, bool editing,
                                                                    const SnapToTerrainCommit &commit)
    {
        if (mState != State::Ready)
            return std::nullopt;
        if (commit.mTransitionRequested)
        {
            Fail("The scene changed before the snap committed.");
            return std::nullopt;
        }
        if (commit.mFrameSubmissions != commit.mCurrentSubmissions)
        {
            Fail("Another edit was submitted before the snap committed.");
            return std::nullopt;
        }
        if (!StillBound(world, document, ground, editing))
            return std::nullopt;

        const SnapToTerrainEdit edit = mEdit;
        mState = State::Idle;
        mMessage.clear();
        return edit;
    }

    std::string SnapToTerrainSession::TakeFailure()
    {
        if (mState != State::Failed)
            return {};
        std::string failure = std::move(mMessage);
        mState = State::Idle;
        mMessage.clear();
        return failure;
    }

    void SnapToTerrainSession::Fail(std::string reason)
    {
        mState = State::Failed;
        mMessage = Bound(reason.empty() ? "Snap to Terrain failed." : std::move(reason));
        mEdit = {};
    }

    std::unique_ptr<ICommand> MakeSnapToTerrainCommand(Entity entity, const SnapToTerrainEdit &edit,
                                                       IAuthoredEntities *authored)
    {
        const ComponentOps *ops = ComponentRegistry::Instance().Find("Transform");
        if (ops == nullptr)
            return nullptr;
        const FieldDesc *position = ops->FindField("position");
        const FieldDesc *rotation = ops->FindField("rotation");
        if (position == nullptr || rotation == nullptr)
            return nullptr;

        std::vector<FieldValue> values(2);
        values[0].mField = static_cast<uint32_t>(position - ops->mFields.data());
        std::memcpy(values[0].mBytes, &edit.mPosition, sizeof(WorldVec3));
        values[1].mField = static_cast<uint32_t>(rotation - ops->mFields.data());
        std::memcpy(values[1].mBytes, &edit.mRotation, sizeof(glm::quat));
        return MakeSetFieldsEditCommand(entity, *ops, std::move(values), "Snap to Terrain", authored);
    }
}
