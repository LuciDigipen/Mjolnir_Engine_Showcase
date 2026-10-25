/**
 * @file PrefabCapture.cpp
 * @author Jackey Cen
 * @brief Captures an ordinary scene subtree into a new Prefab document.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabCapture.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Signature.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/ecs/components/Name.h"
#include "prefab/PrefabFormat.h"
#include "scene/ComponentJson.h"
#include "scene/EntitySnapshot.h"
#include "scene/SceneAsset.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        std::string EntityLabel(World &world, Entity entity)
        {
            if (const Name *name = world.GetComponent<Name>(entity))
                return std::string(name->mValue);
            return "Entity " + std::to_string(entity.mIndex);
        }

        std::unordered_set<uint64_t> InstanceMembers(const LoadedScene &scene)
        {
            std::unordered_set<uint64_t> members;
            for (const auto &[root, instance] : scene.mInstances)
            {
                const auto rootEntity = scene.mEntities.find(root);
                if (rootEntity != scene.mEntities.end())
                    members.insert(PackEntity(rootEntity->second));
                for (const auto &[address, id] : instance.mBindings)
                {
                    (void)address;
                    const auto member = scene.mEntities.find(id);
                    if (member != scene.mEntities.end())
                        members.insert(PackEntity(member->second));
                }
            }
            return members;
        }

        void ResetRootTransform(PrefabComponentInstance &component, const ComponentOps &ops)
        {
            const FieldDesc *position = ops.FindField("position");
            if (position != nullptr)
            {
                const WorldVec3 origin{ 0 };
                component.mFields[std::string(position->mName)] = FieldToJson(position->mKind, &origin);
            }

            const FieldDesc *rotation = ops.FindField("rotation");
            if (rotation != nullptr)
            {
                const glm::quat identity{ 1.0f, 0.0f, 0.0f, 0.0f };
                component.mFields[std::string(rotation->mName)] = FieldToJson(rotation->mKind, &identity);
            }
        }
    }

    PrefabCaptureResult CapturePrefab(World &world, const LoadedScene &scene, Entity root)
    {
        PrefabCaptureResult result;
        if (!world.IsAlive(root))
        {
            result.mRefusal = "the selection is not alive";
            return result;
        }

        const std::vector<Entity> members = SnapshotMembers(world, root);
        if (members.empty())
        {
            result.mRefusal = "the selection has no entities";
            return result;
        }

        const std::unordered_set<uint64_t> instanceMembers = InstanceMembers(scene);
        for (const Entity member : members)
        {
            if (instanceMembers.contains(PackEntity(member)))
            {
                result.mRefusal = "Entity '" + EntityLabel(world, member) +
                    "' is a Prefab instance root or member; capture ordinary entities only";
                return result;
            }
        }

        PrefabDocument document;
        document.mId = GeneratePrefabId();
        document.mNextNodeId = 1;

        std::map<uint64_t, PrefabNodeId> nodeIds;
        std::vector<PrefabNodeId> order;
        order.reserve(members.size());
        for (const Entity member : members)
        {
            const std::optional<PrefabNodeId> id = AllocateNodeId(document);
            if (!id)
            {
                result.mRefusal = "the document has no free node id";
                return result;
            }
            nodeIds.emplace(PackEntity(member), *id);
            order.push_back(*id);
        }
        document.mRootNode = order.front();

        std::unordered_map<uint64_t, StableId> owners;
        for (const auto &[id, entity] : scene.mEntities)
            if (nodeIds.contains(PackEntity(entity)))
                owners.emplace(PackEntity(entity), id);

        ComponentRegistry &registry = ComponentRegistry::Instance();
        for (const Entity member : members)
        {
            const auto savedOwner = owners.find(PackEntity(member));
            const StableId owner = savedOwner != owners.end() ? savedOwner->second : kNullStableId;

            PrefabNode node;
            node.mId = nodeIds.at(PackEntity(member));
            node.mKind = PrefabNodeKind::Entity;

            if (member != root)
            {
                const Entity parent = ParentOf(world, member);
                const auto parentId = nodeIds.find(PackEntity(parent));
                if (parentId != nodeIds.end())
                    node.mParent = parentId->second;
            }

            for (std::size_t seq = 0; seq < kMaxComponentTypes; ++seq)
            {
                const ComponentOps *ops = registry.FindBySeq(static_cast<uint32_t>(seq));
                if (ops == nullptr || !ops->Has(world, member))
                    continue;

                const void *component = ops->GetComponent(world, member);
                PrefabComponentInstance instance;
                instance.mType = std::string(ops->mType.name);

                for (const FieldDesc &field : ops->mFields)
                {
                    if (field.ReadOnly())
                        continue;

                    FieldBuffer buffer{};
                    field.Read(component, buffer.mBytes);

                    if (field.mKind == FieldKind::EntityRef)
                    {
                        Entity target = kNullEntity;
                        field.Read(component, &target);
                        if (target.IsNull())
                        {
                            if (owner != kNullStableId && scene.mUnresolvedEntityRefs.contains(
                                    { owner, instance.mType, std::string(field.mName) }))
                            {
                                result.mRefusal = "Entity '" + EntityLabel(world, member) + "' field '" +
                                    instance.mType + "." + std::string(field.mName) +
                                    "' has an unresolved reference to a temporarily unavailable target";
                                return result;
                            }
                            instance.mFields[std::string(field.mName)] = nullptr;
                            continue;
                        }

                        const auto targetId = nodeIds.find(PackEntity(target));
                        if (targetId == nodeIds.end())
                        {
                            result.mRefusal = "Entity '" + EntityLabel(world, member) + "' field '" +
                                instance.mType + "." + std::string(field.mName) +
                                "' points outside the subtree";
                            return result;
                        }
                        instance.mFields[std::string(field.mName)] =
                            json::array({ std::to_string(targetId->second) });
                        continue;
                    }

                    instance.mFields[std::string(field.mName)] = FieldToJson(field.mKind, buffer.mBytes);
                }

                if (member == root && instance.mType == "Transform")
                    ResetRootTransform(instance, *ops);

                node.mComponents.push_back(std::move(instance));
            }

            document.mNodes.push_back(std::move(node));
        }

        result.mDocument = std::move(document);
        return result;
    }

    std::string PrefabNameForFile(std::string_view name)
    {
        std::string text;
        text.reserve(name.size());
        for (const char raw : name)
        {
            const unsigned char character = static_cast<unsigned char>(raw);
            if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
                character == ' ')
            {
                text.push_back(raw);
            }
            else
            {
                text.push_back('_');
            }
        }

        while (!text.empty() && (text.back() == ' ' || text.back() == '.'))
            text.pop_back();
        const std::size_t first = text.find_first_not_of(' ');
        if (first == std::string::npos)
            return "Prefab";
        return text.substr(first);
    }

    std::filesystem::path UniquePrefabPath(const std::filesystem::path &prefabsDir, std::string_view name,
        const PrefabRegistry &registry)
    {
        const std::string base = PrefabNameForFile(name);
        for (uint64_t suffix = 0;; ++suffix)
        {
            const std::string stem = suffix == 0 ? base : base + " " + std::to_string(suffix);
            const std::filesystem::path candidate = prefabsDir / (stem + ".prefab");

            std::error_code ec;
            const bool exists = std::filesystem::exists(candidate, ec);
            if (!exists && !ec && !registry.IdAtPath(candidate))
                return candidate;
        }
    }
}
