/**
 * @file TransformHierarchy.cpp
 * @author Sumin Park
 * @brief Resolving Transform through the scene graph into WorldTransform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/ecs/TransformHierarchy.h"

#include "core/ecs/CommandBuffer.h"
#include "core/ecs/HierarchyMutator.h"
#include "core/ecs/Query.h"
#include "core/ecs/World.h"
#include "core/log/Assert.h"
#include "core/log/Log.h"

#include <algorithm>
#include <optional>

namespace mir
{
    namespace detail
    {
        // Only thing allowed to write WorldTransform's private members
        struct TransformResolver
        {
            static void Store(WorldTransform &target,
                const WorldPose &pose,
                uint32_t localVersion,
                uint32_t parentVersion)
            {
                target.mPose = pose;
                target.mLocalVersion = localVersion;
                target.mParentVersion = parentVersion;
                target.mDirty = false;

                if (++target.mVersion == 0)
                    target.mVersion = 1;
            }

            static bool IsClean(const WorldTransform &target, uint32_t localVersion, uint32_t parentVersion)
            {
                return !target.mDirty &&
                    target.mLocalVersion == localVersion &&
                    target.mParentVersion == parentVersion;
            }

            static void Invalidate(WorldTransform &target) { target.mDirty = true; }
        };
    }

    namespace
    {
        Entity LiveParentOf(World &world, Entity entity)
        {
            const Entity parent = world.HierarchyIndex().ParentOf(entity);
            return world.IsAlive(parent) ? parent : kNullEntity;
        }

        // force recalculation
        void InvalidateCache(World &world, Entity entity)
        {
            if (WorldTransform *cache = world.GetComponent<WorldTransform>(entity))
                detail::TransformResolver::Invalidate(*cache);
        }

        // A root composes under the identity pose at the origin, which is exact
        // in either precision, so one path serves roots and children.
        WorldPose Compose(const WorldPose &parent, const Transform &local, Entity entity)
        {
            if (const std::optional<WorldPose> pose = TryComposePose(parent, local.LinearMatrix(), local.Position()))
                return *pose;

            MIR_LOG_WARN("ResolveWorld: entity {} leaves the finite world; kept at its parent's position",
                entity.mIndex);
            return WorldPose{ parent.position, parent.linear * local.LinearMatrix() };
        }

        constexpr float kMinParentAxis = 1e-12f;

        // The local values that hold `child`'s world pose under `parent`. A zero-scale
        // parent counts as translation only. nullopt when the position does not fit.
        std::optional<Transform> TryKeepWorld(World &world, Entity child, Entity parent)
        {
            const WorldPose childPose = ResolveWorld(world, child);
            WorldPose parentPose = parent.IsNull() ? WorldPose{} : ResolveWorld(world, parent);

            for (int axis = 0; axis < 3; ++axis)
            {
                if (glm::length(parentPose.linear[axis]) < kMinParentAxis)
                    parentPose.linear = glm::mat3(1.0f);
            }

            const std::optional<ParentLocalTranslation> position = TryWorldToParentLocal(parentPose, childPose.position);
            if (!position)
                return std::nullopt;

            glm::quat rotation{};
            glm::vec3 scale{};
            DecomposeRotationScale(glm::inverse(parentPose.linear) * childPose.linear, rotation, scale);
            return Transform(position->Meters(), rotation, scale);
        }
    }

    Entity ParentOf(const World &world, Entity entity)
    {
        return world.HierarchyIndex().ParentOf(entity);
    }

    bool IsAncestorOf(const World &world, Entity ancestor, Entity entity)
    {
        if (ancestor.IsNull() || entity.IsNull())
            return false;

        if (ancestor == entity)
            return true;

        return world.HierarchyIndex().IsAncestorOf(ancestor, entity);
    }

    WorldPose ResolveWorld(World &world, Entity entity)
    {
        // if dead, identity
        if (!world.IsAlive(entity))
            return WorldPose{};

        Entity chain[kMaxHierarchyDepth];
        uint32_t depth = 0;

        for (Entity cursor = entity; !cursor.IsNull(); cursor = LiveParentOf(world, cursor))
        {
            MIR_ASSERT(depth < kMaxHierarchyDepth,
                "ResolveWorld: hierarchy deeper than {} - the graph should have refused this",
                kMaxHierarchyDepth);

            if (depth >= kMaxHierarchyDepth)
                break;

            chain[depth++] = cursor;
        }

        WorldPose accumulated;

        uint32_t parentVersion = 0;

        // False once an ancestor has no WorldTransform to stamp.
        bool cacheable = true;

        // Now back down, root first.
        for (uint32_t i = depth; i-- > 0;)
        {
            const Entity current = chain[i];
            const Transform *local = world.GetComponent<Transform>(current);
            WorldTransform *cache = world.GetComponent<WorldTransform>(current);

            const uint32_t localVersion = local ? local->Version() : 0;

            if (cache == nullptr)
            {
                accumulated = local ? Compose(accumulated, *local, current) : accumulated;
                cacheable = false;
                continue;
            }

            if (cacheable && detail::TransformResolver::IsClean(*cache, localVersion, parentVersion))
            {
                accumulated = cache->Pose();
                parentVersion = cache->Version();
                continue;
            }

            accumulated = local ? Compose(accumulated, *local, current) : accumulated;
            detail::TransformResolver::Store(*cache, accumulated, localVersion, parentVersion);
            parentVersion = cache->Version();
            cacheable = true;
        }

        return accumulated;
    }

    bool SetParent(World &world, Entity child, Entity parent, Entity before, bool keepWorld)
    {
        if (!world.IsAlive(child))
        {
            MIR_LOG_WARN("SetParent: child is not alive");
            return false;
        }

        if (!parent.IsNull() && !world.IsAlive(parent))
        {
            MIR_LOG_WARN("SetParent: parent is not alive");
            return false;
        }

        // its destroy has already cut it loose; a new link could put it under a parent that is draining
        const bool destroying = world.IsDestroying(child);
        MIR_ASSERT(!destroying, "SetParent: child is being destroyed");
        if (destroying)
            return false;

        HierarchyIndex &index = world.HierarchyIndex();

        // a reorder under the same parent keeps local values, so it never round-trips them
        std::optional<Transform> kept;
        if (keepWorld && index.ParentOf(child) != parent && world.Has<Transform>(child))
        {
            kept = TryKeepWorld(world, child, parent);
            if (!kept)
            {
                MIR_LOG_WARN("SetParent: refused - the child's world position does not fit under the new parent");
                return false;
            }
        }

        if (!detail::HierarchyMutator::SetParent(index, child, parent, before))
        {
            MIR_LOG_WARN("SetParent: refused - self-parenting, a cycle, deeper than "
                "kMaxHierarchyDepth ({}), or `before` is not a sibling under the parent",
                kMaxHierarchyDepth);
            return false;
        }

        if (kept)
        {
            // setters, not assignment, so the version bumps
            Transform &local = *world.GetComponent<Transform>(child);
            local.SetPosition(kept->Position());
            local.SetRotation(kept->Rotation());
            local.SetScale(kept->Scale());
        }

        InvalidateCache(world, child);
        return true;
    }

    bool CanSetParent(const World &world, Entity child, Entity parent, Entity before)
    {
        if (!world.IsAlive(child) || (!parent.IsNull() && !world.IsAlive(parent)) || world.IsDestroying(child))
            return false;

        return world.HierarchyIndex().CanSetParent(child, parent, before);
    }

    std::vector<Entity> TopmostInHierarchyOrder(const World &world, std::span<const Entity> entities)
    {
        std::vector<Entity> ordered;
        const HierarchyIndex &index = world.HierarchyIndex();

        const auto listed = [&entities](Entity entity)
            {
                return std::find(entities.begin(), entities.end(), entity) != entities.end();
            };

// a listed entity's subtree is skipped, which drops its listed descendants
        std::vector<Entity> pending(index.Roots().rbegin(), index.Roots().rend());
        while (!pending.empty())
        {
            const Entity entity = pending.back();
            pending.pop_back();

            if (listed(entity))
            {
                ordered.push_back(entity);
                continue;
            }

            const std::span<const Entity> children = index.ChildrenOf(entity);
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
        return ordered;
    }

    bool CanSetParentAll(const World &world, std::span<const Entity> entities, Entity parent, Entity before)
    {
        const std::vector<Entity> moved = TopmostInHierarchyOrder(world, entities);
        return !moved.empty() && std::all_of(moved.begin(), moved.end(), [&](Entity entity)
            {
                return CanSetParent(world, entity, parent, before);
            });
    }

    bool MoveToLastSibling(World &world, Entity entity)
    {
        if (!world.IsAlive(entity))
        {
            MIR_LOG_WARN("MoveToLastSibling: entity is not alive");
            return false;
        }

        return detail::HierarchyMutator::MoveToLastSibling(world.HierarchyIndex(), entity);
    }

    Transform &AddTransform(World &world, Entity entity, const Transform &transform, Entity parent)
    {
        if (world.Has<Transform>(entity))
            *world.GetComponent<Transform>(entity) = transform;
        else
            world.AddComponent<Transform>(entity, transform);

        if (!world.Has<WorldTransform>(entity))
            world.AddComponent<WorldTransform>(entity, WorldTransform{});

        if (!parent.IsNull())
            SetParent(world, entity, parent, kNullEntity, false);

        return *world.GetComponent<Transform>(entity);
    }

    void TransformPropagateSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, WorldTransform>();
    }

    void TransformPropagateSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;
        mQuery->ForEach([&world](Entity entity, Transform &, WorldTransform &)
            {
                ResolveWorld(world, entity);
            });
    }
}
