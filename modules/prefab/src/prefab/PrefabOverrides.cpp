/**
 * @file PrefabOverrides.cpp
 * @author Jackey Cen
 * @brief Field and component overrides of a single-level prefab scene instance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabOverrides.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/World.h"
#include "scene/ComponentJson.h"
#include "scene/SceneInstancePatch.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

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

        std::string SelectionText(const PrefabPatchSelection &selection)
        {
            const std::string target = "\"" + AddressText(selection.mTarget) + "\"";
            switch (selection.mKind)
            {
            case PrefabPatchKind::Field:
                return "entry " + target + " field \"" + selection.mComponent + "." + selection.mField + "\"";
            case PrefabPatchKind::AddedComponent:
                return "entry " + target + " added component \"" + selection.mComponent + "\"";
            case PrefabPatchKind::RemovedComponent:
                return "entry " + target + " removed component \"" + selection.mComponent + "\"";
            }
            return "entry";
        }

        void AddError(std::vector<PrefabDiagnostic> *diagnostics, std::string message)
        {
            if (diagnostics != nullptr)
                diagnostics->push_back({ PrefabDiagnosticSeverity::Error, {}, std::move(message) });
        }

        const PrefabDocument *FindTemplate(const PrefabRegistry &registry, const ScenePrefabInstance &instance)
        {
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
                return nullptr;
            return registry.Find(*id);
        }

        const PrefabNode *FindOwnNode(const PrefabDocument &document, PrefabNodeId id)
        {
            for (const PrefabNode &node : document.mNodes)
            {
                if (node.mId == id)
                    return &node;
            }
            return nullptr;
        }

        PrefabNode *FindOwnNode(PrefabDocument &document, PrefabNodeId id)
        {
            for (PrefabNode &node : document.mNodes)
            {
                if (node.mId == id)
                    return &node;
            }
            return nullptr;
        }

        const PrefabNode *FindDocumentNode(const PrefabRegistry &registry, const PrefabDocument &document,
            const SceneNodeAddress &address)
        {
            const PrefabDocument *current = &document;
            for (std::size_t i = 0; i < address.size(); ++i)
            {
                const PrefabNode *node = current != nullptr ? FindOwnNode(*current, address[i]) : nullptr;
                if (node == nullptr)
                    return nullptr;
                if (i + 1 == address.size())
                    return node->mKind == PrefabNodeKind::Entity ? node : nullptr;
                if (node->mKind != PrefabNodeKind::Prefab)
                    return nullptr;

                current = registry.Find(node->mPrefab);
                if (current == nullptr)
                    return nullptr;
            }
            return nullptr;
        }

        const PrefabComponentInstance *FindDocumentComponent(const PrefabNode &node, std::string_view type)
        {
            for (const PrefabComponentInstance &component : node.mComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        PrefabComponentInstance *FindDocumentComponent(PrefabNode &node, std::string_view type)
        {
            for (PrefabComponentInstance &component : node.mComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        // The entity node's own components, then each enclosing occurrence's patches from the innermost out,
        // in the order CompilePrefab applies them.
        bool TemplateHasComponent(const PrefabDocumentLookup &documents, const PrefabDocument &document,
            const SceneNodeAddress &address, std::string_view type)
        {
            std::vector<const PrefabNode *> occurrences;
            const PrefabDocument *current = &document;
            const PrefabNode *entity = nullptr;
            for (std::size_t i = 0; i < address.size() && current != nullptr; ++i)
            {
                const PrefabNode *node = FindOwnNode(*current, address[i]);
                if (node == nullptr)
                    return false;
                if (i + 1 == address.size())
                {
                    entity = node->mKind == PrefabNodeKind::Entity ? node : nullptr;
                    break;
                }
                if (node->mKind != PrefabNodeKind::Prefab)
                    return false;
                occurrences.push_back(node);
                current = documents(node->mPrefab);
            }
            if (entity == nullptr)
                return false;

            bool present = FindDocumentComponent(*entity, type) != nullptr;
            for (std::size_t level = occurrences.size(); level-- > 0;)
            {
                const SceneNodeAddress suffix(address.begin() + static_cast<std::ptrdiff_t>(level) + 1, address.end());
                for (const PrefabPatch &patch : occurrences[level]->mPatches)
                {
                    if (patch.mTarget != suffix)
                        continue;
                    for (const PrefabComponentInstance &added : patch.mAddedComponents)
                    {
                        if (added.mType == type)
                            present = true;
                    }
                    if (std::find(patch.mRemovedComponents.begin(), patch.mRemovedComponents.end(), type) !=
                        patch.mRemovedComponents.end())
                    {
                        present = false;
                    }
                }
            }
            return present;
        }

        bool TemplateHasComponent(const PrefabRegistry &registry, const PrefabDocument &document,
            const SceneNodeAddress &address, std::string_view type)
        {
            return TemplateHasComponent(RegistryLookup(registry), document, address, type);
        }

        bool MemberEntity(World &world, const LoadedScene &scene, const ScenePrefabInstance &instance,
            const SceneNodeAddress &node, Entity &outEntity)
        {
            const auto bindingIt = instance.mBindings.find(node);
            if (bindingIt == instance.mBindings.end())
                return false;

            const auto entityIt = scene.mEntities.find(bindingIt->second);
            if (entityIt == scene.mEntities.end() || !world.IsAlive(entityIt->second))
                return false;

            outEntity = entityIt->second;
            return true;
        }

        std::size_t FindCompiledNode(const CompiledPrefab &prefab, const SceneNodeAddress &address)
        {
            const std::span<const CompiledPrefabNode> nodes = prefab.Nodes();
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].mAddress == address)
                    return i;
            }
            return kNoIndex;
        }

        std::size_t FindCompiledComponent(const CompiledPrefabNode &node, std::string_view type)
        {
            for (std::size_t i = 0; i < node.mComponents.size(); ++i)
            {
                if (node.mComponents[i].mOps->mType.name == type)
                    return i;
            }
            return kNoIndex;
        }

        const PrefabEntityRefRelocation *FindRelocation(const CompiledPrefab &prefab, std::size_t node,
            std::size_t component, std::size_t field)
        {
            for (const PrefabEntityRefRelocation &relocation : prefab.Relocations())
            {
                if (relocation.mNode == node && relocation.mComponent == component && relocation.mField == field)
                    return &relocation;
            }
            return nullptr;
        }

        std::shared_ptr<const CompiledPrefab> FindCompiledTemplate(const PrefabRegistry &registry,
            CompiledPrefabCache &cache, const ScenePrefabInstance &instance,
            std::vector<PrefabDiagnostic> *diagnostics)
        {
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
            {
                AddError(diagnostics, "instance \"" + instance.mPrefab + "\" has an invalid prefab id");
                return nullptr;
            }

            if (std::shared_ptr<const CompiledPrefab> cached = cache.Find(*id))
                return cached;

            const PrefabDocument *document = registry.Find(*id);
            if (document == nullptr)
            {
                AddError(diagnostics, "prefab \"" + ToString(*id) + "\" is not registered");
                return nullptr;
            }

            const std::vector<PrefabDiagnostic> compiled = cache.Compile(*document, RegistryLookup(registry));
            if (HasPrefabErrors(compiled))
            {
                if (diagnostics != nullptr)
                    diagnostics->insert(diagnostics->end(), compiled.begin(), compiled.end());
                return nullptr;
            }
            return cache.Find(*id);
        }

        bool ValidateFieldValue(const FieldDesc &field, const json &value, std::string &reason)
        {
            if (field.mKind == FieldKind::EntityRef)
            {
                if (!StableIdFromJson(value))
                {
                    reason = "is not a StableId or null";
                    return false;
                }
                return true;
            }

            FieldBuffer buffer{};
            if (!JsonToField(field.mKind, value, buffer.mBytes))
            {
                reason = std::string("is not a ") + std::string(FieldKindName(field.mKind)) + " this field can hold";
                return false;
            }
            return true;
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

                outValue = json::array();
                for (uint64_t segment : address)
                    outValue.push_back(std::to_string(segment));
                return true;
            }

            reason = scene.mEntities.contains(*target)
                ? "targets an entity outside this instance"
                : "targets an entity the scene does not contain";
            return false;
        }

        void WriteTemplateEntityRef(World &world, LoadedScene &scene, const ScenePrefabInstance &instance,
            Entity entity, const ComponentOps &ops, const FieldDesc &field, const std::optional<PrefabNodeAddress> &target)
        {
            StableId owner = kNullStableId;
            for (const auto &[address, id] : instance.mBindings)
            {
                const auto entityIt = scene.mEntities.find(id);
                if (entityIt != scene.mEntities.end() && entityIt->second == entity)
                {
                    owner = id;
                    break;
                }
            }

            StableId targetId = kNullStableId;
            if (target)
            {
                const auto bindingIt = instance.mBindings.find(*target);
                if (bindingIt != instance.mBindings.end())
                    targetId = bindingIt->second;
            }

            Entity targetEntity = kNullEntity;
            if (targetId != kNullStableId)
            {
                const auto entityIt = scene.mEntities.find(targetId);
                if (entityIt != scene.mEntities.end() && world.IsAlive(entityIt->second))
                    targetEntity = entityIt->second;
            }

            void *bytes = ops.GetComponent(world, entity);
            if (bytes != nullptr)
                field.Write(bytes, &targetEntity);

            if (owner == kNullStableId)
                return;

            const auto key = std::make_tuple(owner, std::string(ops.mType.name), std::string(field.mName));
            scene.mUnresolvedEntityRefs.erase(key);
            if (targetId != kNullStableId && targetEntity.IsNull())
                scene.mUnresolvedEntityRefs[key] = targetId;
        }
    }

    PrefabFieldState InstanceFieldState(const LoadedScene &scene, StableId root, const SceneNodeAddress &node,
        std::string_view component, std::string_view field)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
            return PrefabFieldState::Inherited;

        return FindInstanceFieldPatch(instanceIt->second, node, component, field) != nullptr
            ? PrefabFieldState::Overridden
            : PrefabFieldState::Inherited;
    }

    PrefabComponentState InstanceComponentState(const PrefabRegistry &registry, const LoadedScene &scene,
        StableId root, const SceneNodeAddress &node, std::string_view component)
    {
        return InstanceComponentState(RegistryLookup(registry), scene, root, node, component);
    }

    PrefabComponentState InstanceComponentState(const PrefabDocumentLookup &documents, const LoadedScene &scene,
        StableId root, const SceneNodeAddress &node, std::string_view component)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
            return PrefabComponentState::Absent;

        const ScenePrefabInstance &instance = instanceIt->second;
        if (FindAddedComponent(instance, node, component) != nullptr)
            return PrefabComponentState::Added;
        if (HasRemovedComponent(instance, node, component))
            return PrefabComponentState::Removed;

        const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
        const PrefabDocument *document = id ? documents(*id) : nullptr;
        if (document == nullptr)
            return PrefabComponentState::Absent;

        if (!TemplateHasComponent(documents, *document, node, component))
            return PrefabComponentState::Absent;
        return PrefabComponentState::Inherited;
    }

    bool SetInstanceFieldOverride(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component, std::string_view field,
        const json &value, std::vector<PrefabDiagnostic> *diagnostics)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " is not in this scene");
            return false;
        }
        ScenePrefabInstance &instance = instanceIt->second;

        const PrefabDocument *document = FindTemplate(registry, instance);
        if (document == nullptr)
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " has no prefab document");
            return false;
        }

        const PrefabNode *templateNode = FindDocumentNode(registry, *document, node);
        if (templateNode == nullptr)
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not an entity node of the prefab");
            return false;
        }

        const ComponentOps *ops = ComponentRegistry::Instance().Find(component);
        if (ops == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is not registered");
            return false;
        }

        const FieldDesc *fieldDesc = ops->FindField(field);
        if (fieldDesc == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" has no field \"" +
                std::string(field) + "\"");
            return false;
        }
        if (fieldDesc->ReadOnly())
        {
            AddError(diagnostics, "field \"" + std::string(component) + "." + std::string(field) + "\" is read-only");
            return false;
        }

        Entity entity = kNullEntity;
        if (!MemberEntity(world, scene, instance, node, entity))
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not a live member of instance " +
                std::to_string(root));
            return false;
        }

        SceneComponentRecord *added = FindAddedComponent(instance, node, component);
        const bool inherited = TemplateHasComponent(registry, *document, node, component);
        if (!inherited && added == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is not part of node \"" +
                AddressText(node) + "\"");
            return false;
        }

        if (fieldDesc->mKind == FieldKind::EntityRef)
        {
            const std::optional<StableId> target = StableIdFromJson(value);
            if (!target)
            {
                AddError(diagnostics, "field \"" + std::string(component) + "." + std::string(field) +
                    "\" value is not a StableId or null");
                return false;
            }
            if (*target != kNullStableId)
            {
                const auto targetIt = scene.mEntities.find(*target);
                if (targetIt == scene.mEntities.end() || !world.IsAlive(targetIt->second))
                {
                    AddError(diagnostics, "field \"" + std::string(component) + "." + std::string(field) +
                        "\" references a StableId the scene does not contain");
                    return false;
                }
            }
            if (!ops->Has(world, entity))
            {
                AddError(diagnostics, "component \"" + std::string(component) + "\" is removed from node \"" +
                    AddressText(node) + "\"");
                return false;
            }
            SetInstanceEntityRef(world, scene, root, node, component, field, *target);
            return true;
        }

        std::string reason;
        if (!ValidateFieldValue(*fieldDesc, value, reason))
        {
            AddError(diagnostics, "field \"" + std::string(component) + "." + std::string(field) + "\" value " +
                reason);
            return false;
        }

        if (!ops->Has(world, entity))
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is removed from node \"" +
                AddressText(node) + "\"");
            return false;
        }

        void *componentBytes = ops->GetComponent(world, entity);
        if (componentBytes == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" cannot be read on node \"" +
                AddressText(node) + "\"");
            return false;
        }

        FieldBuffer buffer{};
        JsonToField(fieldDesc->mKind, value, buffer.mBytes);
        if (!fieldDesc->Write(componentBytes, buffer.mBytes))
        {
            AddError(diagnostics, "field \"" + std::string(component) + "." + std::string(field) +
                "\" cannot be written");
            return false;
        }

        if (added != nullptr)
        {
            added->mFields[std::string(field)] = value;
        }
        else if (SceneFieldPatch *patch = FindInstanceFieldPatch(instance, node, component, field))
        {
            patch->mValue = value;
        }
        else
        {
            GetOrAddInstancePatch(instance, node).mFields.push_back(SceneFieldPatch{
                std::string(component), std::string(field), value });
        }

        scene.mDirty = true;
        return true;
    }

    bool AddInstanceComponent(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component, std::vector<PrefabDiagnostic> *diagnostics)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " is not in this scene");
            return false;
        }
        ScenePrefabInstance &instance = instanceIt->second;

        const PrefabDocument *document = FindTemplate(registry, instance);
        if (document == nullptr)
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " has no prefab document");
            return false;
        }

        const PrefabNode *templateNode = FindDocumentNode(registry, *document, node);
        if (templateNode == nullptr)
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not an entity node of the prefab");
            return false;
        }

        const ComponentOps *ops = ComponentRegistry::Instance().Find(component);
        if (ops == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is not registered");
            return false;
        }

        if (FindAddedComponent(instance, node, component) != nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is already added to node \"" +
                AddressText(node) + "\"");
            return false;
        }
        if (TemplateHasComponent(registry, *document, node, component))
        {
            AddError(diagnostics, "component \"" + std::string(component) +
                "\" is part of the prefab node; revert its removal instead");
            return false;
        }

        Entity entity = kNullEntity;
        if (!MemberEntity(world, scene, instance, node, entity))
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not a live member of instance " +
                std::to_string(root));
            return false;
        }
        // The pre-batch template may still supply it until the batch's documents commit.
        if (!ops->Has(world, entity))
            ops->AddDefault(world, entity);
        GetOrAddInstancePatch(instance, node).mAddedComponents.push_back(SceneComponentRecord{
            std::string(component), json::object() });
        scene.mDirty = true;
        return true;
    }

    bool RemoveInstanceComponent(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component, std::vector<PrefabDiagnostic> *diagnostics)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " is not in this scene");
            return false;
        }
        ScenePrefabInstance &instance = instanceIt->second;

        const PrefabDocument *document = FindTemplate(registry, instance);
        if (document == nullptr)
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " has no prefab document");
            return false;
        }

        const PrefabNode *templateNode = FindDocumentNode(registry, *document, node);
        if (templateNode == nullptr)
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not an entity node of the prefab");
            return false;
        }

        const ComponentOps *ops = ComponentRegistry::Instance().Find(component);
        if (ops == nullptr)
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is not registered");
            return false;
        }

        Entity entity = kNullEntity;
        if (!MemberEntity(world, scene, instance, node, entity))
        {
            AddError(diagnostics, "node \"" + AddressText(node) + "\" is not a live member of instance " +
                std::to_string(root));
            return false;
        }

        if (FindAddedComponent(instance, node, component) != nullptr)
        {
            ops->Remove(world, entity);
            EraseAddedComponent(instance, node, component);
            scene.mDirty = true;
            return true;
        }

        if (HasRemovedComponent(instance, node, component))
            return false;

        if (!TemplateHasComponent(registry, *document, node, component))
        {
            AddError(diagnostics, "component \"" + std::string(component) + "\" is not part of node \"" +
                AddressText(node) + "\"");
            return false;
        }
        // The pre-batch template may already have removed it until the batch's documents commit.
        if (ops->Has(world, entity))
            ops->Remove(world, entity);

        SceneInstancePatch &patch = GetOrAddInstancePatch(instance, node);
        patch.mRemovedComponents.push_back(std::string(component));
        std::erase_if(patch.mFields, [&](const SceneFieldPatch &fieldPatch)
            {
                return fieldPatch.mComponent == component;
            });

        for (const FieldDesc &field : ops->mFields)
            scene.mUnresolvedEntityRefs.erase(
                { instance.mBindings.at(node), std::string(component), std::string(field.mName) });

        scene.mDirty = true;
        return true;
    }

    bool RevertInstancePatch(World &world, const PrefabRegistry &registry, CompiledPrefabCache &cache,
        LoadedScene &scene, StableId root, const PrefabPatchSelection &selection,
        std::vector<PrefabDiagnostic> *diagnostics)
    {
        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
        {
            AddError(diagnostics, "instance " + std::to_string(root) + " is not in this scene");
            return false;
        }
        ScenePrefabInstance &instance = instanceIt->second;

        Entity entity = kNullEntity;
        if (!MemberEntity(world, scene, instance, selection.mTarget, entity))
        {
            AddError(diagnostics, "node \"" + AddressText(selection.mTarget) +
                "\" is not a live member of instance " + std::to_string(root));
            return false;
        }

        const std::shared_ptr<const CompiledPrefab> prefab =
            FindCompiledTemplate(registry, cache, instance, diagnostics);
        if (!prefab)
            return false;

        const std::size_t nodeIndex = FindCompiledNode(*prefab, selection.mTarget);
        if (nodeIndex == kNoIndex)
        {
            AddError(diagnostics, "node \"" + AddressText(selection.mTarget) + "\" is not in the prefab template");
            return false;
        }
        const CompiledPrefabNode &prefabNode = prefab->Nodes()[nodeIndex];

        switch (selection.mKind)
        {
        case PrefabPatchKind::Field:
        {
            if (FindInstanceFieldPatch(instance, selection.mTarget, selection.mComponent, selection.mField) == nullptr)
            {
                AddError(diagnostics, SelectionText(selection) + " has no field override");
                return false;
            }

            const std::size_t componentIndex = FindCompiledComponent(prefabNode, selection.mComponent);
            if (componentIndex == kNoIndex)
            {
                AddError(diagnostics, "component \"" + selection.mComponent + "\" is not in the prefab template");
                return false;
            }
            const CompiledPrefabComponent &prefabComponent = prefabNode.mComponents[componentIndex];
            const ComponentOps &ops = *prefabComponent.mOps;
            const FieldDesc *field = ops.FindField(selection.mField);
            if (field == nullptr || field->ReadOnly())
            {
                AddError(diagnostics, "field \"" + selection.mComponent + "." + selection.mField +
                    "\" is not a writable template field");
                return false;
            }

            void *componentBytes = ops.GetComponent(world, entity);
            if (componentBytes == nullptr)
            {
                AddError(diagnostics, "node \"" + AddressText(selection.mTarget) + "\" does not have component \"" +
                    selection.mComponent + "\"");
                return false;
            }

            if (field->mKind == FieldKind::EntityRef)
            {
                const std::size_t fieldIndex = static_cast<std::size_t>(field - ops.mFields.data());
                const PrefabEntityRefRelocation *relocation =
                    FindRelocation(*prefab, nodeIndex, componentIndex, fieldIndex);
                std::optional<PrefabNodeAddress> target;
                if (relocation != nullptr)
                    target = prefab->Nodes()[relocation->mTargetNode].mAddress;
                WriteTemplateEntityRef(world, scene, instance, entity, ops, *field, target);
            }
            else
            {
                FieldBuffer buffer{};
                field->Read(prefabComponent.mBytes.data(), buffer.mBytes);
                if (!field->Write(componentBytes, buffer.mBytes))
                {
                    AddError(diagnostics, "field \"" + selection.mComponent + "." + selection.mField +
                        "\" cannot be written");
                    return false;
                }
            }

            EraseInstanceFieldPatch(instance, selection.mTarget, selection.mComponent, selection.mField);
            scene.mDirty = true;
            return true;
        }
        case PrefabPatchKind::AddedComponent:
        {
            if (FindAddedComponent(instance, selection.mTarget, selection.mComponent) == nullptr)
            {
                AddError(diagnostics, SelectionText(selection) + " is not a local component addition");
                return false;
            }

            const ComponentOps *ops = ComponentRegistry::Instance().Find(selection.mComponent);
            if (ops == nullptr)
            {
                AddError(diagnostics, "component \"" + selection.mComponent + "\" is not registered");
                return false;
            }

            ops->Remove(world, entity);
            EraseAddedComponent(instance, selection.mTarget, selection.mComponent);
            scene.mDirty = true;
            return true;
        }
        case PrefabPatchKind::RemovedComponent:
        {
            if (!HasRemovedComponent(instance, selection.mTarget, selection.mComponent))
            {
                AddError(diagnostics, SelectionText(selection) + " is not a removed inherited component");
                return false;
            }

            const std::size_t componentIndex = FindCompiledComponent(prefabNode, selection.mComponent);
            if (componentIndex == kNoIndex)
            {
                AddError(diagnostics, "component \"" + selection.mComponent + "\" is not in the prefab template");
                return false;
            }
            const CompiledPrefabComponent &prefabComponent = prefabNode.mComponents[componentIndex];
            const ComponentOps &ops = *prefabComponent.mOps;
            if (ops.Has(world, entity))
            {
                AddError(diagnostics, "node \"" + AddressText(selection.mTarget) + "\" already has component \"" +
                    selection.mComponent + "\"");
                return false;
            }

            ops.AddCopy(world, entity, prefabComponent.mBytes.data());
            for (const PrefabEntityRefRelocation &relocation : prefab->Relocations())
            {
                if (relocation.mNode != nodeIndex || relocation.mComponent != componentIndex)
                    continue;

                const FieldDesc &field = ops.mFields[relocation.mField];
                const PrefabNodeAddress &target = prefab->Nodes()[relocation.mTargetNode].mAddress;
                WriteTemplateEntityRef(world, scene, instance, entity, ops, field, target);
            }

            EraseRemovedComponent(instance, selection.mTarget, selection.mComponent);
            scene.mDirty = true;
            return true;
        }
        }
        return false;
    }

    PrefabApplyResult ApplyPrefabInstancePatches(LoadedScene &scene, PrefabRegistry &registry,
        CompiledPrefabCache &cache, StableId root, std::span<const PrefabPatchSelection> selections)
    {
        PrefabApplyResult result;
        if (selections.empty())
            return result;

        const auto instanceIt = scene.mInstances.find(root);
        if (instanceIt == scene.mInstances.end())
        {
            AddError(&result.mDiagnostics, "instance " + std::to_string(root) + " is not in this scene");
            return result;
        }
        ScenePrefabInstance &instance = instanceIt->second;

        const PrefabDocument *document = FindTemplate(registry, instance);
        if (document == nullptr)
        {
            AddError(&result.mDiagnostics, "instance " + std::to_string(root) + " has no prefab document");
            return result;
        }

        const PrefabDocument original = *document;
        PrefabDocument copy = original;

        for (const PrefabPatchSelection &selection : selections)
        {
            const std::string entry = SelectionText(selection);
            if (!instance.mBindings.contains(selection.mTarget))
            {
                AddError(&result.mDiagnostics, entry + " targets a node the instance does not bind");
                return result;
            }

            if (selection.mTarget.size() != 1)
            {
                AddError(&result.mDiagnostics, entry +
                    " is a member of a nested prefab occurrence; edit its inner Prefab document instead");
                return result;
            }

            PrefabNode *node = FindOwnNode(copy, selection.mTarget.front());
            if (node == nullptr || node->mKind != PrefabNodeKind::Entity)
            {
                AddError(&result.mDiagnostics, entry + " targets a node that is not in the prefab document");
                return result;
            }

            switch (selection.mKind)
            {
            case PrefabPatchKind::Field:
            {
                const SceneFieldPatch *patch =
                    FindInstanceFieldPatch(instance, selection.mTarget, selection.mComponent, selection.mField);
                if (patch == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " has no field override");
                    return result;
                }

                const ComponentOps *ops = ComponentRegistry::Instance().Find(selection.mComponent);
                if (ops == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " names an unregistered component");
                    return result;
                }
                const FieldDesc *field = ops->FindField(selection.mField);
                if (field == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " names an unknown field");
                    return result;
                }

                PrefabComponentInstance *component = FindDocumentComponent(*node, selection.mComponent);
                if (component == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " names a component the prefab node does not have");
                    return result;
                }

                json documentValue;
                std::string reason;
                if (field->mKind == FieldKind::EntityRef)
                {
                    if (!TranslateEntityRef(scene, instance, patch->mValue, documentValue, reason))
                    {
                        AddError(&result.mDiagnostics, entry + " " + reason);
                        return result;
                    }

                    const std::optional<StableId> targetId = StableIdFromJson(patch->mValue);
                    if (targetId && *targetId != kNullStableId)
                    {
                        SceneNodeAddress targetAddress;
                        for (const auto &[address, id] : instance.mBindings)
                        {
                            if (id == *targetId)
                            {
                                targetAddress = address;
                                break;
                            }
                        }
                        if (FindDocumentNode(registry, copy, targetAddress) == nullptr)
                        {
                            AddError(&result.mDiagnostics, entry +
                                " targets a node that is not in the prefab document");
                            return result;
                        }
                    }
                }
                else
                {
                    if (!ValidateFieldValue(*field, patch->mValue, reason))
                    {
                        AddError(&result.mDiagnostics, entry + " " + reason);
                        return result;
                    }
                    documentValue = patch->mValue;
                }

                component->mFields[selection.mField] = std::move(documentValue);
                break;
            }
            case PrefabPatchKind::AddedComponent:
            {
                const SceneComponentRecord *record =
                    FindAddedComponent(instance, selection.mTarget, selection.mComponent);
                if (record == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " is not a local component addition");
                    return result;
                }

                if (FindDocumentComponent(*node, selection.mComponent) != nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " names a component the prefab node already has");
                    return result;
                }

                const ComponentOps *ops = ComponentRegistry::Instance().Find(selection.mComponent);
                if (ops == nullptr)
                {
                    AddError(&result.mDiagnostics, entry + " names an unregistered component");
                    return result;
                }

                PrefabComponentInstance added;
                added.mType = selection.mComponent;
                for (auto fieldIt = record->mFields.begin(); fieldIt != record->mFields.end(); ++fieldIt)
                {
                    const FieldDesc *field = ops->FindField(fieldIt.key());
                    if (field == nullptr)
                    {
                        AddError(&result.mDiagnostics, entry + " names an unknown field \"" + fieldIt.key() + "\"");
                        return result;
                    }

                    json documentValue = fieldIt.value();
                    std::string reason;
                    if (field->mKind == FieldKind::EntityRef)
                    {
                        if (!TranslateEntityRef(scene, instance, fieldIt.value(), documentValue, reason))
                        {
                            AddError(&result.mDiagnostics, entry + " field \"" + fieldIt.key() + "\" " + reason);
                            return result;
                        }
                    }
                    else if (!ValidateFieldValue(*field, fieldIt.value(), reason))
                    {
                        AddError(&result.mDiagnostics, entry + " field \"" + fieldIt.key() + "\" " + reason);
                        return result;
                    }

                    added.mFields[fieldIt.key()] = std::move(documentValue);
                }

                node->mComponents.push_back(std::move(added));
                break;
            }
            case PrefabPatchKind::RemovedComponent:
            {
                if (!HasRemovedComponent(instance, selection.mTarget, selection.mComponent))
                {
                    AddError(&result.mDiagnostics, entry + " is not a removed inherited component");
                    return result;
                }

                const std::size_t before = node->mComponents.size();
                std::erase_if(node->mComponents, [&](const PrefabComponentInstance &component)
                    {
                        return component.mType == selection.mComponent;
                    });
                if (node->mComponents.size() == before)
                {
                    AddError(&result.mDiagnostics, entry + " names a component the prefab node does not have");
                    return result;
                }
                break;
            }
            }
        }

        const PrefabDocumentLookup lookup = RegistryLookup(registry);
        const PrefabCompileResult compiled = CompilePrefab(copy, lookup);
        if (!compiled.mPrefab)
        {
            result.mDiagnostics.insert(result.mDiagnostics.end(),
                compiled.mDiagnostics.begin(), compiled.mDiagnostics.end());
            return result;
        }

        std::vector<PrefabDiagnostic> setDiagnostics;
        if (!registry.SetDocument(copy, &setDiagnostics))
        {
            result.mDiagnostics.insert(result.mDiagnostics.end(),
                setDiagnostics.begin(), setDiagnostics.end());
            return result;
        }

        const std::vector<PrefabDiagnostic> cacheDiagnostics = cache.Compile(copy, lookup);
        if (HasPrefabErrors(cacheDiagnostics))
        {
            std::vector<PrefabDiagnostic> rollbackDiagnostics;
            registry.SetDocument(original, &rollbackDiagnostics);
            result.mDiagnostics.insert(result.mDiagnostics.end(),
                cacheDiagnostics.begin(), cacheDiagnostics.end());
            return result;
        }

        std::vector<PrefabId> dependents = cache.Dependents(copy.mId);
        std::stable_sort(dependents.begin(), dependents.end(), [&cache](PrefabId a, PrefabId b)
            {
                const std::shared_ptr<const CompiledPrefab> first = cache.Find(a);
                const std::shared_ptr<const CompiledPrefab> second = cache.Find(b);
                const std::size_t firstCount = first ? first->Dependencies().size() : 0;
                const std::size_t secondCount = second ? second->Dependencies().size() : 0;
                return firstCount < secondCount;
            });
        for (PrefabId dependent : dependents)
        {
            const PrefabDocument *document = registry.Find(dependent);
            if (document == nullptr)
                continue;
            const std::vector<PrefabDiagnostic> dependentDiagnostics = cache.Compile(*document, lookup);
            result.mDiagnostics.insert(result.mDiagnostics.end(),
                dependentDiagnostics.begin(), dependentDiagnostics.end());
        }

        for (const PrefabPatchSelection &selection : selections)
        {
            switch (selection.mKind)
            {
            case PrefabPatchKind::Field:
                EraseInstanceFieldPatch(instance, selection.mTarget, selection.mComponent, selection.mField);
                break;
            case PrefabPatchKind::AddedComponent:
                EraseAddedComponent(instance, selection.mTarget, selection.mComponent);
                break;
            case PrefabPatchKind::RemovedComponent:
                EraseRemovedComponent(instance, selection.mTarget, selection.mComponent);
                break;
            }
        }

        scene.mDirty = true;
        result.mApplied = true;
        return result;
    }
}
