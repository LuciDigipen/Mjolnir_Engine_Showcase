/**
 * @file SceneDuplicate.cpp
 * @author Jackey Cen
 * @brief Duplicate scene entities, keeping a Prefab instance linked or copying a member as ordinary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneDuplicate.h"

#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/HierarchyIndex.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "scene/EntitySnapshot.h"
#include "scene/SceneIO.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        std::optional<StableId> StableIdOf(const LoadedScene &scene, Entity entity)
        {
            for (const auto &[id, candidate] : scene.mEntities)
            {
                if (candidate == entity)
                    return id;
            }
            return std::nullopt;
        }

        const ScenePrefabInstance *FindInstanceBinding(const LoadedScene &scene, StableId member,
            StableId &outRoot)
        {
            for (const auto &[root, instance] : scene.mInstances)
            {
                for (const auto &[address, id] : instance.mBindings)
                {
                    (void)address;
                    if (id != member)
                        continue;
                    outRoot = root;
                    return &instance;
                }
            }
            return nullptr;
        }

        Entity NextSiblingOf(World &world, Entity entity)
        {
            const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr)
                return kNullEntity;

            const std::span<const Entity> siblings = index->SiblingsOf(entity);
            const auto it = std::find(siblings.begin(), siblings.end(), entity);
            if (it == siblings.end() || std::next(it) == siblings.end())
                return kNullEntity;
            return *std::next(it);
        }

        uint32_t SceneSiblingIndex(World &world, Entity entity, const LoadedScene &scene)
        {
            const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
            if (index == nullptr)
                return 0;

            std::unordered_set<uint64_t> sceneEntities;
            sceneEntities.reserve(scene.mEntities.size());
            for (const auto &[id, member] : scene.mEntities)
            {
                (void)id;
                sceneEntities.insert(PackEntity(member));
            }

            uint32_t position = 0;
            for (const Entity sibling : index->SiblingsOf(entity))
            {
                if (sibling == entity)
                    break;
                if (sceneEntities.contains(PackEntity(sibling)))
                    ++position;
            }
            return position;
        }

        bool IsEntityRefField(std::string_view component, std::string_view field)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(component);
            if (ops == nullptr)
                return false;

            const FieldDesc *desc = ops->FindField(field);
            return desc != nullptr && desc->mKind == FieldKind::EntityRef;
        }

        void RemapRefValue(json &value, const std::map<StableId, StableId> &remap)
        {
            std::optional<StableId> id;
            if (value.is_number_unsigned())
            {
                id = value.get<StableId>();
            }
            else if (value.is_number_integer())
            {
                const int64_t raw = value.get<int64_t>();
                if (raw >= 0)
                    id = static_cast<StableId>(raw);
            }

            if (!id)
                return;

            const auto it = remap.find(*id);
            if (it != remap.end())
                value = it->second;
        }

        void RemapPatchRefs(ScenePrefabInstance &record, const std::map<StableId, StableId> &remap)
        {
            for (SceneInstancePatch &patch : record.mPatches)
            {
                for (SceneFieldPatch &field : patch.mFields)
                {
                    if (IsEntityRefField(field.mComponent, field.mField))
                        RemapRefValue(field.mValue, remap);
                }

                for (SceneComponentRecord &component : patch.mAddedComponents)
                {
                    for (auto &[field, value] : component.mFields.items())
                    {
                        if (IsEntityRefField(component.mType, field))
                            RemapRefValue(value, remap);
                    }
                }
            }
        }

        Entity DuplicateLinkedInstance(World &world, LoadedScene &scene, Entity source,
            const ScenePrefabInstance &sourceRecord)
        {
            const std::vector<Entity> sourceMembers = SnapshotMembers(world, source);
            std::unordered_map<uint64_t, std::size_t> sourceIndex;
            sourceIndex.reserve(sourceMembers.size());
            for (std::size_t i = 0; i < sourceMembers.size(); ++i)
                sourceIndex[PackEntity(sourceMembers[i])] = i;

            const Entity copy = DuplicateSceneEntity(world, scene, source);
            if (copy.IsNull())
                return kNullEntity;

            const std::vector<Entity> copyMembers = SnapshotMembers(world, copy);
            if (copyMembers.size() != sourceMembers.size())
            {
                MIR_LOG_WARN("Duplicate: the copy has {} members but the instance has {}", copyMembers.size(),
                    sourceMembers.size());
                DestroySceneEntity(world, scene, copy);
                return kNullEntity;
            }

            std::map<StableId, StableId> remap;
            for (const auto &[address, id] : sourceRecord.mBindings)
            {
                (void)address;
                const auto entityIt = scene.mEntities.find(id);
                if (entityIt == scene.mEntities.end() || !world.IsAlive(entityIt->second))
                    continue;

                const auto indexIt = sourceIndex.find(PackEntity(entityIt->second));
                if (indexIt == sourceIndex.end())
                    continue;

                const std::optional<StableId> copyId = StableIdOf(scene, copyMembers[indexIt->second]);
                if (copyId)
                    remap.emplace(id, *copyId);
            }

            const std::optional<StableId> copyRoot = StableIdOf(scene, copy);
            if (!copyRoot)
            {
                DestroySceneEntity(world, scene, copy);
                return kNullEntity;
            }

            ScenePrefabInstance record = sourceRecord;
            record.mRoot = *copyRoot;
            record.mBindings.clear();
            for (const auto &[address, id] : sourceRecord.mBindings)
            {
                const auto remapped = remap.find(id);
                if (remapped != remap.end())
                    record.mBindings[address] = remapped->second;
            }

            if (record.mBindings.empty())
            {
                DestroySceneEntity(world, scene, copy);
                return kNullEntity;
            }

            RemapPatchRefs(record, remap);

            record.mParent = kNullStableId;
            const Entity parent = ParentOf(world, copy);
            if (!parent.IsNull())
            {
                const std::optional<StableId> parentId = StableIdOf(scene, parent);
                if (parentId)
                    record.mParent = *parentId;
            }
            record.mSiblingIndex = SceneSiblingIndex(world, copy, scene);
            record.mPlacement = InstancePlacementFromWorld(world, copy);

            scene.mInstances[*copyRoot] = std::move(record);
            scene.mDirty = true;
            return copy;
        }
    }

    Entity DuplicateSceneEntitySubtree(World &world, LoadedScene &scene, Entity source)
    {
        if (!world.IsAlive(source))
            return kNullEntity;

        const std::optional<StableId> sourceId = StableIdOf(scene, source);
        StableId instanceRoot = kNullStableId;
        const ScenePrefabInstance *instance =
            sourceId ? FindInstanceBinding(scene, *sourceId, instanceRoot) : nullptr;

        if (instance == nullptr)
            return DuplicateSceneEntity(world, scene, source);

        if (instanceRoot != *sourceId)
        {
            MIR_LOG_WARN("Duplicate: cannot duplicate a prefab instance member; copy it as an ordinary object");
            return kNullEntity;
        }

        return DuplicateLinkedInstance(world, scene, source, *instance);
    }

    Entity CopyInstanceMemberAsOrdinary(World &world, LoadedScene &scene, Entity source)
    {
        if (!world.IsAlive(source))
            return kNullEntity;

        const std::optional<StableId> sourceId = StableIdOf(scene, source);
        StableId instanceRoot = kNullStableId;
        if (!sourceId || FindInstanceBinding(scene, *sourceId, instanceRoot) == nullptr)
        {
            MIR_LOG_WARN("Copy as Ordinary Object: the entity is not a prefab instance member");
            return kNullEntity;
        }

        const auto rootIt = scene.mEntities.find(instanceRoot);
        if (rootIt == scene.mEntities.end() || !world.IsAlive(rootIt->second))
        {
            MIR_LOG_WARN("Copy as Ordinary Object: instance root {} is gone", instanceRoot);
            return kNullEntity;
        }

        const Entity root = rootIt->second;
        const Entity parent = ParentOf(world, root);
        const Entity before = NextSiblingOf(world, root);

        const Entity copy = DuplicateSceneEntity(world, scene, source);
        if (copy.IsNull())
            return kNullEntity;

        if (!SetParent(world, copy, parent, before, true))
        {
            MIR_LOG_WARN("Copy as Ordinary Object: could not place the copy beside instance root {}", instanceRoot);
            DestroySceneEntity(world, scene, copy);
            return kNullEntity;
        }

        scene.mDirty = true;
        return copy;
    }
}
