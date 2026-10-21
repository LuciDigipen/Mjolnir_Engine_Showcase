/**
 * @file EntityCodec.cpp
 * @author Sumin Park
 * @brief Entity <-> Json converter
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EntityCodec.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Signature.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "scene/ComponentJson.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <string>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        // Marks an EntityRef written as a live handle rather than an id in the set
        constexpr const char *kLiveEntityKey = "entity";
        constexpr const char *kSiblingIndexKey = "siblingIndex";

        // Missing on scenes saved before sibling order; sorts after every saved index, then by id
        constexpr uint32_t kNoSiblingIndex = std::numeric_limits<uint32_t>::max();

        json RefToJson(Entity ref, const EntityIdMap &ids, bool keepOutsideRefs)
        {
            if (ref.IsNull())
                return kNullStableId;

            const auto it = ids.find(PackEntity(ref));
            if (it != ids.end())
                return it->second;

            return keepOutsideRefs ? json{ { kLiveEntityKey, PackEntity(ref) } } : json(kNullStableId);
        }

        Entity ResolveRef(const World &world, const json &target, const std::map<StableId, Entity> &spawned,
            bool resolveOutsideRefs)
        {
            if (target.is_number_integer())
            {
                const std::optional<StableId> id = ReadStableIdJson(target);
                if (!id)
                {
                    MIR_LOG_WARN("SpawnEntities: EntityRef id is out of range, left null");
                    return kNullEntity;
                }
                if (*id == kNullStableId)
                    return kNullEntity;

                const auto it = spawned.find(*id);
                if (it != spawned.end())
                    return it->second;

                MIR_LOG_WARN("SpawnEntities: EntityRef target id {} not found in this set", *id);
                return kNullEntity;
            }

            if (resolveOutsideRefs && target.is_object())
            {
                const auto it = target.find(kLiveEntityKey);
                if (it != target.end() && it->is_number_integer())
                {
                    // the generation check turns a target destroyed since capture into null
                    const Entity entity = UnpackEntity(it->get<uint64_t>());
                    return world.IsAlive(entity) ? entity : kNullEntity;
                }
            }

            MIR_LOG_WARN("SpawnEntities: EntityRef '{}' is not an id this load resolves, left null", target.dump());
            return kNullEntity;
        }
    }

    uint32_t SiblingIndexOf(const World &world, Entity entity, const EntityIdMap &ids)
    {
        const HierarchyIndex *index = world.TryResource<HierarchyIndex>();
        if (index == nullptr)
            return 0;

        uint32_t position = 0;
        for (const Entity sibling : index->SiblingsOf(entity))
        {
            if (sibling == entity)
                break;
            if (ids.contains(PackEntity(sibling)))
                ++position;
        }
        return position;
    }

    uint32_t EntitySiblingIndexFromJson(const json &entityJson)
    {
        const auto it = entityJson.find(kSiblingIndexKey);
        if (it == entityJson.end())
            return kNoSiblingIndex;

        if (it->is_number_unsigned())
        {
            const uint64_t value = it->get<uint64_t>();
            return value <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(value) : kNoSiblingIndex;
        }
        if (it->is_number_integer())
        {
            const int64_t value = it->get<int64_t>();
            if (value >= 0 && value <= std::numeric_limits<uint32_t>::max())
                return static_cast<uint32_t>(value);
        }
        return kNoSiblingIndex;
    }

    std::optional<StableId> ReadStableIdJson(const json &value)
    {
        if (value.is_number_unsigned())
            return value.get<StableId>();
        if (!value.is_number_integer())
            return std::nullopt;

        const int64_t raw = value.get<int64_t>();
        if (raw < 0)
            return std::nullopt;
        return static_cast<StableId>(raw);
    }

    void ApplyEntityComponents(World &world, Entity entity, const json &entityJson, StableId id,
        std::vector<PendingEntityRef> &pendingRefs)
    {
        const auto componentsIt = entityJson.find("components");
        if (componentsIt == entityJson.end() || !componentsIt->is_array())
            return;

        ComponentRegistry &registry = ComponentRegistry::Instance();

        for (const json &blob : *componentsIt)
        {
            if (!blob.is_object())
                continue;

            const auto typeIt = blob.find("type");
            if (typeIt == blob.end() || !typeIt->is_string())
            {
                MIR_LOG_WARN("SpawnEntities: entity {} has a component without a type, skipped", id);
                continue;
            }

            const std::string typeName = typeIt->get<std::string>();
            const ComponentOps *ops = registry.Find(typeName);
            if (ops == nullptr)
            {
                MIR_LOG_WARN("SpawnEntities: unknown component '{}', skipped", typeName);
                continue; // forward compatibility: a name this build doesn't know
            }

            ops->AddCopy(world, entity, ops->mDefaultValue.data());
            void *component = ops->GetComponent(world, entity);

            const auto fieldsIt = blob.find("fields");
            if (fieldsIt == blob.end() || !fieldsIt->is_object())
                continue;

            const json &fields = *fieldsIt;
            for (const FieldDesc &field : ops->mFields)
            {
                auto it = fields.find(std::string(field.mName));
                if (it == fields.end())
                    continue;

                if (field.mKind == FieldKind::EntityRef)
                {
                    pendingRefs.push_back({ entity, ops, std::string(field.mName), *it });
                    continue;
                }

                FieldBuffer buffer{};
                if (!JsonToField(field.mKind, *it, buffer.mBytes))
                {
                    MIR_LOG_ERROR("SpawnEntities: entity {} '{}.{}' has a value this field cannot hold, left at default",
                        id, typeName, field.mName);
                    continue;
                }
                field.Write(component, buffer.mBytes);
            }
        }
    }

    void ResolvePendingEntityRefs(World &world, const std::vector<PendingEntityRef> &pendingRefs,
        const std::map<StableId, Entity> &spawned, bool resolveOutsideRefs)
    {
        for (const PendingEntityRef &pending : pendingRefs)
        {
            const Entity resolved = ResolveRef(world, pending.mTarget, spawned, resolveOutsideRefs);

            void *component = pending.mOps->GetComponent(world, pending.mEntity);
            const FieldDesc *field = pending.mOps->FindField(pending.mField);
            if (component != nullptr && field != nullptr)
                field->Write(component, &resolved);
        }
    }

    json EntityToJson(World &world, Entity entity, StableId id, const EntityIdMap &ids, bool keepOutsideRefs)
    {
        json file;
        file["id"] = id;

        const Entity parent = ParentOf(world, entity);
        const auto parentIt = ids.find(PackEntity(parent));
        file["parent"] = (!parent.IsNull() && parentIt != ids.end()) ? parentIt->second : kNullStableId;
        file[kSiblingIndexKey] = SiblingIndexOf(world, entity, ids);

        ComponentRegistry &registry = ComponentRegistry::Instance();

        json components = json::array();
        for (uint32_t seq = 0; seq < kMaxComponentTypes; ++seq)
        {
            const ComponentOps *ops = registry.FindBySeq(seq);
            if (ops == nullptr || !ops->Has(world, entity))
                continue;

            json fields = json::object();
            const void *component = ops->GetComponent(world, entity);
            for (const FieldDesc &field : ops->mFields)
            {
                FieldBuffer buffer{};
                field.Read(component, buffer.mBytes);

                if (field.mKind == FieldKind::EntityRef)
                {
                    Entity ref;
                    std::memcpy(&ref, buffer.mBytes, sizeof(Entity));
                    fields[std::string(field.mName)] = RefToJson(ref, ids, keepOutsideRefs);
                }
                else
                {
                    fields[std::string(field.mName)] = FieldToJson(field.mKind, buffer.mBytes);
                }
            }

            json blob;
            blob["type"] = std::string(ops->mType.name);
            blob["fields"] = std::move(fields);
            components.push_back(std::move(blob));
        }
        file["components"] = std::move(components);

        return file;
    }

    std::map<StableId, Entity> SpawnEntities(World &world, const std::vector<std::pair<StableId, json>> &entities,
        bool resolveOutsideRefs)
    {
        // before any create, so the sorted creation order below sets the root order
        InstallHierarchy(world);

        std::map<StableId, Entity> spawned;
        std::vector<PendingEntityRef> pendingRefs;

        struct PendingParent
        {
            Entity mEntity;
            StableId mParent;
        };
        std::vector<PendingParent> pendingParents;

        // Creation order sets root order and parent replay order sets child order,
        // so one sort by (siblingIndex, id) restores both
        std::vector<const std::pair<StableId, json> *> ordered;
        ordered.reserve(entities.size());
        for (const auto &entry : entities)
            ordered.push_back(&entry);

        std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b)
            {
                return std::pair(EntitySiblingIndexFromJson(a->second), a->first) <
                    std::pair(EntitySiblingIndexFromJson(b->second), b->first);
            });

        // 1. spawn every entity, add its components, write every non-EntityRef field
        for (const auto *entry : ordered)
        {
            const auto &[id, ejson] = *entry;
            Entity entity = world.CreateEntity();
            spawned[id] = entity;

            const auto parentIt = ejson.find("parent");
            if (parentIt != ejson.end())
            {
                const std::optional<StableId> parent = ReadStableIdJson(*parentIt);
                if (parent)
                {
                    if (*parent != kNullStableId)
                        pendingParents.push_back({ entity, *parent });
                }
                else
                {
                    MIR_LOG_WARN("SpawnEntities: entity {} has a malformed parent, left unparented", id);
                }
            }

            ApplyEntityComponents(world, entity, ejson, id, pendingRefs);
        }

        // 2. resolve parent ids
        for (const auto &p : pendingParents)
        {
            auto it = spawned.find(p.mParent);
            if (it == spawned.end())
            {
                MIR_LOG_WARN("SpawnEntities: parent id {} not found in this set, leaving unparented", p.mParent);
                continue;
            }
            // saved values are already parent-local
            SetParent(world, p.mEntity, it->second, kNullEntity, false);
        }

        // 3. patch EntityRef fields
        ResolvePendingEntityRefs(world, pendingRefs, spawned, resolveOutsideRefs);

        return spawned;
    }
}
