/**
 * @file PrefabAuthoring.cpp
 * @author Jackey Cen
 * @brief Authored field, component and node state of prefab documents and scene instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabAuthoring.h"

#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "prefab/CompiledPrefab.h"
#include "prefab/PrefabFormat.h"
#include "scene/ComponentJson.h"
#include "scene/SceneInstancePatch.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        std::string AddressText(const SceneNodeAddress &address)
        {
            std::string text;
            for (uint64_t segment : address)
            {
                if (!text.empty())
                    text += '/';
                text += std::to_string(segment);
            }
            return text;
        }

        bool IsNodeKey(const PrefabAuthoredKey &key)
        {
            return key.mComponent.empty() && key.mField.empty();
        }

        bool IsFieldKey(const PrefabAuthoredKey &key)
        {
            return !key.mComponent.empty() && !key.mField.empty();
        }

        bool IsComponentKey(const PrefabAuthoredKey &key)
        {
            return !key.mComponent.empty() && key.mField.empty();
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

        bool DecodeValue(const std::optional<std::string> &text, json &out)
        {
            if (!text)
                return true;
            out = json::parse(*text, nullptr, false);
            return !out.is_discarded();
        }

        std::optional<std::size_t> FindNodeIndex(const PrefabDocument &document, PrefabNodeId id)
        {
            for (std::size_t i = 0; i < document.mNodes.size(); ++i)
            {
                if (document.mNodes[i].mId == id)
                    return i;
            }
            return std::nullopt;
        }

        const PrefabNode *FindNode(const PrefabDocument &document, const SceneNodeAddress &address)
        {
            if (address.size() != 1)
                return nullptr;
            const std::optional<std::size_t> index = FindNodeIndex(document, address.front());
            return index ? &document.mNodes[*index] : nullptr;
        }

        const PrefabComponentInstance *FindComponent(const PrefabNode &node, std::string_view type)
        {
            for (const PrefabComponentInstance &component : node.mComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        PrefabComponentInstance *FindComponent(PrefabNode &node, std::string_view type)
        {
            for (PrefabComponentInstance &component : node.mComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        bool ValueFitsField(const FieldDesc &field, const json &value)
        {
            if (field.mKind == FieldKind::EntityRef)
                return value.is_null() || value.is_array();
            FieldBuffer buffer{};
            return JsonToField(field.mKind, value, buffer.mBytes);
        }

        bool ValidateDocumentFields(std::string_view type, const json &fields, std::string &reason)
        {
            if (!fields.is_object())
            {
                reason = "component \"" + std::string(type) + "\" fields are not an object";
                return false;
            }

            const ComponentOps *ops = ComponentRegistry::Instance().Find(type);
            if (ops == nullptr)
            {
                reason = "component \"" + std::string(type) + "\" is not registered";
                return false;
            }

            for (auto it = fields.begin(); it != fields.end(); ++it)
            {
                const FieldDesc *field = ops->FindField(it.key());
                if (field == nullptr)
                {
                    reason = "component \"" + std::string(type) + "\" has no field \"" + it.key() + "\"";
                    return false;
                }
                if (field->ReadOnly())
                {
                    reason = "field \"" + std::string(type) + "." + it.key() + "\" is read-only";
                    return false;
                }
                if (!ValueFitsField(*field, it.value()))
                {
                    reason = "field \"" + std::string(type) + "." + it.key() + "\" value does not fit";
                    return false;
                }
            }

            return true;
        }

        json NodeRecord(const PrefabDocument &document, const PrefabNode &node)
        {
            json out = json::object();
            const std::optional<std::size_t> index = FindNodeIndex(document, node.mId);
            out["index"] = index.value_or(document.mNodes.size());

            if (node.mId != document.mRootNode)
                out["parent"] = std::to_string(node.mParent);

            json components = json::array();
            for (const PrefabComponentInstance &component : node.mComponents)
                components.push_back(json{ { "type", component.mType }, { "fields", component.mFields } });
            out["components"] = std::move(components);
            return out;
        }

        bool ReadNodeRecord(const json &record, PrefabNode &out, std::string &reason)
        {
            if (!record.is_object())
            {
                reason = "a node record is not an object";
                return false;
            }

            if (const auto components = record.find("components"); components != record.end())
            {
                if (!components->is_array())
                {
                    reason = "node components are not an array";
                    return false;
                }

                for (const json &entry : *components)
                {
                    if (!entry.is_object())
                    {
                        reason = "a node component is not an object";
                        return false;
                    }
                    const auto type = entry.find("type");
                    if (type == entry.end() || !type->is_string())
                    {
                        reason = "a node component has no type";
                        return false;
                    }

                    PrefabComponentInstance component;
                    component.mType = type->get<std::string>();
                    if (const auto fields = entry.find("fields"); fields != entry.end())
                    {
                        std::string fieldReason;
                        if (!ValidateDocumentFields(component.mType, *fields, fieldReason))
                        {
                            reason = std::move(fieldReason);
                            return false;
                        }
                        component.mFields = *fields;
                    }
                    out.mComponents.push_back(std::move(component));
                }
            }

            if (const auto parent = record.find("parent"); parent != record.end())
            {
                if (!parent->is_string())
                {
                    reason = "node parent is not a node id";
                    return false;
                }
                const std::string &text = parent->get_ref<const std::string &>();
                uint64_t id = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
                if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
                    id == kNullPrefabNodeId)
                {
                    reason = "node parent \"" + text + "\" is not a node id";
                    return false;
                }
                out.mParent = id;
            }

            return true;
        }

        bool ApplyDocumentEntry(PrefabDocument &document, const PrefabAuthoredEntry &entry,
            std::vector<SceneNodeAddress> &removedNodes)
        {
            if (entry.mKey.mAddress.size() != 1)
                return false;

            const PrefabNodeId id = entry.mKey.mAddress.front();
            if (IsNodeKey(entry.mKey))
            {
                const std::optional<std::size_t> existing = FindNodeIndex(document, id);
                if (!entry.mValue)
                {
                    if (id == document.mRootNode)
                        return false;
                    if (existing)
                    {
                        document.mNodes.erase(document.mNodes.begin() + static_cast<std::ptrdiff_t>(*existing));
                        removedNodes.push_back(entry.mKey.mAddress);
                    }
                    return true;
                }

                json record;
                if (!DecodeValue(entry.mValue, record))
                    return false;

                PrefabNode node;
                node.mId = id;
                node.mKind = PrefabNodeKind::Entity;
                std::string reason;
                if (!ReadNodeRecord(record, node, reason))
                {
                    MIR_LOG_WARN("Prefab authoring: node \"{}\" record: {}", AddressText(entry.mKey.mAddress), reason);
                    return false;
                }

                std::size_t index = existing.value_or(document.mNodes.size());
                if (record.contains("index") && record["index"].is_number_unsigned())
                    index = static_cast<std::size_t>(record["index"].get<uint64_t>());
                if (existing)
                    document.mNodes.erase(document.mNodes.begin() + static_cast<std::ptrdiff_t>(*existing));
                index = std::min(index, document.mNodes.size());
                document.mNodes.insert(document.mNodes.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
                return true;
            }

            const std::optional<std::size_t> nodeIndex = FindNodeIndex(document, id);
            if (!nodeIndex)
                return false;
            PrefabNode &node = document.mNodes[*nodeIndex];

            if (IsComponentKey(entry.mKey))
            {
                json fields;
                if (!DecodeValue(entry.mValue, fields))
                    return false;

                if (!entry.mValue)
                {
                    std::erase_if(node.mComponents, [&](const PrefabComponentInstance &component)
                        {
                            return component.mType == entry.mKey.mComponent;
                        });
                    return true;
                }

                std::string reason;
                if (!ValidateDocumentFields(entry.mKey.mComponent, fields, reason))
                {
                    MIR_LOG_WARN("Prefab authoring: component \"{}\": {}", entry.mKey.mComponent, reason);
                    return false;
                }

                PrefabComponentInstance added;
                added.mType = entry.mKey.mComponent;
                added.mFields = std::move(fields);
                if (PrefabComponentInstance *existing = FindComponent(node, entry.mKey.mComponent))
                    *existing = std::move(added);
                else
                    node.mComponents.push_back(std::move(added));
                return true;
            }

            if (!IsFieldKey(entry.mKey))
                return false;

            PrefabComponentInstance *component = FindComponent(node, entry.mKey.mComponent);
            if (component == nullptr)
                return false;

            const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mKey.mComponent);
            const FieldDesc *field = ops != nullptr ? ops->FindField(entry.mKey.mField) : nullptr;
            if (field == nullptr || field->ReadOnly())
                return false;

            json value;
            if (!DecodeValue(entry.mValue, value) || (entry.mValue && !ValueFitsField(*field, value)))
                return false;

            if (entry.mValue)
                component->mFields[entry.mKey.mField] = std::move(value);
            else
                component->mFields.erase(entry.mKey.mField);
            return true;
        }

        bool SceneMemberAlive(const LoadedScene &scene, World &world, StableId id)
        {
            const auto it = scene.mEntities.find(id);
            return it != scene.mEntities.end() && world.IsAlive(it->second);
        }

        bool SceneValueFitsField(const FieldDesc &field, const json &value, const LoadedScene &scene, World &world)
        {
            if (field.mKind == FieldKind::EntityRef)
            {
                const std::optional<StableId> target = StableIdFromJson(value);
                if (!target)
                    return false;
                return *target == kNullStableId || SceneMemberAlive(scene, world, *target);
            }
            FieldBuffer buffer{};
            return JsonToField(field.mKind, value, buffer.mBytes);
        }

        // Written back to a removed component, these restore it with the overrides its removal erased.
        std::optional<std::string> InheritedOverrides(const ScenePrefabInstance &instance,
            const SceneNodeAddress &node, std::string_view component)
        {
            const SceneInstancePatch *patch = FindInstancePatch(instance, node);
            if (patch == nullptr)
                return std::nullopt;

            json fields = json::object();
            for (const SceneFieldPatch &field : patch->mFields)
            {
                if (field.mComponent == component)
                    fields[field.mField] = field.mValue;
            }
            if (fields.empty())
                return std::nullopt;
            return fields.dump();
        }

        struct SceneEdit
        {
            const PrefabAuthoredEntry *mEntry = nullptr;
            StableId mRoot = kNullStableId;
            SceneNodeAddress mNode;
            json mValue;
        };

        struct ComponentBackup
        {
            Entity mEntity = kNullEntity;
            const ComponentOps *mOps = nullptr;
            bool mExisted = false;
            std::vector<std::byte> mBytes;
        };

        bool PrepareSceneEdit(const PrefabAuthoredEntry &entry, const LoadedScene &scene, World &world,
            const std::function<const PrefabDocument *(PrefabId)> &documents, SceneEdit &out)
        {
            if (entry.mKey.mAddress.size() != 2)
                return false;

            out.mEntry = &entry;
            out.mRoot = entry.mKey.mAddress.front();
            out.mNode = SceneNodeAddress{ entry.mKey.mAddress.back() };

            if (!DecodeValue(entry.mValue, out.mValue))
                return false;

            const auto instanceIt = scene.mInstances.find(out.mRoot);
            if (instanceIt == scene.mInstances.end())
                return false;
            const ScenePrefabInstance &instance = instanceIt->second;

            const auto binding = instance.mBindings.find(out.mNode);
            if (binding == instance.mBindings.end() || !SceneMemberAlive(scene, world, binding->second))
                return false;

            if (!IsFieldKey(entry.mKey) && !IsComponentKey(entry.mKey))
                return false;

            const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mKey.mComponent);
            if (ops == nullptr)
                return false;

            if (IsFieldKey(entry.mKey))
            {
                const FieldDesc *field = ops->FindField(entry.mKey.mField);
                if (field == nullptr || field->ReadOnly())
                    return false;

                const PrefabComponentState state =
                    InstanceComponentState(documents, scene, out.mRoot, out.mNode, entry.mKey.mComponent);
                if (state == PrefabComponentState::Absent || state == PrefabComponentState::Removed)
                    return false;

                if (entry.mValue && !SceneValueFitsField(*field, out.mValue, scene, world))
                    return false;
                return true;
            }

            if (!entry.mValue)
                return true;

            const PrefabComponentState state = InstanceComponentState(documents, scene, out.mRoot, out.mNode,
                entry.mKey.mComponent);

            if (out.mValue.is_null())
                return state == PrefabComponentState::Inherited || state == PrefabComponentState::Removed;

            if (!out.mValue.is_object())
                return false;

            for (auto it = out.mValue.begin(); it != out.mValue.end(); ++it)
            {
                const FieldDesc *field = ops->FindField(it.key());
                if (field == nullptr || field->ReadOnly())
                    return false;
                if (!SceneValueFitsField(*field, it.value(), scene, world))
                    return false;
            }

            return state != PrefabComponentState::Inherited;
        }

        bool WriteSceneField(World &world, LoadedScene &scene, const PrefabRegistry &registry,
            CompiledPrefabCache &cache, const SceneEdit &edit)
        {
            const PrefabAuthoredKey &key = edit.mEntry->mKey;
            ScenePrefabInstance &instance = scene.mInstances.at(edit.mRoot);

            if (edit.mEntry->mValue)
            {
                return SetInstanceFieldOverride(world, scene, registry, edit.mRoot, edit.mNode, key.mComponent,
                    key.mField, edit.mValue);
            }

            if (SceneComponentRecord *added = FindAddedComponent(instance, edit.mNode, key.mComponent))
            {
                const auto it = added->mFields.find(key.mField);
                if (it != added->mFields.end())
                {
                    added->mFields.erase(it);

                    const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
                    const FieldDesc *field = ops != nullptr ? ops->FindField(key.mField) : nullptr;
                    if (ops == nullptr || field == nullptr)
                        return false;

                    void *component = ops->GetComponent(world, scene.mEntities.at(instance.mBindings.at(edit.mNode)));
                    FieldBuffer buffer{};
                    if (component != nullptr && !ops->mDefaultValue.empty())
                        field->Read(ops->mDefaultValue.data(), buffer.mBytes);
                    if (component != nullptr)
                        field->Write(component, buffer.mBytes);

                    scene.mUnresolvedEntityRefs.erase(
                        { instance.mBindings.at(edit.mNode), std::string(key.mComponent), std::string(key.mField) });
                    scene.mDirty = true;
                    return true;
                }
            }

            if (FindInstanceFieldPatch(instance, edit.mNode, key.mComponent, key.mField) != nullptr)
            {
                return RevertInstancePatch(world, registry, cache, scene, edit.mRoot,
                    PrefabPatchSelection{ edit.mNode, PrefabPatchKind::Field, key.mComponent, key.mField });
            }
            return true;
        }

        bool WriteSceneComponent(World &world, LoadedScene &scene, const PrefabRegistry &registry,
            CompiledPrefabCache &cache, const SceneEdit &edit)
        {
            const PrefabAuthoredKey &key = edit.mEntry->mKey;
            const PrefabComponentState state = InstanceComponentState(registry, scene, edit.mRoot, edit.mNode,
                key.mComponent);

            if (!edit.mEntry->mValue)
            {
                switch (state)
                {
                case PrefabComponentState::Added:
                    return RemoveInstanceComponent(world, scene, registry, edit.mRoot, edit.mNode, key.mComponent);
                case PrefabComponentState::Removed:
                    return RevertInstancePatch(world, registry, cache, scene, edit.mRoot,
                        PrefabPatchSelection{ edit.mNode, PrefabPatchKind::RemovedComponent, key.mComponent, {} });
                case PrefabComponentState::Inherited:
                case PrefabComponentState::Absent:
                    return true;
                }
                return false;
            }

            if (edit.mValue.is_null())
            {
                if (state == PrefabComponentState::Inherited)
                {
                    return RemoveInstanceComponent(world, scene, registry, edit.mRoot, edit.mNode, key.mComponent);
                }
                return state == PrefabComponentState::Removed;
            }

            if (state == PrefabComponentState::Removed)
            {
                if (!RevertInstancePatch(world, registry, cache, scene, edit.mRoot,
                    PrefabPatchSelection{ edit.mNode, PrefabPatchKind::RemovedComponent, key.mComponent, {} }))
                {
                    return false;
                }
            }
            else if (state == PrefabComponentState::Absent)
            {
                if (!AddInstanceComponent(world, scene, registry, edit.mRoot, edit.mNode, key.mComponent))
                    return false;
            }

            for (auto it = edit.mValue.begin(); it != edit.mValue.end(); ++it)
            {
                if (!SetInstanceFieldOverride(world, scene, registry, edit.mRoot, edit.mNode, key.mComponent,
                    it.key(), it.value()))
                {
                    return false;
                }
            }
            return true;
        }

        bool SceneBindsAddress(const LoadedScene &scene, PrefabId prefab, const SceneNodeAddress &address)
        {
            for (const auto &[root, instance] : scene.mInstances)
            {
                (void)root;
                const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
                if (!id || *id != prefab)
                    continue;
                if (instance.mBindings.contains(address))
                    return true;
            }
            return false;
        }

        bool TranslateEntityRef(const LoadedScene &scene, const ScenePrefabInstance &instance, const json &value,
            json &outValue, std::string &reason)
        {
            const std::optional<StableId> target = StableIdFromJson(value);
            if (!target)
            {
                reason = "is not a StableId or null";
                return false;
            }
            if (*target == kNullStableId)
            {
                outValue = nullptr;
                return true;
            }

            for (const auto &[address, id] : instance.mBindings)
            {
                if (id != *target)
                    continue;
                if (address.size() != 1)
                {
                    reason = "targets a nested prefab node, which is not supported";
                    return false;
                }
                outValue = json::array({ std::to_string(address.front()) });
                return true;
            }

            reason = scene.mEntities.contains(*target) ? "targets an entity outside this instance"
                                                       : "targets an entity the scene does not contain";
            return false;
        }
    }

    PrefabAuthoring::PrefabAuthoring(World &world, PrefabCoordinator &coordinator)
        : mWorld(world), mCoordinator(coordinator)
    {
    }

    std::optional<PrefabAuthoring::Member> PrefabAuthoring::FindMember(Entity entity) const
    {
        const LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr || !mWorld.IsAlive(entity))
            return std::nullopt;

        for (const auto &[root, instance] : scene->mInstances)
        {
            for (const auto &[address, id] : instance.mBindings)
            {
                const auto it = scene->mEntities.find(id);
                if (it == scene->mEntities.end() || it->second != entity)
                    continue;

                Member member;
                member.mRoot = root;
                member.mNode = address;
                member.mInstance = &instance;
                member.mEntity = entity;
                return member;
            }
        }
        return std::nullopt;
    }

    std::optional<PrefabAuthoredKey> PrefabAuthoring::FieldKey(Entity entity, std::string_view component,
        std::string_view field) const
    {
        std::optional<Member> member = FindMember(entity);
        if (!member)
            return std::nullopt;

        const LoadedScene &scene = *mCoordinator.Scene();
        const auto root = scene.mEntities.find(member->mInstance->mRoot);
        if (component == "Transform" && root != scene.mEntities.end() && root->second == entity)
            return std::nullopt;

        PrefabAuthoredKey key;
        key.mAddress.reserve(member->mNode.size() + 1);
        key.mAddress.push_back(member->mRoot);
        key.mAddress.insert(key.mAddress.end(), member->mNode.begin(), member->mNode.end());
        key.mComponent = component;
        key.mField = field;
        return key;
    }

    std::optional<PrefabAuthoredKey> PrefabAuthoring::ComponentKey(Entity entity, std::string_view component) const
    {
        std::optional<Member> member = FindMember(entity);
        if (!member)
            return std::nullopt;

        const LoadedScene &scene = *mCoordinator.Scene();
        const auto root = scene.mEntities.find(member->mInstance->mRoot);
        if (component == "Transform" && root != scene.mEntities.end() && root->second == entity)
            return std::nullopt;

        PrefabAuthoredKey key;
        key.mAddress.reserve(member->mNode.size() + 1);
        key.mAddress.push_back(member->mRoot);
        key.mAddress.insert(key.mAddress.end(), member->mNode.begin(), member->mNode.end());
        key.mComponent = component;
        return key;
    }

    PrefabFieldState PrefabAuthoring::FieldState(const PrefabAuthoredKey &key) const
    {
        if (key.mPrefab)
        {
            const PrefabDocument *document = mCoordinator.Registry().Find(*key.mPrefab);
            if (document == nullptr)
                return PrefabFieldState::Inherited;
            const PrefabNode *node = FindNode(*document, key.mAddress);
            const PrefabComponentInstance *component =
                node != nullptr ? FindComponent(*node, key.mComponent) : nullptr;
            return component != nullptr && component->mFields.contains(key.mField) ? PrefabFieldState::Overridden
                                                                                  : PrefabFieldState::Inherited;
        }

        const LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr || key.mAddress.size() < 2)
            return PrefabFieldState::Inherited;

        const StableId root = key.mAddress.front();
        const SceneNodeAddress node(key.mAddress.begin() + 1, key.mAddress.end());
        const auto instance = scene->mInstances.find(root);
        if (instance == scene->mInstances.end())
            return PrefabFieldState::Inherited;

        if (const SceneComponentRecord *added = FindAddedComponent(instance->second, node, key.mComponent))
            return added->mFields.contains(key.mField) ? PrefabFieldState::Overridden : PrefabFieldState::Inherited;
        return FindInstanceFieldPatch(instance->second, node, key.mComponent, key.mField) != nullptr
            ? PrefabFieldState::Overridden
            : PrefabFieldState::Inherited;
    }

    PrefabComponentState PrefabAuthoring::ComponentState(Entity entity, std::string_view component) const
    {
        std::optional<Member> member = FindMember(entity);
        if (!member)
            return PrefabComponentState::Absent;
        return InstanceComponentState(mCoordinator.Registry(), *mCoordinator.Scene(), member->mRoot, member->mNode,
            component);
    }

    std::vector<std::string> PrefabAuthoring::RemovedComponents(Entity entity) const
    {
        std::vector<std::string> removed;
        std::optional<Member> member = FindMember(entity);
        if (!member)
            return removed;

        const std::optional<PrefabId> id = ParsePrefabId(member->mInstance->mPrefab);
        if (!id)
            return removed;

        const PrefabDocument *document = mCoordinator.Registry().Find(*id);
        const PrefabNode *node = document != nullptr ? FindNode(*document, member->mNode) : nullptr;
        if (node == nullptr)
            return removed;

        for (const PrefabComponentInstance &component : node->mComponents)
        {
            if (HasRemovedComponent(*member->mInstance, member->mNode, component.mType))
                removed.push_back(component.mType);
        }
        return removed;
    }

    std::string PrefabAuthoring::EncodeField(FieldKind kind, const void *bytes) const
    {
        if (kind != FieldKind::EntityRef)
            return FieldToJson(kind, bytes).dump();

        const Entity entity = *static_cast<const Entity *>(bytes);
        StableId id = kNullStableId;
        if (const LoadedScene *scene = mCoordinator.Scene())
        {
            for (const auto &[stable, candidate] : scene->mEntities)
            {
                if (candidate == entity)
                {
                    id = stable;
                    break;
                }
            }
        }
        return id == kNullStableId ? std::string("null") : json(id).dump();
    }

    std::optional<std::vector<PrefabAuthoredEntry>> PrefabAuthoring::Read(std::span<const PrefabAuthoredKey> keys)
    {
        std::vector<PrefabAuthoredEntry> entries;
        entries.reserve(keys.size());

        const LoadedScene *scene = mCoordinator.Scene();
        for (const PrefabAuthoredKey &key : keys)
        {
            PrefabAuthoredEntry entry{ key, std::nullopt };

            if (key.mPrefab)
            {
                const PrefabDocument *document = mCoordinator.Registry().Find(*key.mPrefab);
                if (document == nullptr || key.mAddress.size() != 1)
                    return std::nullopt;

                const PrefabNode *node = FindNode(*document, key.mAddress);
                if (IsNodeKey(key))
                {
                    if (node != nullptr)
                        entry.mValue = NodeRecord(*document, *node).dump();
                }
                else if (IsFieldKey(key))
                {
                    const PrefabComponentInstance *component =
                        node != nullptr ? FindComponent(*node, key.mComponent) : nullptr;
                    if (component == nullptr)
                        return std::nullopt;
                    const auto it = component->mFields.find(key.mField);
                    if (it != component->mFields.end())
                        entry.mValue = it->dump();
                    else if (const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
                             ops == nullptr || ops->FindField(key.mField) == nullptr)
                    {
                        return std::nullopt;
                    }
                }
                else if (IsComponentKey(key))
                {
                    if (node == nullptr)
                        return std::nullopt;
                    if (const PrefabComponentInstance *component = FindComponent(*node, key.mComponent))
                        entry.mValue = component->mFields.dump();
                }
                else
                {
                    return std::nullopt;
                }
            }
            else
            {
                if (scene == nullptr || key.mAddress.size() != 2)
                    return std::nullopt;

                const StableId root = key.mAddress.front();
                const SceneNodeAddress node{ key.mAddress.back() };
                const auto instance = scene->mInstances.find(root);
                if (instance == scene->mInstances.end())
                    return std::nullopt;

                const auto binding = instance->second.mBindings.find(node);
                if (binding == instance->second.mBindings.end() ||
                    !SceneMemberAlive(*scene, mWorld, binding->second))
                {
                    return std::nullopt;
                }

                if (IsNodeKey(key))
                    return std::nullopt;

                if (IsFieldKey(key))
                {
                    const PrefabComponentState state = InstanceComponentState(mCoordinator.Registry(), *scene, root,
                        node, key.mComponent);
                    if (state == PrefabComponentState::Absent || state == PrefabComponentState::Removed)
                        return std::nullopt;

                    if (const SceneComponentRecord *added =
                            FindAddedComponent(instance->second, node, key.mComponent))
                    {
                        const auto it = added->mFields.find(key.mField);
                        if (it != added->mFields.end())
                            entry.mValue = it->dump();
                    }
                    else if (const SceneFieldPatch *patch =
                                 FindInstanceFieldPatch(instance->second, node, key.mComponent, key.mField))
                    {
                        entry.mValue = patch->mValue.dump();
                    }
                }
                else if (IsComponentKey(key))
                {
                    const PrefabComponentState state = InstanceComponentState(mCoordinator.Registry(), *scene, root,
                        node, key.mComponent);
                    if (state == PrefabComponentState::Added)
                        entry.mValue = FindAddedComponent(instance->second, node, key.mComponent)->mFields.dump();
                    else if (state == PrefabComponentState::Removed)
                        entry.mValue = "null";
                    else if (state == PrefabComponentState::Inherited)
                        entry.mValue = InheritedOverrides(instance->second, node, key.mComponent);
                }
                else
                {
                    return std::nullopt;
                }
            }

            entries.push_back(std::move(entry));
        }
        return entries;
    }

    PrefabAuthoredWrite PrefabAuthoring::Write(std::span<const PrefabAuthoredEntry> entries)
    {
        PrefabAuthoredWrite result;
        if (entries.empty())
        {
            result.mApplied = true;
            return result;
        }

        LoadedScene *scene = mCoordinator.Scene();
        std::map<PrefabId, PrefabDocument> documents;
        std::map<PrefabId, PrefabDocument> originals;
        std::vector<const PrefabAuthoredEntry *> sceneEntries;

        for (const PrefabAuthoredEntry &entry : entries)
        {
            if (entry.mKey.mPrefab)
            {
                if (documents.contains(*entry.mKey.mPrefab))
                    continue;
                const PrefabDocument *document = mCoordinator.Registry().Find(*entry.mKey.mPrefab);
                if (document == nullptr)
                    return result;
                documents.emplace(*entry.mKey.mPrefab, *document);
                originals.emplace(*entry.mKey.mPrefab, *document);
            }
            else
            {
                if (scene == nullptr)
                    return result;
                sceneEntries.push_back(&entry);
            }
        }

        std::map<PrefabId, std::vector<SceneNodeAddress>> removedNodes;
        for (auto &[id, document] : documents)
        {
            std::vector<const PrefabAuthoredEntry *> nodeInsertions;
            for (const PrefabAuthoredEntry &entry : entries)
            {
                if (!entry.mKey.mPrefab || *entry.mKey.mPrefab != id)
                    continue;
                if (IsNodeKey(entry.mKey) && entry.mValue)
                {
                    nodeInsertions.push_back(&entry);
                    continue;
                }
                if (!ApplyDocumentEntry(document, entry, removedNodes[id]))
                {
                    MIR_LOG_WARN("Prefab authoring: refused entry in \"{}\"", ToString(id));
                    return result;
                }
            }

            // Absolute indices from the original document; ascending keeps sibling order.
            const auto indexOf = [](const PrefabAuthoredEntry &entry)
                {
                    const json parsed = json::parse(*entry.mValue, nullptr, false);
                    if (parsed.is_object() && parsed.contains("index") && parsed["index"].is_number_unsigned())
                        return parsed["index"].get<uint64_t>();
                    return std::numeric_limits<uint64_t>::max();
                };
            std::stable_sort(nodeInsertions.begin(), nodeInsertions.end(),
                [&indexOf](const PrefabAuthoredEntry *a, const PrefabAuthoredEntry *b)
                {
                    return indexOf(*a) < indexOf(*b);
                });
            for (const PrefabAuthoredEntry *entry : nodeInsertions)
            {
                if (!ApplyDocumentEntry(document, *entry, removedNodes[id]))
                {
                    MIR_LOG_WARN("Prefab authoring: refused node entry in \"{}\"", ToString(id));
                    return result;
                }
            }

            const PrefabCompileResult compiled = CompilePrefab(document);
            if (!compiled.mPrefab)
            {
                for (const PrefabDiagnostic &diagnostic : compiled.mDiagnostics)
                    MIR_LOG_WARN("Prefab authoring: \"{}\": {}", ToString(id), diagnostic.mMessage);
                return result;
            }
        }

        std::vector<PrefabAuthoredEntry> sceneBatch;
        sceneBatch.reserve(sceneEntries.size());
        for (const PrefabAuthoredEntry *entry : sceneEntries)
            sceneBatch.push_back(*entry);

        std::optional<std::vector<PrefabAuthoredEntry>> sceneBefore;
        std::map<StableId, ScenePrefabInstance> instancesBefore;
        std::vector<ComponentBackup> componentBackups;
        if (!sceneBatch.empty())
        {
            std::vector<PrefabAuthoredKey> keys;
            keys.reserve(sceneBatch.size());
            for (const PrefabAuthoredEntry &entry : sceneBatch)
                keys.push_back(entry.mKey);
            sceneBefore = Read(keys);
            if (!sceneBefore)
                return result;

            for (const PrefabAuthoredEntry &entry : sceneBatch)
            {
                if (entry.mKey.mAddress.size() != 2)
                    continue;

                const StableId root = entry.mKey.mAddress.front();
                const SceneNodeAddress node{ entry.mKey.mAddress.back() };
                const auto instance = scene->mInstances.find(root);
                if (instance == scene->mInstances.end())
                    continue;
                instancesBefore.try_emplace(root, instance->second);

                const auto binding = instance->second.mBindings.find(node);
                if (binding == instance->second.mBindings.end())
                    continue;
                const auto member = scene->mEntities.find(binding->second);
                if (member == scene->mEntities.end())
                    continue;

                const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mKey.mComponent);
                if (ops == nullptr)
                    continue;

                ComponentBackup backup;
                backup.mEntity = member->second;
                backup.mOps = ops;
                backup.mExisted = ops->Has(mWorld, backup.mEntity);
                if (backup.mExisted && ops->mSize != 0)
                {
                    if (const void *component = ops->GetComponent(mWorld, backup.mEntity))
                    {
                        backup.mBytes.resize(ops->mSize);
                        std::memcpy(backup.mBytes.data(), component, ops->mSize);
                    }
                }
                componentBackups.push_back(std::move(backup));
            }
        }

        const bool sceneDirtyBefore = scene->mDirty;
        const auto unresolvedBefore = scene->mUnresolvedEntityRefs;
        const auto writeEdit = [&](const SceneEdit &edit)
            {
                return IsFieldKey(edit.mEntry->mKey)
                    ? WriteSceneField(mWorld, *scene, mCoordinator.Registry(), mCoordinator.Cache(), edit)
                    : WriteSceneComponent(mWorld, *scene, mCoordinator.Registry(), mCoordinator.Cache(), edit);
            };
        const auto rollbackScene = [&]()
            {
                if (!sceneBefore)
                    return;

                for (const auto &[root, record] : instancesBefore)
                    scene->mInstances[root] = record;
                scene->mUnresolvedEntityRefs = unresolvedBefore;

                for (const ComponentBackup &backup : componentBackups)
                {
                    if (backup.mOps->mSize == 0)
                    {
                        if (backup.mExisted && !backup.mOps->Has(mWorld, backup.mEntity))
                            backup.mOps->AddCopy(mWorld, backup.mEntity, nullptr);
                        else if (!backup.mExisted && backup.mOps->Has(mWorld, backup.mEntity))
                            backup.mOps->Remove(mWorld, backup.mEntity);
                        continue;
                    }

                    if (backup.mExisted)
                    {
                        if (!backup.mOps->Has(mWorld, backup.mEntity))
                            backup.mOps->AddCopy(mWorld, backup.mEntity, backup.mBytes.data());
                        if (void *component = backup.mOps->GetComponent(mWorld, backup.mEntity))
                            std::memcpy(component, backup.mBytes.data(), backup.mOps->mSize);
                    }
                    else if (backup.mOps->Has(mWorld, backup.mEntity))
                    {
                        backup.mOps->Remove(mWorld, backup.mEntity);
                    }
                }

                scene->mDirty = sceneDirtyBefore;
            };

        // Scene entries are checked against the documents as this batch leaves them.
        const std::function<const PrefabDocument *(PrefabId)> batchDocument =
            [&documents, this](PrefabId id) -> const PrefabDocument *
        {
            const auto edited = documents.find(id);
            return edited != documents.end() ? &edited->second : mCoordinator.Registry().Find(id);
        };
        std::vector<SceneEdit> sceneEdits;
        sceneEdits.reserve(sceneBatch.size());
        for (const PrefabAuthoredEntry &entry : sceneBatch)
        {
            SceneEdit edit;
            if (!PrepareSceneEdit(entry, *scene, mWorld, batchDocument, edit))
            {
                MIR_LOG_WARN("Prefab authoring: scene entry could not be written");
                return result;
            }
            sceneEdits.push_back(std::move(edit));
        }

        std::vector<PrefabId> writtenDocuments;
        for (const auto &[id, document] : documents)
        {
            if (!mCoordinator.SetDocument(document))
            {
                MIR_LOG_WARN("Prefab authoring: document \"{}\" was refused", ToString(id));
                for (auto it = writtenDocuments.rbegin(); it != writtenDocuments.rend(); ++it)
                    mCoordinator.SetDocument(originals.at(*it));
                return result;
            }
            writtenDocuments.push_back(id);
        }

        // Written after the documents, so the scene entries resolve the batch's templates.
        for (const SceneEdit &edit : sceneEdits)
        {
            if (!writeEdit(edit))
            {
                MIR_LOG_WARN("Prefab authoring: scene entry could not be written");
                for (auto it = writtenDocuments.rbegin(); it != writtenDocuments.rend(); ++it)
                    mCoordinator.SetDocument(originals.at(*it));
                rollbackScene();
                return result;
            }
        }

        for (const auto &[id, addresses] : removedNodes)
        {
            for (const SceneNodeAddress &address : addresses)
            {
                if (SceneBindsAddress(*scene, id, address))
                    result.mSceneMaintained = true;
            }
        }

        result.mApplied = true;
        return result;
    }

    PrefabAuthoredAction PrefabAuthoring::Revert(const PrefabAuthoredKey &key) const
    {
        PrefabAuthoredAction action;
        const LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr || key.mPrefab || key.mAddress.size() != 2)
        {
            action.mRefusal = "only an instance member has a local override to revert";
            return action;
        }

        const StableId root = key.mAddress.front();
        const SceneNodeAddress node{ key.mAddress.back() };
        const auto instance = scene->mInstances.find(root);
        if (instance == scene->mInstances.end() ||
            !instance->second.mBindings.contains(node))
        {
            action.mRefusal = "the instance member is missing";
            return action;
        }

        if (IsFieldKey(key))
        {
            const SceneComponentRecord *added = FindAddedComponent(instance->second, node, key.mComponent);
            const bool inAdded = added != nullptr && added->mFields.contains(key.mField);
            if (!inAdded && FindInstanceFieldPatch(instance->second, node, key.mComponent, key.mField) == nullptr)
            {
                action.mRefusal = "the field is not overridden";
                return action;
            }
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (!IsComponentKey(key))
        {
            action.mRefusal = "select a field or a component";
            return action;
        }

        const PrefabComponentState state =
            InstanceComponentState(mCoordinator.Registry(), *scene, root, node, key.mComponent);
        if (state != PrefabComponentState::Added && state != PrefabComponentState::Removed)
        {
            action.mRefusal = "the component has no local change to revert";
            return action;
        }
        action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
        return action;
    }

    PrefabAuthoredAction PrefabAuthoring::Apply(const PrefabAuthoredKey &key) const
    {
        PrefabAuthoredAction action;
        const LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr || key.mPrefab || key.mAddress.size() != 2)
        {
            action.mRefusal = "only an instance member can be applied to its prefab";
            return action;
        }

        const StableId root = key.mAddress.front();
        const SceneNodeAddress node{ key.mAddress.back() };
        const auto instanceIt = scene->mInstances.find(root);
        if (instanceIt == scene->mInstances.end())
        {
            action.mRefusal = "the instance is missing from this scene";
            return action;
        }
        const ScenePrefabInstance &instance = instanceIt->second;

        const auto binding = instance.mBindings.find(node);
        if (binding == instance.mBindings.end() || !SceneMemberAlive(*scene, mWorld, binding->second))
        {
            action.mRefusal = "the instance member is missing";
            return action;
        }

        const std::optional<PrefabId> prefab = ParsePrefabId(instance.mPrefab);
        if (!prefab)
        {
            action.mRefusal = "the instance has an invalid prefab id";
            return action;
        }
        const PrefabDocument *document = mCoordinator.Registry().Find(*prefab);
        const PrefabNode *documentNode = document != nullptr ? FindNode(*document, node) : nullptr;
        if (documentNode == nullptr)
        {
            action.mRefusal = "the prefab document has no such node";
            return action;
        }

        const PrefabAuthoredKey documentKey{ *prefab, node, key.mComponent, key.mField };

        if (IsFieldKey(key))
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
            const FieldDesc *field = ops != nullptr ? ops->FindField(key.mField) : nullptr;
            if (field == nullptr || field->ReadOnly())
            {
                action.mRefusal = "the field cannot be authored on a prefab";
                return action;
            }

            const json *value = nullptr;
            if (const SceneComponentRecord *added = FindAddedComponent(instance, node, key.mComponent))
            {
                const auto it = added->mFields.find(key.mField);
                if (it != added->mFields.end())
                    value = &it.value();
            }
            if (value == nullptr)
            {
                if (const SceneFieldPatch *patch =
                        FindInstanceFieldPatch(instance, node, key.mComponent, key.mField))
                {
                    value = &patch->mValue;
                }
            }
            if (value == nullptr)
            {
                action.mRefusal = "the field is not overridden";
                return action;
            }

            if (FindComponent(*documentNode, key.mComponent) == nullptr)
            {
                action.mRefusal = "apply the added component before its fields";
                return action;
            }

            json documentValue;
            std::string reason;
            if (field->mKind == FieldKind::EntityRef)
            {
                if (!TranslateEntityRef(*scene, instance, *value, documentValue, reason))
                {
                    action.mRefusal = "the field " + reason;
                    return action;
                }
            }
            else
            {
                FieldBuffer buffer{};
                if (!JsonToField(field->mKind, *value, buffer.mBytes))
                {
                    action.mRefusal = "the field value does not fit its type";
                    return action;
                }
                documentValue = *value;
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ documentKey, documentValue.dump() });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (!IsComponentKey(key))
        {
            action.mRefusal = "select a field or a component";
            return action;
        }

        const PrefabComponentState state = InstanceComponentState(mCoordinator.Registry(), *scene, root, node,
            key.mComponent);
        if (state == PrefabComponentState::Added)
        {
            if (FindComponent(*documentNode, key.mComponent) != nullptr)
            {
                action.mRefusal = "the prefab already has this component";
                return action;
            }

            const SceneComponentRecord *record = FindAddedComponent(instance, node, key.mComponent);
            if (record == nullptr)
            {
                action.mRefusal = "the component addition is missing";
                return action;
            }

            const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
            if (ops == nullptr)
            {
                action.mRefusal = "the component is not registered";
                return action;
            }

            json fields = json::object();
            for (auto it = record->mFields.begin(); it != record->mFields.end(); ++it)
            {
                const FieldDesc *field = ops->FindField(it.key());
                if (field == nullptr)
                {
                    action.mRefusal = "the component has an unknown field \"" + it.key() + "\"";
                    return action;
                }

                if (field->mKind == FieldKind::EntityRef)
                {
                    json documentValue;
                    std::string reason;
                    if (!TranslateEntityRef(*scene, instance, it.value(), documentValue, reason))
                    {
                        action.mRefusal = "field \"" + it.key() + "\" " + reason;
                        return action;
                    }
                    fields[it.key()] = std::move(documentValue);
                    continue;
                }

                FieldBuffer buffer{};
                if (!JsonToField(field->mKind, it.value(), buffer.mBytes))
                {
                    action.mRefusal = "field \"" + it.key() + "\" value does not fit its type";
                    return action;
                }
                fields[it.key()] = it.value();
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ documentKey, fields.dump() });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (state == PrefabComponentState::Removed)
        {
            if (FindComponent(*documentNode, key.mComponent) == nullptr)
            {
                action.mRefusal = "the prefab does not have this component";
                return action;
            }
            action.mEntries.push_back(PrefabAuthoredEntry{ documentKey, std::nullopt });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        action.mRefusal = "the component has no local change to apply";
        return action;
    }
}
