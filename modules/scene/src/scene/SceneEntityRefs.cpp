/**
 * @file SceneEntityRefs.cpp
 * @author Jackey Cen
 * @brief Clears authored EntityRefs that name removed scene entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneEntityRefs.h"

#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/World.h"
#include "scene/SceneInstancePatch.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mir
{
    namespace
    {
        std::optional<StableId> StableIdValue(const nlohmann::json &value)
        {
            if (value.is_number_unsigned())
                return value.get<uint64_t>();
            if (value.is_number_integer())
            {
                const int64_t raw = value.get<int64_t>();
                if (raw >= 0)
                    return static_cast<StableId>(raw);
            }
            return std::nullopt;
        }

        struct MemberOwner
        {
            ScenePrefabInstance *mInstance = nullptr;
            SceneNodeAddress mAddress;
        };
    }

    bool ClearSceneEntityRefsTo(World &world, LoadedScene &scene, std::span<const StableId> removedIds,
        std::vector<ClearedSceneEntityRef> *cleared)
    {
        if (removedIds.empty())
            return false;

        const std::unordered_set<StableId> removed(removedIds.begin(), removedIds.end());

        std::unordered_map<uint64_t, StableId> stableByEntity;
        stableByEntity.reserve(scene.mEntities.size());
        for (const auto &[id, entity] : scene.mEntities)
            stableByEntity.emplace(PackEntity(entity), id);

        std::unordered_map<StableId, MemberOwner> ownerByMember;
        for (auto &[root, instance] : scene.mInstances)
        {
            (void)root;
            for (const auto &[address, id] : instance.mBindings)
                ownerByMember.emplace(id, MemberOwner{ &instance, address });
        }

        std::set<std::tuple<uint64_t, uint32_t, std::string>> recorded;
        const auto record = [&](Entity entity, const ComponentOps &ops, std::string_view field)
            {
                if (cleared == nullptr)
                    return;
                const uint64_t packed = PackEntity(entity);
                if (!recorded.emplace(packed, ops.mType.seq, std::string(field)).second)
                    return;
                cleared->push_back(ClearedSceneEntityRef{ entity, ops.mType, std::string(field) });
            };

        bool changed = false;

        for (const auto &[id, entity] : scene.mEntities)
        {
            if (removed.contains(id) || !world.IsAlive(entity))
                continue;

            const auto ownerIt = ownerByMember.find(id);
            MemberOwner *owner = ownerIt != ownerByMember.end() ? &ownerIt->second : nullptr;

            for (const ComponentOps *ops : ComponentRegistry::Instance().All())
            {
                if (!ops->Has(world, entity))
                    continue;

                void *component = ops->GetComponent(world, entity);
                if (component == nullptr)
                    continue;

                for (const FieldDesc &field : ops->mFields)
                {
                    if (field.mKind != FieldKind::EntityRef)
                        continue;

                    Entity target = kNullEntity;
                    field.Read(component, &target);

                    bool clear = false;
                    if (!target.IsNull())
                    {
                        const auto targetIt = stableByEntity.find(PackEntity(target));
                        clear = targetIt != stableByEntity.end() && removed.contains(targetIt->second);
                    }

                    std::optional<StableId> unresolvedTarget;
                    if (owner != nullptr)
                    {
                        const auto key = std::make_tuple(id, std::string(ops->mType.name), std::string(field.mName));
                        const auto unresolved = scene.mUnresolvedEntityRefs.find(key);
                        if (unresolved != scene.mUnresolvedEntityRefs.end())
                            unresolvedTarget = unresolved->second;
                        if (unresolvedTarget && removed.contains(*unresolvedTarget))
                            clear = true;
                    }

                    if (!clear)
                        continue;

                    if (!target.IsNull())
                        field.Write(component, &kNullEntity);

                    if (owner != nullptr)
                    {
                        if (SceneComponentRecord *added =
                            FindAddedComponent(*owner->mInstance, owner->mAddress, ops->mType.name))
                        {
                            added->mFields[std::string(field.mName)] = nullptr;
                        }
                        else if (SceneFieldPatch *patch =
                            FindInstanceFieldPatch(*owner->mInstance, owner->mAddress, ops->mType.name, field.mName))
                        {
                            patch->mValue = nullptr;
                        }
                        else
                        {
                            GetOrAddInstancePatch(*owner->mInstance, owner->mAddress).mFields.push_back(
                                SceneFieldPatch{ std::string(ops->mType.name), std::string(field.mName), nullptr });
                        }

                        scene.mUnresolvedEntityRefs.erase(
                            { id, std::string(ops->mType.name), std::string(field.mName) });
                    }

                    record(entity, *ops, field.mName);
                    changed = true;
                }
            }
        }

        // Patches can name a removed member even after its live component is gone.
        for (auto &[root, instance] : scene.mInstances)
        {
            (void)root;
            for (SceneInstancePatch &patch : instance.mPatches)
            {
                const auto binding = instance.mBindings.find(patch.mTarget);
                if (binding == instance.mBindings.end())
                    continue;

                const auto entityIt = scene.mEntities.find(binding->second);
                const Entity entity = entityIt != scene.mEntities.end() ? entityIt->second : kNullEntity;

                for (SceneFieldPatch &fieldPatch : patch.mFields)
                {
                    const std::optional<StableId> target = StableIdValue(fieldPatch.mValue);
                    if (!target || !removed.contains(*target))
                        continue;

                    fieldPatch.mValue = nullptr;
                    changed = true;

                    if (!entity.IsNull() && world.IsAlive(entity))
                    {
                        const ComponentOps *ops = ComponentRegistry::Instance().Find(fieldPatch.mComponent);
                        if (ops != nullptr)
                            record(entity, *ops, fieldPatch.mField);
                    }
                }

                for (SceneComponentRecord &added : patch.mAddedComponents)
                {
                    const ComponentOps *ops = ComponentRegistry::Instance().Find(added.mType);
                    for (auto fieldIt = added.mFields.begin(); fieldIt != added.mFields.end(); ++fieldIt)
                    {
                        const std::optional<StableId> target = StableIdValue(fieldIt.value());
                        if (!target || !removed.contains(*target))
                            continue;

                        fieldIt.value() = nullptr;
                        changed = true;

                        if (ops != nullptr && !entity.IsNull() && world.IsAlive(entity))
                            record(entity, *ops, fieldIt.key());
                    }
                }
            }
        }

        const auto beforeMarkers = scene.mUnresolvedEntityRefs.size();
        std::erase_if(scene.mUnresolvedEntityRefs,
            [&](const auto &entry)
            {
                return removed.contains(std::get<0>(entry.first)) || removed.contains(entry.second);
            });
        if (scene.mUnresolvedEntityRefs.size() != beforeMarkers)
            changed = true;

        if (changed)
            scene.mDirty = true;
        return changed;
    }
}
