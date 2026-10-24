/**
 * @file SceneIO.cpp
 * @author Sumin Park
 * @brief Save and load a scene as a manifest plus one file per entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneIO.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "scene/ComponentJson.h"
#include "EntityCodec.h"
#include "SceneInstanceCodec.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        constexpr std::string_view kEntitiesDirName = "entities";
        constexpr std::string_view kInstancesDirName = "instances";

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        fs::path EntityFilePath(const fs::path &sceneDir, StableId id)
        {
            return sceneDir / kEntitiesDirName / (std::to_string(id) + ".json");
        }

        fs::path InstanceFilePath(const fs::path &sceneDir, StableId id)
        {
            return sceneDir / kInstancesDirName / (std::to_string(id) + ".json");
        }

        std::optional<StableId> StableIdFromFile(const fs::path &file)
        {
            if (file.extension() != ".json")
                return std::nullopt;

            const std::string stem = file.stem().string();
            StableId id = kNullStableId;
            const auto [end, err] = std::from_chars(stem.data(), stem.data() + stem.size(), id);
            if (err != std::errc{} || end != stem.data() + stem.size())
                return std::nullopt;
            return id;
        }

        void RemoveStaleFiles(const fs::path &dir, const std::set<StableId> &keep, std::string_view kind)
        {
            std::vector<fs::path> stale;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::optional<StableId> id = StableIdFromFile(it->path());
                if (id && !keep.contains(*id))
                    stale.push_back(it->path());
            }

            for (const fs::path &file : stale)
            {
                std::error_code removeEc;
                fs::remove(file, removeEc);
                if (removeEc)
                    MIR_LOG_WARN("SaveScene: could not remove stale {} file '{}': {}", kind, file.string(),
                        removeEc.message());
            }
        }

        void ApplyPlacement(World &world, Entity root, const json &placement)
        {
            if (!placement.is_object() || placement.empty())
                return;

            const ComponentOps *transform = ComponentRegistry::Instance().Find("Transform");
            if (transform == nullptr)
            {
                MIR_LOG_WARN("LoadScene: Transform is not registered, instance placement skipped");
                return;
            }

            if (!transform->Has(world, root))
                transform->AddDefault(world, root);

            void *component = transform->GetComponent(world, root);
            if (component == nullptr)
                return;

            for (auto it = placement.begin(); it != placement.end(); ++it)
            {
                const FieldDesc *field = transform->FindField(it.key());
                if (field == nullptr)
                {
                    MIR_LOG_WARN("LoadScene: placement has unknown Transform field '{}', skipped", it.key());
                    continue;
                }
                if (field->mKind == FieldKind::EntityRef)
                {
                    MIR_LOG_WARN("LoadScene: placement field '{}' is an EntityRef, skipped", it.key());
                    continue;
                }

                FieldBuffer buffer{};
                if (!JsonToField(field->mKind, it.value(), buffer.mBytes))
                {
                    MIR_LOG_WARN("LoadScene: placement field '{}' has a value it cannot hold, left at template value",
                        it.key());
                    continue;
                }
                field->Write(component, buffer.mBytes);
            }
        }

        void ApplyInstancePatches(World &world, LoadedScene &loaded, ScenePrefabInstance &instance,
            std::vector<PendingEntityRef> &pendingRefs)
        {
            ComponentRegistry &registry = ComponentRegistry::Instance();

            for (const SceneInstancePatch &patch : instance.mPatches)
            {
                const auto bindingIt = instance.mBindings.find(patch.mTarget);
                if (bindingIt == instance.mBindings.end())
                {
                    MIR_LOG_WARN("LoadScene: patch targets an address instance {} does not bind", instance.mRoot);
                    continue;
                }

                const auto entityIt = loaded.mEntities.find(bindingIt->second);
                if (entityIt == loaded.mEntities.end() || !world.IsAlive(entityIt->second))
                {
                    MIR_LOG_WARN("LoadScene: patch targets a member instance {} did not materialize", instance.mRoot);
                    continue;
                }
                const Entity entity = entityIt->second;

                for (const SceneComponentRecord &added : patch.mAddedComponents)
                {
                    const ComponentOps *ops = registry.Find(added.mType);
                    if (ops == nullptr)
                    {
                        MIR_LOG_WARN("LoadScene: patch adds an unknown component '{}', kept", added.mType);
                        continue;
                    }

                    ops->AddDefault(world, entity);
                    void *component = ops->GetComponent(world, entity);
                    if (component == nullptr)
                        continue;

                    for (auto it = added.mFields.begin(); it != added.mFields.end(); ++it)
                    {
                        const FieldDesc *field = ops->FindField(it.key());
                        if (field == nullptr)
                        {
                            MIR_LOG_WARN("LoadScene: added component '{}' has an unknown field '{}', kept", added.mType,
                                it.key());
                            continue;
                        }

                        if (field->mKind == FieldKind::EntityRef)
                        {
                            pendingRefs.push_back({ entity, ops, it.key(), it.value() });
                            continue;
                        }

                        FieldBuffer buffer{};
                        if (!JsonToField(field->mKind, it.value(), buffer.mBytes))
                        {
                            MIR_LOG_WARN("LoadScene: added component '{}' field '{}' has a value it cannot hold, left at default",
                                added.mType, it.key());
                            continue;
                        }
                        field->Write(component, buffer.mBytes);
                    }
                }

                // Added components must exist before their field overrides are applied.
                for (const SceneFieldPatch &fieldPatch : patch.mFields)
                {
                    const ComponentOps *ops = registry.Find(fieldPatch.mComponent);
                    if (ops == nullptr || !ops->Has(world, entity))
                    {
                        MIR_LOG_WARN("LoadScene: patch has an unknown component '{}', kept", fieldPatch.mComponent);
                        continue;
                    }

                    const FieldDesc *field = ops->FindField(fieldPatch.mField);
                    if (field == nullptr)
                    {
                        MIR_LOG_WARN("LoadScene: patch has an unknown field '{}.{}', kept", fieldPatch.mComponent,
                            fieldPatch.mField);
                        continue;
                    }

                    void *component = ops->GetComponent(world, entity);
                    if (component == nullptr)
                        continue;

                    if (field->mKind == FieldKind::EntityRef)
                    {
                        pendingRefs.push_back({ entity, ops, fieldPatch.mField, fieldPatch.mValue });
                        continue;
                    }

                    FieldBuffer buffer{};
                    if (!JsonToField(field->mKind, fieldPatch.mValue, buffer.mBytes))
                    {
                        MIR_LOG_WARN("LoadScene: patch '{}.{}' has a value this field cannot hold, left at template value",
                            fieldPatch.mComponent, fieldPatch.mField);
                        continue;
                    }
                    field->Write(component, buffer.mBytes);
                }

                for (const std::string &removed : patch.mRemovedComponents)
                {
                    const ComponentOps *ops = registry.Find(removed);
                    if (ops == nullptr)
                    {
                        MIR_LOG_WARN("LoadScene: patch removes an unknown component '{}', kept", removed);
                        continue;
                    }
                    ops->Remove(world, entity);
                }
            }
        }

        std::optional<SceneNodeAddress> RootAddressOf(const ScenePrefabInstance &instance)
        {
            for (const auto &[address, id] : instance.mBindings)
            {
                if (id == instance.mRoot)
                    return address;
            }
            return std::nullopt;
        }

        void DeriveEntityRefPatches(World &world, const LoadedScene &scene, const ScenePrefabResolver &resolver,
            const ScenePrefabInstance &instance, const EntityIdMap &ids, ScenePrefabInstance &out)
        {
            ComponentRegistry &registry = ComponentRegistry::Instance();

            for (const auto &[address, stableId] : instance.mBindings)
            {
                const auto entityIt = scene.mEntities.find(stableId);
                if (entityIt == scene.mEntities.end() || !world.IsAlive(entityIt->second))
                    continue;
                const Entity entity = entityIt->second;

                for (const ComponentOps *ops : registry.All())
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

                        std::optional<SceneNodeAddress> defaultTarget;
                        if (!resolver.TemplateEntityRef(instance, address, ops->mType.name, field.mName, defaultTarget))
                            continue;

                        Entity target = kNullEntity;
                        field.Read(component, &target);

                        // A missing template is not a user clearing this field.
                        // SetInstanceEntityRef removes this marker on an explicit edit.
                        if (target.IsNull() && !scene.mUnresolvedEntityRefs.empty() && scene.mUnresolvedEntityRefs.contains(
                            { stableId, std::string(ops->mType.name), std::string(field.mName) }))
                        {
                            continue;
                        }

                        StableId worldValue = kNullStableId;
                        if (!target.IsNull())
                        {
                            const auto targetIt = ids.find(PackEntity(target));
                            if (targetIt != ids.end())
                                worldValue = targetIt->second;
                        }

                        StableId expected = kNullStableId;
                        if (defaultTarget)
                        {
                            const auto bindingIt = out.mBindings.find(*defaultTarget);
                            if (bindingIt != out.mBindings.end())
                                expected = bindingIt->second;
                        }

                        SceneFieldPatch *fieldPatch = FindInstanceFieldPatch(out, address, ops->mType.name,
                            field.mName);
                        if (fieldPatch != nullptr)
                        {
                            fieldPatch->mValue = worldValue;
                            continue;
                        }
                        if (worldValue == expected)
                            continue;

                        GetOrAddInstancePatch(out, address).mFields.push_back(SceneFieldPatch{
                            std::string(ops->mType.name), std::string(field.mName), worldValue });
                    }
                }
            }
        }

        struct SceneManifest
        {
            std::string mName;
            StableId mNextId = 1;
            std::vector<StableId> mEntities;
            std::vector<StableId> mInstances;
        };

        // false for anything but an array of non-null ids
        bool ReadIdList(const json &manifest, const char *key, std::vector<StableId> &out, std::string &error)
        {
            const auto list = manifest.find(key);
            if (list == manifest.end())
                return true;
            if (!list->is_array())
            {
                error = std::string("\"") + key + "\" is not an array";
                return false;
            }
            for (const json &idJson : *list)
            {
                const std::optional<StableId> id = ReadStableIdJson(idJson);
                if (!id || *id == kNullStableId)
                {
                    error = std::string("\"") + key + "\" holds something other than an entity id";
                    return false;
                }
                out.push_back(*id);
            }
            return true;
        }

        // ProbeScene and LoadScene both read through this, so whatever the probe passes the load can read
        std::optional<SceneManifest> ReadManifest(const fs::path &sceneDir, std::string &error)
        {
            const std::optional<json> manifest = ReadJsonFile(SceneManifestPath(sceneDir), &error);
            if (!manifest)
                return std::nullopt;
            if (!manifest->is_object())
            {
                error = "the top level is not an object";
                return std::nullopt;
            }

            SceneManifest read;
            if (const auto name = manifest->find("name"); name != manifest->end())
            {
                if (!name->is_string())
                {
                    error = "\"name\" is not a string";
                    return std::nullopt;
                }
                read.mName = name->get<std::string>();
            }
            if (const auto nextId = manifest->find("nextId"); nextId != manifest->end())
            {
                const std::optional<StableId> value = ReadStableIdJson(*nextId);
                if (!value)
                {
                    error = "\"nextId\" is not an entity id";
                    return std::nullopt;
                }
                read.mNextId = std::max<StableId>(1, *value);
            }

            if (!ReadIdList(*manifest, "entities", read.mEntities, error) ||
                !ReadIdList(*manifest, "instances", read.mInstances, error))
                return std::nullopt;

            std::set<StableId> instances;
            for (const StableId id : read.mInstances)
            {
                if (!instances.insert(id).second)
                {
                    error = "\"instances\" lists " + std::to_string(id) + " more than once";
                    return std::nullopt;
                }
            }
            return read;
        }

        std::optional<json> ReadEntityFile(const fs::path &sceneDir, StableId id)
        {
            std::optional<json> entity = ReadJsonFile(EntityFilePath(sceneDir, id));
            if (!entity || !entity->is_object())
                return std::nullopt;
            return entity;
        }

        std::optional<ScenePrefabInstance> ReadInstanceFile(const fs::path &sceneDir, StableId id, std::string &error)
        {
            const fs::path path = InstanceFilePath(sceneDir, id);
            const std::optional<json> instanceJson = ReadJsonFile(path);
            if (!instanceJson || !instanceJson->is_object())
            {
                error = "is missing or unreadable";
                return std::nullopt;
            }

            std::optional<ScenePrefabInstance> record = InstanceFromJson(*instanceJson, path.string());
            if (!record)
            {
                error = "is not an instance record";
                return std::nullopt;
            }
            if (record->mRoot != id)
            {
                error = "names root " + std::to_string(record->mRoot);
                return std::nullopt;
            }
            return record;
        }

        // Bindings own member ids, even if the prefab is currently unavailable; members have no entity file
        std::set<StableId> MemberIdsOf(const std::map<StableId, ScenePrefabInstance> &instances)
        {
            std::set<StableId> memberIds;
            for (const auto &[root, instance] : instances)
            {
                for (const auto &[address, id] : instance.mBindings)
                    memberIds.insert(id);
            }
            return memberIds;
        }
    }

    fs::path SceneManifestPath(const fs::path &sceneDir)
    {
        return sceneDir / kSceneManifestFileName;
    }

    json InstancePlacementFromWorld(World &world, Entity root)
    {
        const ComponentOps *transform = ComponentRegistry::Instance().Find("Transform");
        if (transform == nullptr || !transform->Has(world, root))
            return json::object();

        const void *component = transform->GetComponent(world, root);
        if (component == nullptr)
            return json::object();

        json placement = json::object();
        for (const FieldDesc &field : transform->mFields)
        {
            if (field.mKind == FieldKind::EntityRef)
                continue;

            FieldBuffer buffer{};
            field.Read(component, buffer.mBytes);
            placement[std::string(field.mName)] = FieldToJson(field.mKind, buffer.mBytes);
        }
        return placement;
    }

    bool SaveScene(World &world, const fs::path &sceneDir, const LoadedScene &scene,
        const ScenePrefabResolver *resolver)
    {
        std::error_code ec;
        fs::create_directories(sceneDir / kEntitiesDirName, ec);
        if (ec)
        {
            MIR_LOG_ERROR("SaveScene: could not create '{}': {}", sceneDir.string(), ec.message());
            return false;
        }

        const fs::path instancesDir = sceneDir / kInstancesDirName;
        if (!scene.mInstances.empty())
        {
            fs::create_directories(instancesDir, ec);
            if (ec)
            {
                MIR_LOG_ERROR("SaveScene: could not create '{}': {}", instancesDir.string(), ec.message());
                return false;
            }
        }

        EntityIdMap ids;
        ids.reserve(scene.mEntities.size());
        for (const auto &[id, entity] : scene.mEntities)
            ids[PackEntity(entity)] = id;

        const std::set<StableId> memberIds = MemberIdsOf(scene.mInstances);

        std::set<StableId> entityFiles;
        for (const auto &[id, entity] : scene.mEntities)
        {
            if (memberIds.contains(id))
                continue;

            entityFiles.insert(id);
            // a file outlives this run, so a reference outside the scene is written null
            json file = EntityToJson(world, entity, id, ids, false);
            if (!scene.mUnresolvedEntityRefs.empty())
            {
                for (json &component : file["components"])
                {
                    const std::string type = component["type"].get<std::string>();
                    for (auto &[field, value] : component["fields"].items())
                    {
                        const auto unresolved = scene.mUnresolvedEntityRefs.find({ id, type, field });
                        if (unresolved == scene.mUnresolvedEntityRefs.end())
                            continue;

                        const ComponentOps *ops = ComponentRegistry::Instance().Find(type);
                        const FieldDesc *desc = ops->FindField(field);
                        Entity current = kNullEntity;
                        desc->Read(ops->GetComponent(world, entity), &current);
                        if (current.IsNull())
                            value = unresolved->second;
                    }
                }
            }
            if (!WriteJsonFile(EntityFilePath(sceneDir, id), file))
            {
                MIR_LOG_ERROR("SaveScene: could not write entity file for id {}", id);
                return false;
            }
        }

        std::set<StableId> instanceFiles;
        for (const auto &[root, record] : scene.mInstances)
        {
            ScenePrefabInstance out = record;

            const auto entityIt = scene.mEntities.find(root);
            if (entityIt != scene.mEntities.end() && world.IsAlive(entityIt->second))
            {
                const Entity rootEntity = entityIt->second;
                const Entity parent = ParentOf(world, rootEntity);
                const auto parentIt = ids.find(PackEntity(parent));
                out.mParent = (!parent.IsNull() && parentIt != ids.end()) ? parentIt->second : kNullStableId;
                out.mSiblingIndex = SiblingIndexOf(world, rootEntity, ids);
                out.mPlacement = InstancePlacementFromWorld(world, rootEntity);

                if (resolver != nullptr)
                    DeriveEntityRefPatches(world, scene, *resolver, record, ids, out);
            }

            if (!WriteJsonFile(InstanceFilePath(sceneDir, root), InstanceToJson(out)))
            {
                MIR_LOG_ERROR("SaveScene: could not write instance file for id {}", root);
                return false;
            }
            instanceFiles.insert(root);
        }

        // The manifest is written last
        json manifest;
        manifest["name"] = scene.mName;
        manifest["nextId"] = scene.mNextId;
        manifest["entities"] = json::array();
        for (const auto &[id, entity] : scene.mEntities)
            manifest["entities"].push_back(id);

        if (!scene.mInstances.empty())
        {
            manifest["instances"] = json::array();
            for (const auto &[root, record] : scene.mInstances)
                manifest["instances"].push_back(root);
        }

        if (!WriteJsonFile(SceneManifestPath(sceneDir), manifest))
        {
            MIR_LOG_ERROR("SaveScene: could not write {} in '{}'", kSceneManifestFileName, sceneDir.string());
            return false;
        }

        RemoveStaleFiles(sceneDir / kEntitiesDirName, entityFiles, "entity");
        if (fs::exists(instancesDir, ec))
            RemoveStaleFiles(instancesDir, instanceFiles, "instance");
        return true;
    }

    SceneProbe ProbeScene(const fs::path &sceneDir)
    {
        SceneProbe probe;

        const std::optional<SceneManifest> manifest = ReadManifest(sceneDir, probe.mManifestError);
        if (!manifest)
            return probe;

        std::map<StableId, ScenePrefabInstance> instances;
        for (const StableId id : manifest->mInstances)
        {
            std::string error;
            if (std::optional<ScenePrefabInstance> record = ReadInstanceFile(sceneDir, id, error))
                instances.emplace(id, std::move(*record));
            else
                probe.mMissingInstances.push_back(id);
        }

        const std::set<StableId> memberIds = MemberIdsOf(instances);
        for (const StableId id : manifest->mEntities)
        {
            if (!memberIds.contains(id) && !ReadEntityFile(sceneDir, id))
                probe.mMissingEntities.push_back(id);
        }

        return probe;
    }

    LoadedScene LoadScene(World &world, const fs::path &sceneDir, ScenePrefabResolver *resolver)
    {
        LoadedScene loaded;

        std::string error;
        const std::optional<SceneManifest> manifest = ReadManifest(sceneDir, error);
        if (!manifest)
        {
            MIR_LOG_ERROR("LoadScene: '{}' has no usable {} ({}), loading an empty scene", sceneDir.string(),
                kSceneManifestFileName, error);
            return loaded;
        }
        loaded.mName = manifest->mName;
        loaded.mNextId = manifest->mNextId;

        for (const StableId id : manifest->mInstances)
        {
            std::string instanceError;
            std::optional<ScenePrefabInstance> record = ReadInstanceFile(sceneDir, id, instanceError);
            if (!record)
            {
                MIR_LOG_ERROR("LoadScene: instance file '{}' {}, skipped", InstanceFilePath(sceneDir, id).string(),
                    instanceError);
                continue;
            }
            loaded.mInstances.emplace(id, std::move(*record));
        }

        // Read before entity files so stale flattened members cannot shadow them
        const std::set<StableId> memberIds = MemberIdsOf(loaded.mInstances);

        std::vector<std::pair<StableId, json>> entities;
        for (const StableId id : manifest->mEntities)
        {
            if (memberIds.contains(id))
                continue;

            std::optional<json> entityJson = ReadEntityFile(sceneDir, id);
            if (!entityJson)
            {
                MIR_LOG_ERROR("LoadScene: entity file for id {} listed in manifest but missing or unreadable", id);
                continue; // 0030 leaves this failure mode open; skip rather than abort the load
            }
            entities.emplace_back(id, std::move(*entityJson));
        }

        const auto raiseNextId = [&loaded](StableId id)
            {
                if (id != std::numeric_limits<StableId>::max())
                    loaded.mNextId = std::max(loaded.mNextId, id + 1);
            };

        for (const StableId id : manifest->mEntities)
            raiseNextId(id);
        for (const auto &[root, instance] : loaded.mInstances)
        {
            raiseNextId(root);
            for (const auto &[address, id] : instance.mBindings)
                raiseNextId(id);
        }

        std::set<StableId> earlierIds;
        std::set<StableId> materializeRoots;
        for (const auto &[root, instance] : loaded.mInstances)
        {
            std::set<StableId> bindingIds;
            for (const auto &[address, id] : instance.mBindings)
                bindingIds.insert(id);

            bool valid = bindingIds.contains(root);
            if (!valid)
                MIR_LOG_ERROR("LoadScene: instance {} has no binding for its root, left unmaterialized", root);

            if (bindingIds.size() != instance.mBindings.size())
            {
                MIR_LOG_ERROR("LoadScene: instance {} binds one StableId to several addresses, left unmaterialized",
                    root);
                valid = false;
            }

            for (StableId id : bindingIds)
            {
                if (earlierIds.contains(id))
                {
                    MIR_LOG_ERROR("LoadScene: instance {} StableId {} collides with an earlier instance", root, id);
                    valid = false;
                }
            }

            earlierIds.insert(bindingIds.begin(), bindingIds.end());
            if (valid)
                materializeRoots.insert(root);
        }

        struct SpawnItem
        {
            StableId mId = kNullStableId;
            uint32_t mSiblingIndex = 0;
            bool mIsInstance = false;
            const json *mEntityJson = nullptr;
        };

        std::vector<SpawnItem> items;
        items.reserve(entities.size() + materializeRoots.size());
        for (const auto &[id, entityJson] : entities)
            items.push_back(SpawnItem{ id, EntitySiblingIndexFromJson(entityJson), false, &entityJson });
        for (StableId root : materializeRoots)
        {
            const ScenePrefabInstance &record = loaded.mInstances.at(root);
            items.push_back(SpawnItem{ root, record.mSiblingIndex, true, nullptr });
        }

        // Create roots in their saved order; parent replay also inserts external
        // children among the members already attached by the prefab resolver.
        std::sort(items.begin(), items.end(), [](const SpawnItem &a, const SpawnItem &b)
            {
                return std::pair(a.mSiblingIndex, a.mId) < std::pair(b.mSiblingIndex, b.mId);
            });

        InstallHierarchy(world);

        std::vector<PendingEntityRef> pendingRefs;

        struct PendingParent
        {
            Entity mEntity;
            StableId mParent;
            uint32_t mSiblingIndex;
        };
        std::vector<PendingParent> pendingParents;

        for (const SpawnItem &item : items)
        {
            if (!item.mIsInstance)
            {
                const json &entityJson = *item.mEntityJson;
                const Entity entity = world.CreateEntity();
                loaded.mEntities[item.mId] = entity;

                const auto parentIt = entityJson.find("parent");
                if (parentIt != entityJson.end())
                {
                    const std::optional<StableId> parent = ReadStableIdJson(*parentIt);
                    if (parent)
                    {
                        if (*parent != kNullStableId)
                            pendingParents.push_back({ entity, *parent, item.mSiblingIndex });
                    }
                    else
                    {
                        MIR_LOG_WARN("LoadScene: entity {} has a malformed parent, left unparented", item.mId);
                    }
                }

                ApplyEntityComponents(world, entity, entityJson, item.mId, pendingRefs);
                continue;
            }

            ScenePrefabInstance &instance = loaded.mInstances.at(item.mId);
            if (resolver == nullptr)
            {
                MIR_LOG_WARN("LoadScene: no prefab resolver, instance {} left unchanged", item.mId);
                continue;
            }

            std::map<SceneNodeAddress, Entity> members = resolver->Materialize(world, instance);
            if (members.empty())
            {
                MIR_LOG_WARN("LoadScene: prefab '{}' for instance {} is unavailable, left unchanged",
                    instance.mPrefab, item.mId);
                continue;
            }

            const std::optional<SceneNodeAddress> rootAddress = RootAddressOf(instance);
            if (!rootAddress)
            {
                MIR_LOG_WARN("LoadScene: instance {} has no root binding, left unchanged", item.mId);
                for (const auto &[address, entity] : members)
                {
                    if (world.IsAlive(entity))
                        world.DestroyEntity(entity);
                }
                continue;
            }

            const auto rootIt = members.find(*rootAddress);
            if (rootIt == members.end())
            {
                MIR_LOG_WARN("LoadScene: prefab for instance {} did not materialize its root, left unchanged",
                    item.mId);
                for (const auto &[address, entity] : members)
                {
                    if (world.IsAlive(entity))
                        world.DestroyEntity(entity);
                }
                continue;
            }
            const Entity rootEntity = rootIt->second;

            for (const auto &[address, entity] : members)
            {
                const auto bindingIt = instance.mBindings.find(address);
                if (bindingIt != instance.mBindings.end())
                {
                    loaded.mEntities[bindingIt->second] = entity;
                    continue;
                }

                const StableId id = AllocateStableId(loaded);
                instance.mBindings[address] = id;
                loaded.mEntities[id] = entity;
                loaded.mDirty = true;
            }

            for (const auto &[address, id] : instance.mBindings)
            {
                if (!members.contains(address))
                {
                    MIR_LOG_WARN("LoadScene: instance {} keeps a binding for an address it did not materialize",
                        item.mId);
                }
            }

            ApplyPlacement(world, rootEntity, instance.mPlacement);
            ApplyInstancePatches(world, loaded, instance, pendingRefs);

            if (instance.mParent != kNullStableId)
                pendingParents.push_back({ rootEntity, instance.mParent, item.mSiblingIndex });
        }

        EntityIdMap ids;
        for (const auto &[id, entity] : loaded.mEntities)
            ids[PackEntity(entity)] = id;

        // resolve parent ids
        const HierarchyIndex &hierarchy = InstallHierarchy(world);
        std::map<StableId, std::size_t> nextSiblingIndices;
        for (const auto &pending : pendingParents)
        {
            const auto parentIt = loaded.mEntities.find(pending.mParent);
            if (parentIt == loaded.mEntities.end())
            {
                MIR_LOG_WARN("LoadScene: parent id {} not found in this scene, leaving unparented", pending.mParent);
                continue;
            }
            // Preserve (index, id) order for tied indices as well as template
            // members whose positions are implicit in the prefab graph.
            const std::size_t insertionIndex = std::max<std::size_t>(
                pending.mSiblingIndex, nextSiblingIndices[pending.mParent]);
            nextSiblingIndices[pending.mParent] = insertionIndex + 1;
            Entity before = kNullEntity;
            const auto siblings = hierarchy.ChildrenOf(parentIt->second);
            // Ordinary scenes append in sorted order without scanning all earlier children.
            if (insertionIndex < siblings.size())
            {
                std::size_t position = 0;
                for (Entity sibling : siblings)
                {
                    if (!ids.contains(PackEntity(sibling)))
                        continue;
                    if (position++ == insertionIndex)
                    {
                        before = sibling;
                        break;
                    }
                }
            }
            // saved values are already parent-local
            SetParent(world, pending.mEntity, parentIt->second, before, false);
        }

        // patch EntityRef fields
        ResolvePendingEntityRefs(world, pendingRefs, loaded.mEntities, false);

        // Remember unresolved member identities separately from the null runtime
        // handles. Later entries win, matching ResolvePendingEntityRefs.
        for (const PendingEntityRef &pending : pendingRefs)
        {
            const auto owner = ids.find(PackEntity(pending.mEntity));
            if (owner == ids.end() || !pending.mOps->Has(world, pending.mEntity))
                continue;

            const auto key = std::make_tuple(owner->second, std::string(pending.mOps->mType.name), pending.mField);
            loaded.mUnresolvedEntityRefs.erase(key);
            const std::optional<StableId> target = ReadStableIdJson(pending.mTarget);
            if (target && memberIds.contains(*target) && !loaded.mEntities.contains(*target))
                loaded.mUnresolvedEntityRefs[key] = *target;
        }

        return loaded;
    }

    bool SetInstanceEntityRef(World &world, LoadedScene &scene, StableId root, const SceneNodeAddress &node,
        std::string_view component, std::string_view field, StableId target)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
            return false;

        ScenePrefabInstance &instance = instanceIt->second;
        const auto bindingIt = instance.mBindings.find(node);
        if (bindingIt == instance.mBindings.end())
            return false;

        const auto entityIt = scene.mEntities.find(bindingIt->second);
        if (entityIt == scene.mEntities.end() || !world.IsAlive(entityIt->second))
            return false;
        const Entity entity = entityIt->second;

        const ComponentOps *ops = ComponentRegistry::Instance().Find(component);
        if (ops == nullptr || !ops->Has(world, entity))
            return false;

        const FieldDesc *fieldDesc = ops->FindField(field);
        if (fieldDesc == nullptr || fieldDesc->mKind != FieldKind::EntityRef)
            return false;

        void *componentBytes = ops->GetComponent(world, entity);
        if (componentBytes == nullptr)
            return false;

        Entity targetEntity = kNullEntity;
        if (target != kNullStableId)
        {
            const auto targetIt = scene.mEntities.find(target);
            if (targetIt == scene.mEntities.end() || !world.IsAlive(targetIt->second))
                return false;
            targetEntity = targetIt->second;
        }

        Entity current = kNullEntity;
        fieldDesc->Read(componentBytes, &current);

        if (SceneComponentRecord *added = FindAddedComponent(instance, node, component))
        {
            bool recordMatches = false;
            const auto fieldIt = added->mFields.find(std::string(field));
            if (fieldIt != added->mFields.end())
            {
                const std::optional<StableId> existingValue = ReadStableIdJson(*fieldIt);
                recordMatches = existingValue && *existingValue == target;
            }

            if (current == targetEntity && recordMatches)
                return false;

            fieldDesc->Write(componentBytes, &targetEntity);
            scene.mUnresolvedEntityRefs.erase({ bindingIt->second, std::string(component), std::string(field) });
            added->mFields[std::string(field)] = target;
            scene.mDirty = true;
            return true;
        }

        bool patchMatches = false;
        if (const SceneFieldPatch *existing = FindInstanceFieldPatch(instance, node, component, field))
        {
            const std::optional<StableId> existingValue = ReadStableIdJson(existing->mValue);
            patchMatches = existingValue && *existingValue == target;
        }

        if (current == targetEntity && patchMatches)
            return false;

        fieldDesc->Write(componentBytes, &targetEntity);
        scene.mUnresolvedEntityRefs.erase({ bindingIt->second, std::string(component), std::string(field) });

        SceneFieldPatch *fieldPatch = FindInstanceFieldPatch(instance, node, component, field);
        if (fieldPatch != nullptr)
        {
            fieldPatch->mValue = target;
        }
        else
        {
            GetOrAddInstancePatch(instance, node).mFields.push_back(SceneFieldPatch{
                std::string(component), std::string(field), target });
        }

        scene.mDirty = true;
        return true;
    }

    void UnloadScene(World &world, const LoadedScene &loaded)
    {
        for (const auto &[id, entity] : loaded.mEntities)
        {
            // destroying a parent cascades, so a child reached later may
            // already be dead
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }
    }
}
