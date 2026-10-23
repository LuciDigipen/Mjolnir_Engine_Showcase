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
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

        std::optional<StableId> StableIdFromJson(const json &value)
        {
            if (value.is_null())
                return kNullStableId;
            if (value.is_number_unsigned())
                return value.get<uint64_t>();
            if (value.is_number_integer())
            {
                const int64_t raw = value.get<int64_t>();
                if (raw < 0)
                    return std::nullopt;
                return static_cast<StableId>(raw);
            }
            return std::nullopt;
        }

        json AddressJson(const SceneNodeAddress &address)
        {
            json out = json::array();
            for (uint64_t segment : address)
                out.push_back(std::to_string(segment));
            return out;
        }

        // A captured entity's address in the new document; empty when it is outside the selection.
        bool TranslateRef(const json &value, const std::map<StableId, SceneNodeAddress> &addresses, json &out)
        {
            const std::optional<StableId> target = StableIdFromJson(value);
            if (!target)
                return false;
            if (*target == kNullStableId)
            {
                out = nullptr;
                return true;
            }

            const auto address = addresses.find(*target);
            if (address == addresses.end())
                return false;
            out = AddressJson(address->second);
            return true;
        }

        SceneNodeAddress InnerRootAddress(const ScenePrefabInstance &instance)
        {
            for (const auto &[address, id] : instance.mBindings)
            {
                if (id == instance.mRoot)
                    return address;
            }
            return {};
        }

        PrefabPatch &GetOrAddPatch(PrefabNode &node, const SceneNodeAddress &target)
        {
            for (PrefabPatch &patch : node.mPatches)
            {
                if (patch.mTarget == target)
                    return patch;
            }

            PrefabPatch patch;
            patch.mTarget = target;
            node.mPatches.push_back(std::move(patch));
            return node.mPatches.back();
        }

        void AddPlacementPatch(PrefabNode &node, World &world, Entity root, const ScenePrefabInstance &instance,
            const ComponentOps &ops)
        {
            const SceneNodeAddress target = InnerRootAddress(instance);
            if (target.empty() || !ops.Has(world, root))
                return;

            const void *component = ops.GetComponent(world, root);
            if (component == nullptr)
                return;

            PrefabPatch &patch = GetOrAddPatch(node, target);
            for (const FieldDesc &field : ops.mFields)
            {
                if (field.ReadOnly() || field.mKind == FieldKind::EntityRef)
                    continue;

                FieldBuffer buffer{};
                field.Read(component, buffer.mBytes);
                const json value = FieldToJson(field.mKind, buffer.mBytes);

                const auto existing = std::find_if(patch.mFields.begin(), patch.mFields.end(),
                    [&ops, &field](const PrefabFieldOverride &entry)
                    {
                        return entry.mComponent == ops.mType.name && entry.mField == field.mName;
                    });
                if (existing != patch.mFields.end())
                    existing->mValue = value;
                else
                    patch.mFields.push_back(
                        PrefabFieldOverride{ std::string(ops.mType.name), std::string(field.mName), value });
            }
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

        std::map<uint64_t, StableId> stableIds;
        for (const auto &[id, entity] : scene.mEntities)
            stableIds.emplace(PackEntity(entity), id);

        const auto rootId = stableIds.find(PackEntity(root));
        if (rootId == stableIds.end())
        {
            result.mRefusal = "the selection is not part of this scene";
            return result;
        }

        std::unordered_set<uint64_t> memberKeys;
        for (const Entity member : members)
            memberKeys.insert(PackEntity(member));

        std::map<StableId, const ScenePrefabInstance *> capturedInstances;
        std::unordered_map<uint64_t, const ScenePrefabInstance *> instanceOfMember;
        for (const auto &[instanceRoot, instance] : scene.mInstances)
        {
            const auto rootEntity = scene.mEntities.find(instanceRoot);
            if (rootEntity == scene.mEntities.end() || !world.IsAlive(rootEntity->second))
                continue;

            const bool rootInside = memberKeys.contains(PackEntity(rootEntity->second));
            bool anyInside = false;
            bool allInside = true;
            for (const auto &[address, id] : instance.mBindings)
            {
                (void)address;
                const auto entity = scene.mEntities.find(id);
                if (entity == scene.mEntities.end() || !world.IsAlive(entity->second))
                    continue;
                if (memberKeys.contains(PackEntity(entity->second)))
                    anyInside = true;
                else
                    allInside = false;
            }

            if (rootInside)
            {
                if (instanceRoot == rootId->second)
                {
                    result.mRefusal = "Entity '" + EntityLabel(world, root) +
                        "' is a Prefab instance root; capture an ordinary parent";
                    return result;
                }
                if (!allInside)
                {
                    result.mRefusal = "Prefab instance '" + EntityLabel(world, rootEntity->second) +
                        "' reaches outside the selection";
                    return result;
                }
                capturedInstances.emplace(instanceRoot, &instance);
            }
            else if (anyInside)
            {
                result.mRefusal = "an entity in the selection belongs to a Prefab instance whose root is outside it";
                return result;
            }
            else
            {
                continue;
            }

            for (const auto &[address, id] : instance.mBindings)
            {
                (void)address;
                const auto entity = scene.mEntities.find(id);
                if (entity != scene.mEntities.end())
                    instanceOfMember.emplace(PackEntity(entity->second), &instance);
            }
        }

        for (const Entity member : members)
        {
            if (member == root)
                continue;

            const auto stable = stableIds.find(PackEntity(member));
            const auto instance = stable != stableIds.end()
                ? instanceOfMember.find(PackEntity(member))
                : instanceOfMember.end();
            if (instance != instanceOfMember.end() && instance->second->mRoot != stable->second)
                continue;

            const Entity parent = ParentOf(world, member);
            if (parent.IsNull() || instanceOfMember.contains(PackEntity(parent)))
            {
                result.mRefusal = "Entity '" + EntityLabel(world, member) +
                    "' sits under a Prefab instance; capture it outside the instance";
                return result;
            }
        }

        PrefabDocument document;
        document.mId = GeneratePrefabId();
        document.mNextNodeId = 1;

        std::map<uint64_t, PrefabNodeId> entityNodes;
        std::map<StableId, PrefabNodeId> occurrenceNodes;
        for (const Entity member : members)
        {
            const auto stable = stableIds.find(PackEntity(member));
            if (stable == stableIds.end())
                continue;

            const auto instance = instanceOfMember.find(PackEntity(member));
            if (instance != instanceOfMember.end())
            {
                if (instance->second->mRoot != stable->second || occurrenceNodes.contains(stable->second))
                    continue;
            }
            else if (entityNodes.contains(PackEntity(member)))
            {
                continue;
            }

            const std::optional<PrefabNodeId> id = AllocateNodeId(document);
            if (!id)
            {
                result.mRefusal = "the document has no free node id";
                return result;
            }

            if (instance != instanceOfMember.end())
                occurrenceNodes.emplace(stable->second, *id);
            else
                entityNodes.emplace(PackEntity(member), *id);
        }

        const auto rootNode = entityNodes.find(PackEntity(root));
        if (rootNode == entityNodes.end())
        {
            result.mRefusal = "the selection needs an ordinary entity root";
            return result;
        }
        document.mRootNode = rootNode->second;

        std::map<StableId, SceneNodeAddress> addresses;
        for (const auto &[entityKey, nodeId] : entityNodes)
            addresses.emplace(stableIds.at(entityKey), SceneNodeAddress{ nodeId });
        for (const auto &[instanceRoot, instance] : capturedInstances)
        {
            const PrefabNodeId occurrence = occurrenceNodes.at(instanceRoot);
            addresses[instanceRoot] = SceneNodeAddress{ occurrence };
            for (const auto &[address, id] : instance->mBindings)
            {
                SceneNodeAddress full{ occurrence };
                full.insert(full.end(), address.begin(), address.end());
                addresses[id] = std::move(full);
            }
        }

        ComponentRegistry &registry = ComponentRegistry::Instance();
        const auto translateFields = [&addresses](const ComponentOps &ops, const json &fields, json &out)
        {
            for (auto it = fields.begin(); it != fields.end(); ++it)
            {
                const FieldDesc *field = ops.FindField(it.key());
                if (field != nullptr && field->ReadOnly())
                    continue;
                if (field == nullptr || field->mKind != FieldKind::EntityRef)
                {
                    out[it.key()] = it.value();
                    continue;
                }

                json translated;
                if (!TranslateRef(it.value(), addresses, translated))
                    return false;
                out[it.key()] = std::move(translated);
            }
            return true;
        };

        for (const Entity member : members)
        {
            const auto stable = stableIds.find(PackEntity(member));
            if (stable == stableIds.end())
                continue;

            const auto instanceIt = instanceOfMember.find(PackEntity(member));
            if (instanceIt != instanceOfMember.end())
            {
                const ScenePrefabInstance &instance = *instanceIt->second;
                if (instance.mRoot != stable->second)
                    continue;

                const std::optional<PrefabId> prefab = ParsePrefabId(instance.mPrefab);
                if (!prefab)
                {
                    result.mRefusal = "Prefab instance '" + EntityLabel(world, member) +
                        "' has an invalid prefab id";
                    return result;
                }

                PrefabNode node;
                node.mId = occurrenceNodes.at(stable->second);
                node.mKind = PrefabNodeKind::Prefab;
                node.mPrefab = *prefab;

                const Entity parent = ParentOf(world, member);
                const auto parentNode = entityNodes.find(PackEntity(parent));
                if (parentNode != entityNodes.end())
                    node.mParent = parentNode->second;

                for (const SceneInstancePatch &patch : instance.mPatches)
                {
                    PrefabPatch converted;
                    converted.mTarget = patch.mTarget;

                    for (const SceneFieldPatch &field : patch.mFields)
                    {
                        PrefabFieldOverride entry{ field.mComponent, field.mField, field.mValue };
                        if (const ComponentOps *ops = registry.Find(field.mComponent))
                        {
                            const FieldDesc *desc = ops->FindField(field.mField);
                            if (desc != nullptr && desc->mKind == FieldKind::EntityRef &&
                                !TranslateRef(field.mValue, addresses, entry.mValue))
                            {
                                result.mRefusal = "Prefab instance '" + EntityLabel(world, member) + "' field '" +
                                    field.mComponent + "." + field.mField + "' points outside the selection";
                                return result;
                            }
                        }
                        converted.mFields.push_back(std::move(entry));
                    }

                    for (const SceneComponentRecord &added : patch.mAddedComponents)
                    {
                        const ComponentOps *ops = registry.Find(added.mType);
                        if (ops == nullptr)
                            continue;

                        PrefabComponentInstance component;
                        component.mType = added.mType;
                        if (!translateFields(*ops, added.mFields, component.mFields))
                        {
                            result.mRefusal = "Prefab instance '" + EntityLabel(world, member) +
                                "' added component '" + added.mType + "' points outside the selection";
                            return result;
                        }
                        converted.mAddedComponents.push_back(std::move(component));
                    }

                    converted.mRemovedComponents = patch.mRemovedComponents;
                    node.mPatches.push_back(std::move(converted));
                }

                if (const ComponentOps *transform = registry.Find("Transform"))
                    AddPlacementPatch(node, world, member, instance, *transform);

                document.mNodes.push_back(std::move(node));
                continue;
            }

            PrefabNode node;
            node.mId = entityNodes.at(PackEntity(member));
            node.mKind = PrefabNodeKind::Entity;

            if (member != root)
            {
                const Entity parent = ParentOf(world, member);
                const auto parentId = entityNodes.find(PackEntity(parent));
                if (parentId != entityNodes.end())
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
                            instance.mFields[std::string(field.mName)] = nullptr;
                            continue;
                        }

                        const auto targetId = stableIds.find(PackEntity(target));
                        json translated;
                        if (targetId == stableIds.end() ||
                            !TranslateRef(json(targetId->second), addresses, translated))
                        {
                            result.mRefusal = "Entity '" + EntityLabel(world, member) + "' field '" +
                                instance.mType + "." + std::string(field.mName) +
                                "' points outside the subtree";
                            return result;
                        }
                        instance.mFields[std::string(field.mName)] = std::move(translated);
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

        const std::vector<PrefabDiagnostic> diagnostics = ValidatePrefab(document);
        if (HasPrefabErrors(diagnostics))
        {
            result.mRefusal = diagnostics.front().mMessage;
            return result;
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
