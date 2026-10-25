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
#include "core/ecs/EntityPresetRegistry.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/ecs/components/Transform.h"
#include "core/log/Log.h"
#include "prefab/CompiledPrefab.h"
#include "prefab/PrefabFormat.h"
#include "scene/ComponentJson.h"
#include "scene/SceneInstancePatch.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <filesystem>
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

        const PrefabNode *FindDocumentNode(const PrefabRegistry &registry, const PrefabDocument &document,
            const SceneNodeAddress &address)
        {
            const PrefabDocument *current = &document;
            for (std::size_t i = 0; i < address.size(); ++i)
            {
                const std::optional<std::size_t> index =
                    current != nullptr ? FindNodeIndex(*current, address[i]) : std::nullopt;
                if (!index)
                    return nullptr;

                const PrefabNode &node = current->mNodes[*index];
                if (i + 1 == address.size())
                    return node.mKind == PrefabNodeKind::Entity ? &node : nullptr;
                if (node.mKind != PrefabNodeKind::Prefab)
                    return nullptr;

                current = registry.Find(node.mPrefab);
                if (current == nullptr)
                    return nullptr;
            }
            return nullptr;
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

        PrefabInstanceRole AddressRole(const PrefabRegistry &registry, const PrefabDocument *document,
            const SceneNodeAddress &address)
        {
            if (document == nullptr || address.size() < 2)
                return PrefabInstanceRole::Ordinary;

            const PrefabDocument *innermost = document;
            for (std::size_t i = 0; i + 1 < address.size(); ++i)
            {
                const std::optional<std::size_t> index = FindNodeIndex(*innermost, address[i]);
                if (!index || innermost->mNodes[*index].mKind != PrefabNodeKind::Prefab)
                    return PrefabInstanceRole::Ordinary;
                innermost = registry.Find(innermost->mNodes[*index].mPrefab);
                if (innermost == nullptr)
                    return PrefabInstanceRole::Ordinary;
            }
            return innermost->mRootNode == address.back() ? PrefabInstanceRole::Root : PrefabInstanceRole::Member;
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

        std::vector<PrefabNodeId> DescendantsOf(const PrefabDocument &document, PrefabNodeId root)
        {
            std::vector<PrefabNodeId> descendants;
            std::vector<PrefabNodeId> pending{ root };
            while (!pending.empty())
            {
                const PrefabNodeId parent = pending.back();
                pending.pop_back();
                for (const PrefabNode &node : document.mNodes)
                {
                    if (node.mParent != parent)
                        continue;
                    descendants.push_back(node.mId);
                    pending.push_back(node.mId);
                }
            }
            return descendants;
        }

        // Index in the array after the moving node is gone, so a record's index
        // survives the erase-then-insert in ApplyDocumentEntry.
        std::size_t InsertionIndex(const PrefabDocument &document, PrefabNodeId parent, std::size_t siblingIndex,
            PrefabNodeId moving)
        {
            std::vector<PrefabNodeId> order;
            order.reserve(document.mNodes.size());
            for (const PrefabNode &node : document.mNodes)
            {
                if (node.mId != moving)
                    order.push_back(node.mId);
            }

            const auto indexOf = [&order](PrefabNodeId id)
                {
                    const auto it = std::find(order.begin(), order.end(), id);
                    return it == order.end() ? order.size()
                                             : static_cast<std::size_t>(std::distance(order.begin(), it));
                };

            std::vector<PrefabNodeId> siblings;
            for (PrefabNodeId id : order)
            {
                const auto it = std::find_if(document.mNodes.begin(), document.mNodes.end(),
                    [id](const PrefabNode &node) { return node.mId == id; });
                if (it != document.mNodes.end() && it->mParent == parent)
                    siblings.push_back(id);
            }

            if (siblingIndex < siblings.size())
                return indexOf(siblings[siblingIndex]);
            if (!siblings.empty())
                return indexOf(siblings.back()) + 1;
            return std::min(indexOf(parent) + 1, order.size());
        }

        json NewOccurrenceRecord(const PrefabDocument &document, PrefabNodeId id, PrefabNodeId parent,
            PrefabId prefab, std::size_t siblingIndex, const std::optional<WorldVec3> &position,
            PrefabNodeId innerRoot)
        {
            PrefabNode node;
            node.mId = id;
            node.mKind = PrefabNodeKind::Prefab;
            node.mParent = parent;
            node.mPrefab = prefab;

            if (position)
            {
                PrefabPatch patch;
                patch.mTarget = { innerRoot };
                patch.mFields.push_back(PrefabFieldOverride{
                    "Transform", "position", json::array({ position->x, position->y, position->z }) });
                node.mPatches.push_back(std::move(patch));
            }

            json record = PrefabNodeToJson(document, node);
            record["index"] = InsertionIndex(document, parent, siblingIndex, kNullPrefabNodeId);
            return record;
        }

        json NewNodeRecord(const PrefabDocument &document, PrefabNodeId id, PrefabNodeId parent,
            std::size_t siblingIndex, const EntityPresetDesc *preset)
        {
            PrefabNode node;
            node.mId = id;
            node.mKind = PrefabNodeKind::Entity;
            node.mParent = parent;

            if (preset != nullptr && preset->mCreate != nullptr)
            {
                World scratch;
                const Entity entity = scratch.CreateEntity();
                preset->mCreate(scratch, entity);

                for (const ComponentOps *ops : ComponentRegistry::Instance().All())
                {
                    if (!ops->Has(scratch, entity))
                        continue;

                    const void *component = ops->GetComponent(scratch, entity);
                    PrefabComponentInstance instance;
                    instance.mType = std::string(ops->mType.name);
                    for (const FieldDesc &field : ops->mFields)
                    {
                        if (field.ReadOnly())
                            continue;
                        // A preset creates one entity, so a reference has no node to point at.
                        if (field.mKind == FieldKind::EntityRef)
                        {
                            instance.mFields[std::string(field.mName)] = nullptr;
                            continue;
                        }

                        FieldBuffer buffer{};
                        field.Read(component, buffer.mBytes);
                        instance.mFields[std::string(field.mName)] = FieldToJson(field.mKind, buffer.mBytes);
                    }
                    node.mComponents.push_back(std::move(instance));
                }
            }
            else
            {
                node.mComponents.push_back(PrefabComponentInstance{ "Transform", json::object() });
                node.mComponents.push_back(PrefabComponentInstance{ "WorldTransform", json::object() });
                node.mComponents.push_back(
                    PrefabComponentInstance{ "Name", json{ { "name", "Entity" } } });
            }

            json record = PrefabNodeToJson(document, node);
            record["index"] = InsertionIndex(document, parent, siblingIndex, kNullPrefabNodeId);
            return record;
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
            json record = PrefabNodeToJson(document, node);
            const std::optional<std::size_t> index = FindNodeIndex(document, node.mId);
            record["index"] = index.value_or(document.mNodes.size());
            return record;
        }

        bool ReadNodeRecord(const json &record, PrefabNode &out, std::string &reason)
        {
            json normalized = record;
            if (!normalized.contains("id"))
                normalized["id"] = std::to_string(out.mId);
            if (!normalized.contains("kind"))
                normalized["kind"] = "entity";

            std::vector<PrefabDiagnostic> diagnostics;
            const std::optional<PrefabNode> parsed = ParsePrefabNode(normalized, diagnostics);
            if (!parsed)
            {
                reason = diagnostics.empty() ? "a node record is malformed" : diagnostics.front().mMessage;
                return false;
            }
            if (parsed->mId != out.mId)
            {
                reason = "node record id " + std::to_string(parsed->mId) + " does not match the key";
                return false;
            }

            out = *parsed;
            if (out.mKind != PrefabNodeKind::Entity)
                return true;

            for (const PrefabComponentInstance &component : out.mComponents)
            {
                std::string fieldReason;
                if (!ValidateDocumentFields(component.mType, component.mFields, fieldReason))
                {
                    reason = std::move(fieldReason);
                    return false;
                }
            }
            return true;
        }

        const PrefabPatch *FindPatch(const PrefabNode &node, const SceneNodeAddress &target)
        {
            for (const PrefabPatch &patch : node.mPatches)
            {
                if (patch.mTarget == target)
                    return &patch;
            }
            return nullptr;
        }

        PrefabPatch *FindPatch(PrefabNode &node, const SceneNodeAddress &target)
        {
            for (PrefabPatch &patch : node.mPatches)
            {
                if (patch.mTarget == target)
                    return &patch;
            }
            return nullptr;
        }

        PrefabPatch &GetOrAddPatch(PrefabNode &node, const SceneNodeAddress &target)
        {
            if (PrefabPatch *existing = FindPatch(node, target))
                return *existing;

            PrefabPatch patch;
            patch.mTarget = target;
            node.mPatches.push_back(std::move(patch));
            return node.mPatches.back();
        }

        void EraseEmptyPatch(PrefabNode &node, const SceneNodeAddress &target)
        {
            std::erase_if(node.mPatches, [&target](const PrefabPatch &patch)
                {
                    return patch.mTarget == target && patch.mFields.empty() &&
                        patch.mAddedComponents.empty() && patch.mRemovedComponents.empty();
                });
        }

        const PrefabComponentInstance *FindAddedComponent(const PrefabPatch &patch, std::string_view type)
        {
            for (const PrefabComponentInstance &component : patch.mAddedComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        PrefabComponentInstance *FindAddedComponent(PrefabPatch &patch, std::string_view type)
        {
            for (PrefabComponentInstance &component : patch.mAddedComponents)
            {
                if (component.mType == type)
                    return &component;
            }
            return nullptr;
        }

        const PrefabFieldOverride *FindPatchField(const PrefabPatch &patch, std::string_view component,
            std::string_view field)
        {
            for (const PrefabFieldOverride &entry : patch.mFields)
            {
                if (entry.mComponent == component && entry.mField == field)
                    return &entry;
            }
            return nullptr;
        }

        PrefabFieldOverride *FindPatchField(PrefabPatch &patch, std::string_view component, std::string_view field)
        {
            for (PrefabFieldOverride &entry : patch.mFields)
            {
                if (entry.mComponent == component && entry.mField == field)
                    return &entry;
            }
            return nullptr;
        }

        Transform ReadTransform(const json &fields)
        {
            Transform transform;
            for (const FieldDesc &field : kTransformFields)
            {
                const auto value = fields.find(std::string(field.mName));
                FieldBuffer buffer{};
                if (value != fields.end() && JsonToField(field.mKind, *value, buffer.mBytes))
                    field.Write(&transform, buffer.mBytes);
            }
            return transform;
        }

        std::optional<PrefabNodeId> OccurrenceRoot(const PrefabRegistry &registry, const PrefabNode &node)
        {
            const PrefabDocument *inner = registry.Find(node.mPrefab);
            if (inner == nullptr)
                return std::nullopt;
            return inner->mRootNode;
        }

        // An occurrence's Transform is its inner root's, under the occurrence's own root patch.
        std::optional<Transform> NodeTransform(const PrefabRegistry &registry, const PrefabNode &node)
        {
            if (node.mKind == PrefabNodeKind::Entity)
            {
                const PrefabComponentInstance *component = FindComponent(node, "Transform");
                return component != nullptr ? std::optional<Transform>(ReadTransform(component->mFields)) : std::nullopt;
            }

            const PrefabDocument *inner = registry.Find(node.mPrefab);
            const PrefabNode *root = inner != nullptr ? FindNode(*inner, SceneNodeAddress{ inner->mRootNode }) : nullptr;
            const PrefabComponentInstance *component = root != nullptr ? FindComponent(*root, "Transform") : nullptr;
            if (component == nullptr)
                return std::nullopt;

            json fields = component->mFields;
            if (const PrefabPatch *patch = FindPatch(node, SceneNodeAddress{ root->mId }))
            {
                for (const PrefabFieldOverride &field : patch->mFields)
                {
                    if (field.mComponent == "Transform")
                        fields[field.mField] = field.mValue;
                }
            }
            return ReadTransform(fields);
        }

        void WriteNodeTransform(const PrefabRegistry &registry, PrefabNode &node, const Transform &local)
        {
            for (const FieldDesc &field : kTransformFields)
            {
                FieldBuffer buffer{};
                field.Read(&local, buffer.mBytes);
                json value = FieldToJson(field.mKind, buffer.mBytes);
                if (node.mKind == PrefabNodeKind::Entity)
                {
                    FindComponent(node, "Transform")->mFields[std::string(field.mName)] = std::move(value);
                    continue;
                }

                PrefabPatch &patch = GetOrAddPatch(node, SceneNodeAddress{ *OccurrenceRoot(registry, node) });
                if (PrefabFieldOverride *existing = FindPatchField(patch, "Transform", field.mName))
                    existing->mValue = std::move(value);
                else
                    patch.mFields.push_back(PrefabFieldOverride{ "Transform", std::string(field.mName), std::move(value) });
            }
        }

        // Nested occurrences hold no nodes, so every ancestor is an entity node.
        std::optional<WorldPose> DocumentPose(const PrefabDocument &document, PrefabNodeId id)
        {
            std::vector<const PrefabNode *> chain;
            for (std::optional<std::size_t> index = FindNodeIndex(document, id);
                index && chain.size() < document.mNodes.size();
                index = FindNodeIndex(document, chain.back()->mParent))
            {
                chain.push_back(&document.mNodes[*index]);
                if (chain.back()->mId == document.mRootNode)
                    break;
            }

            WorldPose pose;
            for (auto node = chain.rbegin(); node != chain.rend(); ++node)
            {
                const PrefabComponentInstance *component = FindComponent(**node, "Transform");
                if (component == nullptr)
                    continue;
                const Transform local = ReadTransform(component->mFields);
                const std::optional<WorldPose> composed = TryComposePose(pose, local.LinearMatrix(), local.Position());
                if (!composed)
                    return std::nullopt;
                pose = *composed;
            }
            return pose;
        }

        // Rewrites the node's Transform so its pose in the document survives the move under parent.
        bool KeepDocumentPose(const PrefabRegistry &registry, const PrefabDocument &document, PrefabNode &node,
            PrefabNodeId parent)
        {
            const std::optional<Transform> current = NodeTransform(registry, node);
            if (!current)
                return true;

            const std::optional<WorldPose> oldParent = DocumentPose(document, node.mParent);
            const std::optional<WorldPose> nodePose =
                oldParent ? TryComposePose(*oldParent, current->LinearMatrix(), current->Position()) : std::nullopt;
            const std::optional<WorldPose> newParent = DocumentPose(document, parent);
            const std::optional<Transform> local =
                nodePose && newParent ? TryLocalForWorldPose(*newParent, *nodePose) : std::nullopt;
            if (!local)
                return false;

            WriteNodeTransform(registry, node, *local);
            return true;
        }

        bool PatchHasComponent(const PrefabPatch &patch, std::string_view component)
        {
            return FindAddedComponent(patch, component) != nullptr ||
                std::find(patch.mRemovedComponents.begin(), patch.mRemovedComponents.end(), component) !=
                patch.mRemovedComponents.end();
        }

        // Written back to a removed component, these restore it with the overrides its removal erased.
        std::optional<std::string> InheritedOverrides(const PrefabPatch &patch, std::string_view component)
        {
            json fields = json::object();
            for (const PrefabFieldOverride &field : patch.mFields)
            {
                if (field.mComponent == component)
                    fields[field.mField] = field.mValue;
            }
            if (fields.empty())
                return std::nullopt;
            return fields.dump();
        }

        // The nested document a Prefab node expands, so an address can be resolved below this layer.
        const PrefabDocument *InnerDocument(const PrefabRegistry &registry, const PrefabNode &node)
        {
            return node.mKind == PrefabNodeKind::Prefab ? registry.Find(node.mPrefab) : nullptr;
        }

        bool DocumentReaches(const PrefabRegistry &registry, PrefabId from, PrefabId target,
            std::vector<PrefabId> &visited)
        {
            if (from == target)
                return true;
            if (std::find(visited.begin(), visited.end(), from) != visited.end())
                return false;
            visited.push_back(from);

            const PrefabDocument *document = registry.Find(from);
            if (document == nullptr)
                return false;
            for (const PrefabNode &node : document->mNodes)
            {
                if (node.mKind == PrefabNodeKind::Prefab && !node.mPrefab.IsNull() &&
                    DocumentReaches(registry, node.mPrefab, target, visited))
                {
                    return true;
                }
            }
            return false;
        }

        bool TemplateHasComponent(const PrefabRegistry &registry, const PrefabDocument &document,
            const SceneNodeAddress &address, std::string_view type);

        bool ApplyPatchEntry(const PrefabRegistry &registry, PrefabNode &occurrence, const SceneNodeAddress &target,
            const PrefabAuthoredEntry &entry)
        {
            if (target.empty())
                return false;

            const PrefabDocument *inner = InnerDocument(registry, occurrence);
            if (inner == nullptr || FindDocumentNode(registry, *inner, target) == nullptr)
                return false;

            json value;
            if (!DecodeValue(entry.mValue, value))
                return false;

            if (IsFieldKey(entry.mKey))
            {
                const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mKey.mComponent);
                const FieldDesc *field = ops != nullptr ? ops->FindField(entry.mKey.mField) : nullptr;
                if (field == nullptr || field->ReadOnly())
                    return false;
                if (entry.mValue && !ValueFitsField(*field, value))
                    return false;

                PrefabPatch *patch = FindPatch(occurrence, target);
                PrefabComponentInstance *added =
                    patch != nullptr ? FindAddedComponent(*patch, entry.mKey.mComponent) : nullptr;
                if (added != nullptr && added->mFields.contains(entry.mKey.mField))
                {
                    if (entry.mValue)
                        added->mFields[entry.mKey.mField] = std::move(value);
                    else
                        added->mFields.erase(entry.mKey.mField);
                    return true;
                }

                if (!entry.mValue)
                {
                    if (patch != nullptr)
                    {
                        std::erase_if(patch->mFields, [&entry](const PrefabFieldOverride &override)
                            {
                                return override.mComponent == entry.mKey.mComponent &&
                                    override.mField == entry.mKey.mField;
                            });
                        EraseEmptyPatch(occurrence, target);
                    }
                    return true;
                }

                if (patch == nullptr)
                    patch = &GetOrAddPatch(occurrence, target);
                if (PrefabFieldOverride *existing =
                        FindPatchField(*patch, entry.mKey.mComponent, entry.mKey.mField))
                {
                    existing->mValue = std::move(value);
                }
                else
                {
                    patch->mFields.push_back(
                        PrefabFieldOverride{ entry.mKey.mComponent, entry.mKey.mField, std::move(value) });
                }
                return true;
            }

            if (!IsComponentKey(entry.mKey))
                return false;

            const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mKey.mComponent);
            if (ops == nullptr)
                return false;

            if (!entry.mValue)
            {
                PrefabPatch *patch = FindPatch(occurrence, target);
                if (patch == nullptr)
                    return true;

                const std::size_t addedBefore = patch->mAddedComponents.size();
                std::erase_if(patch->mAddedComponents, [&entry](const PrefabComponentInstance &component)
                    {
                        return component.mType == entry.mKey.mComponent;
                    });
                const std::size_t removedBefore = patch->mRemovedComponents.size();
                std::erase(patch->mRemovedComponents, entry.mKey.mComponent);
                const bool changed = patch->mAddedComponents.size() != addedBefore ||
                    patch->mRemovedComponents.size() != removedBefore;
                if (changed)
                {
                    std::erase_if(patch->mFields, [&entry](const PrefabFieldOverride &override)
                        {
                            return override.mComponent == entry.mKey.mComponent;
                        });
                    EraseEmptyPatch(occurrence, target);
                }
                return true;
            }

            if (value.is_null())
            {
                PrefabPatch &patch = GetOrAddPatch(occurrence, target);
                std::erase_if(patch.mAddedComponents, [&entry](const PrefabComponentInstance &component)
                    {
                        return component.mType == entry.mKey.mComponent;
                    });
                if (std::find(patch.mRemovedComponents.begin(), patch.mRemovedComponents.end(),
                        entry.mKey.mComponent) == patch.mRemovedComponents.end())
                {
                    patch.mRemovedComponents.push_back(entry.mKey.mComponent);
                }
                std::erase_if(patch.mFields, [&entry](const PrefabFieldOverride &override)
                    {
                        return override.mComponent == entry.mKey.mComponent;
                    });
                return true;
            }

            if (!value.is_object())
                return false;

            for (auto it = value.begin(); it != value.end(); ++it)
            {
                const FieldDesc *field = ops->FindField(it.key());
                if (field == nullptr || field->ReadOnly())
                    return false;
                if (!ValueFitsField(*field, it.value()))
                    return false;
            }

            PrefabPatch &patch = GetOrAddPatch(occurrence, target);
            std::erase(patch.mRemovedComponents, entry.mKey.mComponent);
            // A component of the inner template takes the fields as overrides rather than a second copy.
            if (TemplateHasComponent(registry, *inner, target, entry.mKey.mComponent))
            {
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    if (PrefabFieldOverride *existing = FindPatchField(patch, entry.mKey.mComponent, it.key()))
                        existing->mValue = it.value();
                    else
                        patch.mFields.push_back(PrefabFieldOverride{ entry.mKey.mComponent, it.key(), it.value() });
                }
                EraseEmptyPatch(occurrence, target);
                return true;
            }
            if (PrefabComponentInstance *added = FindAddedComponent(patch, entry.mKey.mComponent))
            {
                added->mFields = std::move(value);
            }
            else
            {
                patch.mAddedComponents.push_back(
                    PrefabComponentInstance{ entry.mKey.mComponent, std::move(value) });
            }
            return true;
        }

        bool ApplyDocumentEntry(const PrefabRegistry &registry, PrefabDocument &document,
            const PrefabAuthoredEntry &entry, std::vector<SceneNodeAddress> &removedNodes)
        {
            if (entry.mKey.mAddress.empty())
                return false;

            if (entry.mKey.mAddress.size() > 1)
            {
                if (IsNodeKey(entry.mKey))
                    return false;

                const std::optional<std::size_t> index = FindNodeIndex(document, entry.mKey.mAddress.front());
                if (!index || document.mNodes[*index].mKind != PrefabNodeKind::Prefab)
                    return false;

                const SceneNodeAddress target(entry.mKey.mAddress.begin() + 1, entry.mKey.mAddress.end());
                return ApplyPatchEntry(registry, document.mNodes[*index], target, entry);
            }

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
                if (id >= document.mNextNodeId)
                    document.mNextNodeId = id + 1;
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

                if (!entry.mValue || fields.is_null())
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
            const PrefabDocumentLookup &documents, SceneEdit &out)
        {
            if (entry.mKey.mAddress.size() < 2)
                return false;

            out.mEntry = &entry;
            out.mRoot = entry.mKey.mAddress.front();
            out.mNode = SceneNodeAddress(entry.mKey.mAddress.begin() + 1, entry.mKey.mAddress.end());

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

                outValue = json::array();
                for (uint64_t segment : address)
                    outValue.push_back(std::to_string(segment));
                return true;
            }

            reason = scene.mEntities.contains(*target) ? "targets an entity outside this instance"
                                                       : "targets an entity the scene does not contain";
            return false;
        }

        std::optional<SceneNodeAddress> ReadAddressJson(const json &value)
        {
            if (value.is_null())
                return SceneNodeAddress{};
            if (!value.is_array())
                return std::nullopt;

            SceneNodeAddress address;
            for (const json &segment : value)
            {
                if (!segment.is_string())
                    return std::nullopt;
                const std::string &text = segment.get_ref<const std::string &>();
                if (text.empty())
                    return std::nullopt;

                uint64_t id = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
                if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
                    return std::nullopt;
                address.push_back(id);
            }
            return address;
        }

        json AddressJson(const SceneNodeAddress &address)
        {
            json out = json::array();
            for (uint64_t segment : address)
                out.push_back(std::to_string(segment));
            return out;
        }

        // The entity node's own components, then each enclosing occurrence's patches from the innermost out.
        bool TemplateHasComponent(const PrefabRegistry &registry, const PrefabDocument &document,
            const SceneNodeAddress &address, std::string_view type)
        {
            std::vector<const PrefabNode *> occurrences;
            const PrefabDocument *current = &document;
            const PrefabNode *entity = nullptr;
            for (std::size_t i = 0; i < address.size() && current != nullptr; ++i)
            {
                const std::optional<std::size_t> index = FindNodeIndex(*current, address[i]);
                if (!index)
                    return false;

                const PrefabNode &node = current->mNodes[*index];
                if (i + 1 == address.size())
                {
                    entity = node.mKind == PrefabNodeKind::Entity ? &node : nullptr;
                    break;
                }
                if (node.mKind != PrefabNodeKind::Prefab)
                    return false;

                occurrences.push_back(&node);
                current = registry.Find(node.mPrefab);
            }
            if (entity == nullptr)
                return false;

            bool present = FindComponent(*entity, type) != nullptr;
            for (std::size_t level = occurrences.size(); level-- > 0;)
            {
                const SceneNodeAddress suffix(address.begin() + static_cast<std::ptrdiff_t>(level) + 1, address.end());
                for (const PrefabPatch &patch : occurrences[level]->mPatches)
                {
                    if (patch.mTarget != suffix)
                        continue;
                    if (FindAddedComponent(patch, type) != nullptr)
                        present = true;
                    if (std::find(patch.mRemovedComponents.begin(), patch.mRemovedComponents.end(), type) !=
                        patch.mRemovedComponents.end())
                    {
                        present = false;
                    }
                }
            }
            return present;
        }

        struct ApplyOrigin
        {
            bool mScene = false;
            StableId mRoot = kNullStableId;                  // scene origin
            const ScenePrefabInstance *mInstance = nullptr;  // scene origin
            std::optional<PrefabId> mPrefab;                 // Prefab-tab origin
            SceneNodeAddress mSegments;                      // address in the origin's document space
            std::vector<PrefabId> mDocuments;                // documents owning each segment
        };

        struct ApplyLayer
        {
            PrefabId mPrefab{};
            std::size_t mLevel = 0;
            SceneNodeAddress mAddress; // relative to the layer's document
        };

        std::string LayerLabel(const PrefabRegistry &registry, PrefabId id)
        {
            if (const std::optional<std::filesystem::path> path = registry.PathOf(id))
            {
                const std::u8string stem = path->stem().u8string();
                return std::string(stem.begin(), stem.end());
            }

            constexpr std::size_t kShortIdLength = 8;
            const std::string text = ToString(id);
            return text.substr(0, kShortIdLength);
        }

        // Each segment names a node of the document owning the one before it; the last names an entity.
        bool ResolveDocumentChain(const PrefabRegistry &registry, PrefabDocument const &document,
            const SceneNodeAddress &segments, std::vector<PrefabId> &documents)
        {
            documents.clear();
            const PrefabDocument *current = &document;
            for (std::size_t i = 0; i < segments.size(); ++i)
            {
                const std::optional<std::size_t> index = FindNodeIndex(*current, segments[i]);
                if (!index)
                    return false;

                const PrefabNode &node = current->mNodes[*index];
                if (i + 1 == segments.size())
                    return node.mKind == PrefabNodeKind::Entity;

                if (node.mKind != PrefabNodeKind::Prefab)
                    return false;
                const PrefabDocument *next = registry.Find(node.mPrefab);
                if (next == nullptr)
                    return false;

                documents.push_back(node.mPrefab);
                current = next;
            }
            return false;
        }

        struct ApplyChain
        {
            ApplyOrigin mOrigin;
            std::vector<ApplyLayer> mLayers;
        };

        std::optional<ApplyChain> ResolveApplyChain(const PrefabRegistry &registry, const LoadedScene *scene,
            const PrefabAuthoredKey &key)
        {
            ApplyChain chain;

            if (key.mPrefab)
            {
                const PrefabDocument *document = registry.Find(*key.mPrefab);
                if (document == nullptr || key.mAddress.size() < 2)
                    return std::nullopt;

                std::vector<PrefabId> documents;
                if (!ResolveDocumentChain(registry, *document, key.mAddress, documents))
                    return std::nullopt;

                chain.mOrigin.mPrefab = *key.mPrefab;
                chain.mOrigin.mSegments = key.mAddress;
                chain.mOrigin.mDocuments.push_back(*key.mPrefab);
                chain.mOrigin.mDocuments.insert(chain.mOrigin.mDocuments.end(), documents.begin(), documents.end());
            }
            else
            {
                if (scene == nullptr || key.mAddress.size() < 2)
                    return std::nullopt;

                const StableId root = key.mAddress.front();
                const auto instance = scene->mInstances.find(root);
                if (instance == scene->mInstances.end())
                    return std::nullopt;

                const SceneNodeAddress node(key.mAddress.begin() + 1, key.mAddress.end());
                if (!instance->second.mBindings.contains(node))
                    return std::nullopt;

                const std::optional<PrefabId> prefab = ParsePrefabId(instance->second.mPrefab);
                const PrefabDocument *document = prefab ? registry.Find(*prefab) : nullptr;
                if (document == nullptr)
                    return std::nullopt;

                std::vector<PrefabId> documents;
                if (!ResolveDocumentChain(registry, *document, node, documents))
                    return std::nullopt;

                chain.mOrigin.mScene = true;
                chain.mOrigin.mRoot = root;
                chain.mOrigin.mInstance = &instance->second;
                chain.mOrigin.mSegments = node;
                chain.mOrigin.mDocuments.push_back(*prefab);
                chain.mOrigin.mDocuments.insert(chain.mOrigin.mDocuments.end(), documents.begin(), documents.end());
            }

            const std::size_t start = chain.mOrigin.mScene ? 0 : 1;
            for (std::size_t level = start; level < chain.mOrigin.mSegments.size(); ++level)
            {
                ApplyLayer layer;
                layer.mPrefab = chain.mOrigin.mDocuments[level];
                layer.mLevel = level;
                layer.mAddress.assign(chain.mOrigin.mSegments.begin() + static_cast<std::ptrdiff_t>(level),
                    chain.mOrigin.mSegments.end());
                chain.mLayers.push_back(std::move(layer));
            }
            return chain;
        }

        bool LayerPrefabNode(const PrefabRegistry &registry, const ApplyLayer &layer, const PrefabNode *&node)
        {
            const PrefabDocument *document = registry.Find(layer.mPrefab);
            if (document == nullptr || layer.mAddress.empty())
                return false;
            const std::optional<std::size_t> index = FindNodeIndex(*document, layer.mAddress.front());
            if (!index)
                return false;
            node = &document->mNodes[*index];
            return true;
        }

        bool LayerHasField(const PrefabRegistry &registry, const ApplyLayer &layer, std::string_view component,
            std::string_view field)
        {
            const PrefabNode *node = nullptr;
            if (!LayerPrefabNode(registry, layer, node))
                return false;

            if (layer.mAddress.size() == 1)
            {
                const PrefabComponentInstance *instance = FindComponent(*node, component);
                return instance != nullptr && instance->mFields.contains(std::string(field));
            }

            if (node->mKind != PrefabNodeKind::Prefab)
                return false;
            const SceneNodeAddress target(layer.mAddress.begin() + 1, layer.mAddress.end());
            const PrefabPatch *patch = FindPatch(*node, target);
            if (patch == nullptr)
                return false;

            if (const PrefabComponentInstance *added = FindAddedComponent(*patch, component))
            {
                if (added->mFields.contains(std::string(field)))
                    return true;
            }
            return FindPatchField(*patch, component, field) != nullptr;
        }

        bool LayerHasComponent(const PrefabRegistry &registry, const ApplyLayer &layer, std::string_view component)
        {
            const PrefabNode *node = nullptr;
            if (!LayerPrefabNode(registry, layer, node))
                return false;

            if (layer.mAddress.size() == 1)
                return FindComponent(*node, component) != nullptr;

            if (node->mKind != PrefabNodeKind::Prefab)
                return false;
            const SceneNodeAddress target(layer.mAddress.begin() + 1, layer.mAddress.end());
            const PrefabPatch *patch = FindPatch(*node, target);
            return patch != nullptr && PatchHasComponent(*patch, component);
        }

        bool TranslateLayerRef(const ApplyOrigin &origin, const ApplyLayer &layer, const LoadedScene *scene,
            const json &value, json &outValue, std::string &reason)
        {
            const std::size_t level = layer.mLevel;
            SceneNodeAddress address;
            if (origin.mScene)
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

                const ScenePrefabInstance &instance = *origin.mInstance;
                const auto binding = std::find_if(instance.mBindings.begin(), instance.mBindings.end(),
                    [target](const auto &entry) { return entry.second == *target; });
                if (binding == instance.mBindings.end())
                {
                    reason = scene != nullptr && scene->mEntities.contains(*target)
                        ? "targets an entity outside this instance"
                        : "targets an entity the scene does not contain";
                    return false;
                }
                address = binding->first;
            }
            else
            {
                if (value.is_null())
                {
                    outValue = nullptr;
                    return true;
                }
                const std::optional<SceneNodeAddress> parsed = ReadAddressJson(value);
                if (!parsed)
                {
                    reason = "is not a node address or null";
                    return false;
                }
                address = *parsed;
            }

            if (address.size() <= level ||
                !std::equal(origin.mSegments.begin(), origin.mSegments.begin() + static_cast<std::ptrdiff_t>(level),
                    address.begin()))
            {
                reason = "targets an entity outside this layer";
                return false;
            }

            outValue = AddressJson(SceneNodeAddress(address.begin() + static_cast<std::ptrdiff_t>(level),
                address.end()));
            return true;
        }

        const PrefabPatch *OriginPatch(const PrefabRegistry &registry, const PrefabAuthoredKey &key,
            const ApplyOrigin &origin)
        {
            if (origin.mScene || !key.mPrefab || origin.mSegments.size() < 2)
                return nullptr;

            const PrefabDocument *document = registry.Find(*key.mPrefab);
            if (document == nullptr)
                return nullptr;
            const std::optional<std::size_t> index = FindNodeIndex(*document, origin.mSegments.front());
            if (!index || document->mNodes[*index].mKind != PrefabNodeKind::Prefab)
                return nullptr;

            const SceneNodeAddress target(origin.mSegments.begin() + 1, origin.mSegments.end());
            return FindPatch(document->mNodes[*index], target);
        }

        std::optional<json> OriginFieldValue(const PrefabRegistry &registry, const PrefabAuthoredKey &key,
            const ApplyOrigin &origin)
        {
            if (!origin.mScene)
            {
                const PrefabPatch *patch = OriginPatch(registry, key, origin);
                if (patch == nullptr)
                    return std::nullopt;
                if (const PrefabComponentInstance *added = FindAddedComponent(*patch, key.mComponent))
                {
                    const auto it = added->mFields.find(key.mField);
                    if (it != added->mFields.end())
                        return std::optional<json>(it.value());
                }
                if (const PrefabFieldOverride *entry = FindPatchField(*patch, key.mComponent, key.mField))
                    return std::optional<json>(entry->mValue);
                return std::nullopt;
            }

            const ScenePrefabInstance &instance = *origin.mInstance;
            if (const SceneComponentRecord *added = FindAddedComponent(instance, origin.mSegments, key.mComponent))
            {
                const auto it = added->mFields.find(key.mField);
                if (it != added->mFields.end())
                    return std::optional<json>(it.value());
            }
            if (const SceneFieldPatch *patch =
                    FindInstanceFieldPatch(instance, origin.mSegments, key.mComponent, key.mField))
            {
                return std::optional<json>(patch->mValue);
            }
            return std::nullopt;
        }

        std::optional<json> OriginAddedFields(const PrefabRegistry &registry, const PrefabAuthoredKey &key,
            const ApplyOrigin &origin)
        {
            if (!origin.mScene)
            {
                const PrefabPatch *patch = OriginPatch(registry, key, origin);
                const PrefabComponentInstance *added =
                    patch != nullptr ? FindAddedComponent(*patch, key.mComponent) : nullptr;
                return added != nullptr ? std::optional<json>(added->mFields) : std::nullopt;
            }

            const SceneComponentRecord *added =
                FindAddedComponent(*origin.mInstance, origin.mSegments, key.mComponent);
            return added != nullptr ? std::optional<json>(added->mFields) : std::nullopt;
        }

        PrefabComponentState OriginComponentState(const PrefabRegistry &registry, const LoadedScene *scene,
            const PrefabAuthoredKey &key, const ApplyOrigin &origin)
        {
            if (!origin.mScene)
            {
                const PrefabPatch *patch = OriginPatch(registry, key, origin);
                if (patch == nullptr)
                    return PrefabComponentState::Inherited;
                if (FindAddedComponent(*patch, key.mComponent) != nullptr)
                    return PrefabComponentState::Added;
                if (std::find(patch->mRemovedComponents.begin(), patch->mRemovedComponents.end(),
                        key.mComponent) != patch->mRemovedComponents.end())
                {
                    return PrefabComponentState::Removed;
                }
                return PrefabComponentState::Inherited;
            }

            if (scene == nullptr)
                return PrefabComponentState::Absent;
            return InstanceComponentState(registry, *scene, origin.mRoot, origin.mSegments, key.mComponent);
        }
    }

    PrefabAuthoring::PrefabAuthoring(World &world, PrefabCoordinator &coordinator)
        : mWorld(world), mCoordinator(coordinator)
    {
    }

    std::optional<PrefabAuthoring::Context> PrefabAuthoring::CurrentContext() const
    {
        if (mActiveDocument)
        {
            World *world = mCoordinator.PreviewWorld(*mActiveDocument);
            LoadedScene *scene = mCoordinator.PreviewScene(*mActiveDocument);
            if (world == nullptr || scene == nullptr)
                return std::nullopt;
            return Context{ world, scene, mActiveDocument };
        }

        LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr)
            return std::nullopt;
        return Context{ &mWorld, scene, std::nullopt };
    }

    std::optional<PrefabAuthoring::Member> PrefabAuthoring::FindMember(Entity entity) const
    {
        const std::optional<Context> context = CurrentContext();
        if (!context || !context->mWorld->IsAlive(entity))
            return std::nullopt;

        for (const auto &[root, instance] : context->mScene->mInstances)
        {
            for (const auto &[address, id] : instance.mBindings)
            {
                const auto it = context->mScene->mEntities.find(id);
                if (it == context->mScene->mEntities.end() || it->second != entity)
                    continue;

                Member member;
                member.mRoot = root;
                member.mNode = address;
                member.mInstance = &instance;
                member.mEntity = entity;
                member.mPrefab = context->mPrefab;
                return member;
            }
        }
        return std::nullopt;
    }

    const PrefabDocument *PrefabAuthoring::ActivePrefabDocument() const
    {
        return mActiveDocument ? mCoordinator.Registry().Find(*mActiveDocument) : nullptr;
    }

    std::optional<PrefabNodeId> PrefabAuthoring::NodeIdOf(Entity entity) const
    {
        const std::optional<Member> member = FindMember(entity);
        if (!member || !member->mPrefab)
            return std::nullopt;

        if (member->mNode.size() == 1)
            return member->mNode.front();

        const PrefabDocument *document = ActivePrefabDocument();
        if (document == nullptr || member->mNode.size() != 2)
            return std::nullopt;

        const std::optional<std::size_t> index = FindNodeIndex(*document, member->mNode.front());
        if (!index || document->mNodes[*index].mKind != PrefabNodeKind::Prefab)
            return std::nullopt;

        const PrefabDocument *inner = mCoordinator.Registry().Find(document->mNodes[*index].mPrefab);
        if (inner == nullptr || inner->mRootNode != member->mNode.back())
            return std::nullopt;
        return member->mNode.front();
    }

    bool PrefabAuthoring::WritesComponentDirectly(Entity entity, std::string_view component) const
    {
        const std::optional<Member> member = FindMember(entity);
        if (!member || !member->mPrefab || !mActiveDocument || *member->mPrefab != *mActiveDocument)
            return false;
        const PrefabDocument *document = ActivePrefabDocument();
        const PrefabNode *node = document != nullptr ? FindNode(*document, member->mNode) : nullptr;
        return node != nullptr && FindComponent(*node, component) != nullptr;
    }

    std::optional<PrefabAuthoredKey> PrefabAuthoring::FieldKey(Entity entity, std::string_view component,
        std::string_view field) const
    {
        if (mCoordinator.IsFrozen())
            return std::nullopt;

        std::optional<Member> member = FindMember(entity);
        if (!member)
            return std::nullopt;

        PrefabAuthoredKey key;
        if (member->mPrefab)
        {
            key.mPrefab = member->mPrefab;
            key.mAddress = member->mNode;
            key.mComponent = component;
            key.mField = field;
            return key;
        }

        const LoadedScene &scene = *mCoordinator.Scene();
        const auto root = scene.mEntities.find(member->mInstance->mRoot);
        if (component == "Transform" && root != scene.mEntities.end() && root->second == entity)
            return std::nullopt;

        key.mAddress.reserve(member->mNode.size() + 1);
        key.mAddress.push_back(member->mRoot);
        key.mAddress.insert(key.mAddress.end(), member->mNode.begin(), member->mNode.end());
        key.mComponent = component;
        key.mField = field;
        return key;
    }

    std::optional<PrefabAuthoredKey> PrefabAuthoring::ComponentKey(Entity entity, std::string_view component) const
    {
        if (mCoordinator.IsFrozen())
            return std::nullopt;

        std::optional<Member> member = FindMember(entity);
        if (!member)
            return std::nullopt;

        PrefabAuthoredKey key;
        if (member->mPrefab)
        {
            key.mPrefab = member->mPrefab;
            key.mAddress = member->mNode;
            key.mComponent = component;
            return key;
        }

        const LoadedScene &scene = *mCoordinator.Scene();
        const auto root = scene.mEntities.find(member->mInstance->mRoot);
        if (component == "Transform" && root != scene.mEntities.end() && root->second == entity)
            return std::nullopt;

        key.mAddress.reserve(member->mNode.size() + 1);
        key.mAddress.push_back(member->mRoot);
        key.mAddress.insert(key.mAddress.end(), member->mNode.begin(), member->mNode.end());
        key.mComponent = component;
        return key;
    }

    PrefabFieldState PrefabAuthoring::FieldState(const PrefabAuthoredKey &key) const
    {
        if (mCoordinator.IsFrozen())
            return PrefabFieldState::Inherited;

        if (key.mPrefab)
        {
            const PrefabDocument *document = mCoordinator.Registry().Find(*key.mPrefab);
            if (document == nullptr)
                return PrefabFieldState::Inherited;

            if (key.mAddress.size() > 1)
            {
                const std::optional<std::size_t> index = FindNodeIndex(*document, key.mAddress.front());
                if (index && document->mNodes[*index].mKind == PrefabNodeKind::Prefab)
                {
                    const SceneNodeAddress target(key.mAddress.begin() + 1, key.mAddress.end());
                    const PrefabPatch *patch = FindPatch(document->mNodes[*index], target);
                    if (patch != nullptr)
                    {
                        if (const PrefabComponentInstance *added = FindAddedComponent(*patch, key.mComponent))
                        {
                            if (added->mFields.contains(key.mField))
                                return PrefabFieldState::Overridden;
                        }
                        if (FindPatchField(*patch, key.mComponent, key.mField) != nullptr)
                            return PrefabFieldState::Overridden;
                    }
                }
                return PrefabFieldState::Inherited;
            }

            if (mActiveDocument && *mActiveDocument == *key.mPrefab)
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
        if (mCoordinator.IsFrozen())
            return PrefabComponentState::Absent;

        std::optional<Member> member = FindMember(entity);
        if (!member)
            return PrefabComponentState::Absent;

        if (member->mPrefab)
        {
            const PrefabDocument *document = ActivePrefabDocument();
            if (document == nullptr)
                return PrefabComponentState::Absent;

            if (member->mNode.size() > 1)
            {
                const std::optional<std::size_t> index = FindNodeIndex(*document, member->mNode.front());
                if (index && document->mNodes[*index].mKind == PrefabNodeKind::Prefab)
                {
                    const SceneNodeAddress target(member->mNode.begin() + 1, member->mNode.end());
                    const PrefabPatch *patch = FindPatch(document->mNodes[*index], target);
                    if (patch != nullptr)
                    {
                        if (FindAddedComponent(*patch, component) != nullptr)
                            return PrefabComponentState::Added;
                        if (std::find(patch->mRemovedComponents.begin(), patch->mRemovedComponents.end(),
                                component) != patch->mRemovedComponents.end())
                        {
                            return PrefabComponentState::Removed;
                        }
                    }
                }
                return TemplateHasComponent(mCoordinator.Registry(), *document, member->mNode, component)
                    ? PrefabComponentState::Inherited
                    : PrefabComponentState::Absent;
            }

            const PrefabNode *node = FindNode(*document, member->mNode);
            return node != nullptr && FindComponent(*node, component) != nullptr ? PrefabComponentState::Inherited
                                                                                 : PrefabComponentState::Absent;
        }

        return InstanceComponentState(mCoordinator.Registry(), *mCoordinator.Scene(), member->mRoot, member->mNode,
            component);
    }

    PrefabInstanceRole PrefabAuthoring::InstanceRole(Entity entity) const
    {
        const std::optional<Context> context = CurrentContext();
        if (!context || !context->mWorld->IsAlive(entity))
            return PrefabInstanceRole::Ordinary;

        const std::optional<Member> member = FindMember(entity);
        if (!member)
            return PrefabInstanceRole::Ordinary;

        const auto root = context->mScene->mEntities.find(member->mRoot);
        if (root == context->mScene->mEntities.end())
            return PrefabInstanceRole::Ordinary;

        if (!member->mPrefab)
            return root->second == entity ? PrefabInstanceRole::Root : PrefabInstanceRole::Member;

        return AddressRole(mCoordinator.Registry(), ActivePrefabDocument(), member->mNode);
    }

    std::vector<Entity> PrefabAuthoring::PrefabLinkedEntities() const
    {
        std::vector<Entity> entities;
        const std::optional<Context> context = CurrentContext();
        if (!context)
            return entities;

        const PrefabDocument *document = context->mPrefab ? ActivePrefabDocument() : nullptr;
        if (context->mPrefab && document == nullptr)
            return entities;

        for (const auto &[root, instance] : context->mScene->mInstances)
        {
            (void)root;
            for (const auto &[address, id] : instance.mBindings)
            {
                if (document != nullptr &&
                    AddressRole(mCoordinator.Registry(), document, address) == PrefabInstanceRole::Ordinary)
                {
                    continue;
                }

                const auto entity = context->mScene->mEntities.find(id);
                if (entity != context->mScene->mEntities.end() && context->mWorld->IsAlive(entity->second))
                    entities.push_back(entity->second);
            }
        }
        return entities;
    }

    std::vector<std::string> PrefabAuthoring::RemovedComponents(Entity entity) const
    {
        std::vector<std::string> removed;
        if (mCoordinator.IsFrozen())
            return removed;

        std::optional<Member> member = FindMember(entity);
        if (!member)
            return removed;

        if (member->mPrefab)
        {
            const PrefabDocument *document = ActivePrefabDocument();
            if (document == nullptr || member->mNode.size() < 2)
                return removed;

            const std::optional<std::size_t> index = FindNodeIndex(*document, member->mNode.front());
            if (!index || document->mNodes[*index].mKind != PrefabNodeKind::Prefab)
                return removed;

            const SceneNodeAddress target(member->mNode.begin() + 1, member->mNode.end());
            if (const PrefabPatch *patch = FindPatch(document->mNodes[*index], target))
                removed = patch->mRemovedComponents;
            return removed;
        }

        const std::optional<PrefabId> id = ParsePrefabId(member->mInstance->mPrefab);
        if (!id)
            return removed;

        const PrefabRegistry &registry = mCoordinator.Registry();
        const PrefabDocument *document = registry.Find(*id);
        const PrefabNode *node =
            document != nullptr ? FindDocumentNode(registry, *document, member->mNode) : nullptr;
        if (node == nullptr)
            return removed;

        for (const PrefabComponentInstance &component : node->mComponents)
        {
            if (HasRemovedComponent(*member->mInstance, member->mNode, component.mType))
                removed.push_back(component.mType);
        }

        // A component an outer occurrence added is not in the inner node.
        for (const SceneInstancePatch &patch : member->mInstance->mPatches)
        {
            if (patch.mTarget != member->mNode)
                continue;
            for (const std::string &type : patch.mRemovedComponents)
            {
                if (std::find(removed.begin(), removed.end(), type) == removed.end())
                    removed.push_back(type);
            }
        }
        return removed;
    }

    std::string PrefabAuthoring::EncodeField(FieldKind kind, const void *bytes) const
    {
        if (kind != FieldKind::EntityRef)
            return FieldToJson(kind, bytes).dump();

        const Entity entity = *static_cast<const Entity *>(bytes);
        if (const std::optional<Member> member = FindMember(entity); member && member->mPrefab)
        {
            json address = json::array();
            for (PrefabNodeId segment : member->mNode)
                address.push_back(std::to_string(segment));
            return address.dump();
        }

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
                const PrefabRegistry &registry = mCoordinator.Registry();
                const PrefabDocument *document = registry.Find(*key.mPrefab);
                if (document == nullptr)
                    return std::nullopt;

                if (key.mAddress.size() > 1)
                {
                    const PrefabNode *member = FindDocumentNode(registry, *document, key.mAddress);
                    if (member == nullptr)
                        return std::nullopt;

                    const std::optional<std::size_t> index = FindNodeIndex(*document, key.mAddress.front());
                    const PrefabNode *occurrence =
                        index ? &document->mNodes[*index] : nullptr;
                    const SceneNodeAddress target(key.mAddress.begin() + 1, key.mAddress.end());
                    const PrefabPatch *patch =
                        occurrence != nullptr && occurrence->mKind == PrefabNodeKind::Prefab
                        ? FindPatch(*occurrence, target)
                        : nullptr;

                    if (IsNodeKey(key))
                        return std::nullopt;

                    if (IsFieldKey(key))
                    {
                        if (const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
                            ops == nullptr || ops->FindField(key.mField) == nullptr)
                        {
                            return std::nullopt;
                        }
                        if (patch == nullptr)
                        {
                            entries.push_back(std::move(entry));
                            continue;
                        }
                        if (const PrefabComponentInstance *added = FindAddedComponent(*patch, key.mComponent))
                        {
                            const auto it = added->mFields.find(key.mField);
                            if (it != added->mFields.end())
                                entry.mValue = it->dump();
                        }
                        else if (const PrefabFieldOverride *override =
                                     FindPatchField(*patch, key.mComponent, key.mField))
                        {
                            entry.mValue = override->mValue.dump();
                        }
                    }
                    else if (IsComponentKey(key))
                    {
                        if (patch == nullptr)
                        {
                            entries.push_back(std::move(entry));
                            continue;
                        }
                        if (const PrefabComponentInstance *added = FindAddedComponent(*patch, key.mComponent))
                            entry.mValue = added->mFields.dump();
                        else if (std::find(patch->mRemovedComponents.begin(), patch->mRemovedComponents.end(),
                                     key.mComponent) != patch->mRemovedComponents.end())
                        {
                            entry.mValue = "null";
                        }
                        else
                        {
                            entry.mValue = InheritedOverrides(*patch, key.mComponent);
                        }
                    }
                    else
                    {
                        return std::nullopt;
                    }

                    entries.push_back(std::move(entry));
                    continue;
                }

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
                if (scene == nullptr || key.mAddress.size() < 2)
                    return std::nullopt;

                const StableId root = key.mAddress.front();
                const SceneNodeAddress node(key.mAddress.begin() + 1, key.mAddress.end());
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
        if (mCoordinator.IsFrozen())
        {
            MIR_LOG_WARN("Prefab authoring: the documents are frozen during Play");
            return result;
        }

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
        const PrefabRegistry &registry = mCoordinator.Registry();
        for (auto &[id, document] : documents)
        {
            std::vector<const PrefabAuthoredEntry *> nodeInsertions;
            for (const PrefabAuthoredEntry &entry : entries)
            {
                if (!entry.mKey.mPrefab || *entry.mKey.mPrefab != id)
                    continue;
                if (IsNodeKey(entry.mKey) && entry.mKey.mAddress.size() != 1)
                {
                    MIR_LOG_WARN("Prefab authoring: a nested occurrence member has no node of its own");
                    return result;
                }
                if (IsNodeKey(entry.mKey) && entry.mValue)
                {
                    nodeInsertions.push_back(&entry);
                    continue;
                }
                if (!ApplyDocumentEntry(registry, document, entry, removedNodes[id]))
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
                if (!ApplyDocumentEntry(registry, document, *entry, removedNodes[id]))
                {
                    MIR_LOG_WARN("Prefab authoring: refused node entry in \"{}\"", ToString(id));
                    return result;
                }
            }
        }

        // Edits can span a layer and the Prefab that nests it, so compile against the batch's own copies.
        const PrefabDocumentLookup lookup = [&documents, &registry](PrefabId id) -> const PrefabDocument *
        {
            const auto edited = documents.find(id);
            return edited != documents.end() ? &edited->second : registry.Find(id);
        };
        std::vector<PrefabId> compileOrder;
        compileOrder.reserve(documents.size());
        for (const auto &[id, document] : documents)
        {
            (void)document;
            compileOrder.push_back(id);
        }
        const auto nestingCount = [&documents](PrefabId id)
            {
                const PrefabDocument &document = documents.at(id);
                std::size_t count = 0;
                for (const PrefabNode &node : document.mNodes)
                {
                    if (node.mKind == PrefabNodeKind::Prefab && documents.contains(node.mPrefab))
                        ++count;
                }
                return count;
            };
        std::stable_sort(compileOrder.begin(), compileOrder.end(), [&nestingCount](PrefabId a, PrefabId b)
            {
                return nestingCount(a) < nestingCount(b);
            });
        for (PrefabId id : compileOrder)
        {
            const PrefabCompileResult compiled = CompilePrefab(documents.at(id), lookup);
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
                if (entry.mKey.mAddress.size() < 2)
                    continue;

                const StableId root = entry.mKey.mAddress.front();
                const SceneNodeAddress node(entry.mKey.mAddress.begin() + 1, entry.mKey.mAddress.end());
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

        const bool sceneDirtyBefore = scene != nullptr && scene->mDirty;
        const auto unresolvedBefore = scene != nullptr ? scene->mUnresolvedEntityRefs
                                                      : decltype(LoadedScene::mUnresolvedEntityRefs){};
        const auto writeEdit = [&](const SceneEdit &edit)
            {
                return IsFieldKey(edit.mEntry->mKey)
                    ? WriteSceneField(mWorld, *scene, mCoordinator.Registry(), mCoordinator.Cache(), edit)
                    : WriteSceneComponent(mWorld, *scene, mCoordinator.Registry(), mCoordinator.Cache(), edit);
            };
        const auto rollbackScene = [&]()
            {
                if (scene == nullptr || !sceneBefore)
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

        std::vector<SceneEdit> sceneEdits;
        sceneEdits.reserve(sceneBatch.size());
        for (const PrefabAuthoredEntry &entry : sceneBatch)
        {
            SceneEdit edit;
            if (!PrepareSceneEdit(entry, *scene, mWorld, lookup, edit))
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
            if (scene == nullptr)
                break;
            for (const SceneNodeAddress &address : addresses)
            {
                if (scene != nullptr && SceneBindsAddress(*scene, id, address))
                    result.mSceneMaintained = true;
            }
        }

        result.mApplied = true;
        return result;
    }

    PrefabAuthoredAction PrefabAuthoring::Revert(const PrefabAuthoredKey &key) const
    {
        PrefabAuthoredAction action;
        if (mCoordinator.IsFrozen())
        {
            action.mRefusal = "the prefab documents are frozen during Play";
            return action;
        }

        const LoadedScene *scene = mCoordinator.Scene();
        if (key.mPrefab)
        {
            const std::optional<ApplyChain> chain =
                ResolveApplyChain(mCoordinator.Registry(), nullptr, key);
            if (!chain)
            {
                action.mRefusal = "the nested occurrence member is missing";
                return action;
            }

            const PrefabPatch *patch = OriginPatch(mCoordinator.Registry(), key, chain->mOrigin);
            if (patch == nullptr)
            {
                action.mRefusal = "the member has no local override to revert";
                return action;
            }

            if (IsFieldKey(key))
            {
                bool present = false;
                if (const PrefabComponentInstance *added = FindAddedComponent(*patch, key.mComponent))
                    present = added->mFields.contains(key.mField);
                present = present || FindPatchField(*patch, key.mComponent, key.mField) != nullptr;
                if (!present)
                {
                    action.mRefusal = "the field is not overridden";
                    return action;
                }
            }
            else if (IsComponentKey(key))
            {
                if (!PatchHasComponent(*patch, key.mComponent))
                {
                    action.mRefusal = "the component has no local change to revert";
                    return action;
                }
            }
            else
            {
                action.mRefusal = "select a field or a component";
                return action;
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (scene == nullptr || key.mAddress.size() < 2)
        {
            action.mRefusal = "only an instance member has a local override to revert";
            return action;
        }

        const StableId root = key.mAddress.front();
        const SceneNodeAddress node(key.mAddress.begin() + 1, key.mAddress.end());
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

    std::vector<PrefabApplyTarget> PrefabAuthoring::ApplyTargets(const PrefabAuthoredKey &key) const
    {
        std::vector<PrefabApplyTarget> targets;
        if (mCoordinator.IsFrozen() || key.mAddress.size() < 2)
            return targets;

        const PrefabRegistry &registry = mCoordinator.Registry();
        const std::optional<ApplyChain> chain = ResolveApplyChain(registry, mCoordinator.Scene(), key);
        if (!chain)
            return targets;

        for (const ApplyLayer &layer : chain->mLayers)
        {
            PrefabApplyTarget target;
            target.mPrefab = layer.mPrefab;
            target.mLabel = LayerLabel(registry, layer.mPrefab);
            target.mOverrides = IsFieldKey(key)
                ? LayerHasField(registry, layer, key.mComponent, key.mField)
                : IsComponentKey(key) && LayerHasComponent(registry, layer, key.mComponent);
            targets.push_back(std::move(target));
        }
        return targets;
    }

    PrefabAuthoredAction PrefabAuthoring::Apply(const PrefabAuthoredKey &key, std::size_t target) const
    {
        PrefabAuthoredAction action;
        if (mCoordinator.IsFrozen())
        {
            action.mRefusal = "the prefab documents are frozen during Play";
            return action;
        }

        const LoadedScene *scene = mCoordinator.Scene();
        const PrefabRegistry &registry = mCoordinator.Registry();
        const std::optional<ApplyChain> chain = ResolveApplyChain(registry, scene, key);
        if (!chain)
        {
            action.mRefusal = key.mPrefab ? "the nested occurrence member is missing"
                                          : "the instance member is missing";
            return action;
        }

        const ApplyOrigin &origin = chain->mOrigin;
        if (origin.mScene)
        {
            const auto binding = origin.mInstance->mBindings.find(origin.mSegments);
            if (binding == origin.mInstance->mBindings.end() ||
                !SceneMemberAlive(*scene, mWorld, binding->second))
            {
                action.mRefusal = "the instance member is missing";
                return action;
            }
        }

        if (chain->mLayers.empty())
        {
            action.mRefusal = "the member has no lower layer to apply to";
            return action;
        }
        if (target >= chain->mLayers.size())
        {
            action.mRefusal = "the apply target is not one of this member's layers";
            return action;
        }

        const ApplyLayer &layer = chain->mLayers[target];
        const PrefabAuthoredKey targetKey{ layer.mPrefab, layer.mAddress, key.mComponent, key.mField };
        const PrefabDocument *layerDocument = registry.Find(layer.mPrefab);
        if (layerDocument == nullptr)
        {
            action.mRefusal = "the target layer document is missing";
            return action;
        }

        if (IsFieldKey(key))
        {
            const ComponentOps *ops = ComponentRegistry::Instance().Find(key.mComponent);
            const FieldDesc *field = ops != nullptr ? ops->FindField(key.mField) : nullptr;
            if (field == nullptr || field->ReadOnly())
            {
                action.mRefusal = "the field cannot be authored on a prefab";
                return action;
            }

            const std::optional<json> value = OriginFieldValue(registry, key, origin);
            if (!value)
            {
                action.mRefusal = "the field is not overridden";
                return action;
            }

            if (!TemplateHasComponent(registry, *layerDocument, layer.mAddress, key.mComponent))
            {
                action.mRefusal = "apply the added component before its fields";
                return action;
            }

            json layerValue;
            std::string reason;
            if (field->mKind == FieldKind::EntityRef)
            {
                if (!TranslateLayerRef(origin, layer, scene, *value, layerValue, reason))
                {
                    action.mRefusal = "the field " + reason;
                    return action;
                }
            }
            else
            {
                if (!ValueFitsField(*field, *value))
                {
                    action.mRefusal = "the field value does not fit its type";
                    return action;
                }
                layerValue = *value;
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ targetKey, layerValue.dump() });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (!IsComponentKey(key))
        {
            action.mRefusal = "select a field or a component";
            return action;
        }

        const PrefabComponentState state = OriginComponentState(registry, scene, key, origin);
        if (state == PrefabComponentState::Added)
        {
            const std::optional<json> record = OriginAddedFields(registry, key, origin);
            if (!record)
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

            if (TemplateHasComponent(registry, *layerDocument, layer.mAddress, key.mComponent))
            {
                action.mRefusal = "the target layer already has this component";
                return action;
            }

            json fields = json::object();
            for (auto it = record->begin(); it != record->end(); ++it)
            {
                const FieldDesc *field = ops->FindField(it.key());
                if (field == nullptr)
                {
                    action.mRefusal = "the component has an unknown field \"" + it.key() + "\"";
                    return action;
                }

                if (field->mKind == FieldKind::EntityRef)
                {
                    json layerValue;
                    std::string reason;
                    if (!TranslateLayerRef(origin, layer, scene, it.value(), layerValue, reason))
                    {
                        action.mRefusal = "field \"" + it.key() + "\" " + reason;
                        return action;
                    }
                    fields[it.key()] = std::move(layerValue);
                    continue;
                }

                if (!ValueFitsField(*field, it.value()))
                {
                    action.mRefusal = "field \"" + it.key() + "\" value does not fit its type";
                    return action;
                }
                fields[it.key()] = it.value();
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ targetKey, fields.dump() });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        if (state == PrefabComponentState::Removed)
        {
            if (!TemplateHasComponent(registry, *layerDocument, layer.mAddress, key.mComponent))
            {
                action.mRefusal = "the target layer does not have this component";
                return action;
            }

            action.mEntries.push_back(PrefabAuthoredEntry{ targetKey, std::string("null") });
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
            return action;
        }

        action.mRefusal = "the component has no local change to apply";
        return action;
    }

    PrefabAuthoredAction PrefabAuthoring::CreateNode(PrefabNodeId parent, std::size_t siblingIndex,
        const EntityPresetDesc *preset)
    {
        PrefabAuthoredAction action;
        if (!mActiveDocument)
        {
            action.mRefusal = "no prefab document is active";
            return action;
        }

        const PrefabDocument *document = ActivePrefabDocument();
        const PrefabNode *parentNode = document != nullptr ? FindNode(*document, SceneNodeAddress{ parent }) : nullptr;
        if (parentNode == nullptr)
        {
            action.mRefusal = "the parent node is missing";
            return action;
        }
        if (parentNode->mKind != PrefabNodeKind::Entity)
        {
            action.mRefusal = "a nested Prefab occurrence cannot hold nodes";
            return action;
        }
        if (preset != nullptr && preset->mCanCreate != nullptr)
        {
            const std::optional<Context> context = CurrentContext();
            if (context && !preset->mCanCreate(*context->mWorld))
            {
                action.mRefusal = "the preset cannot be created in this document";
                return action;
            }
        }

        PrefabDocument copy = *document;
        const std::optional<PrefabNodeId> id = AllocateNodeId(copy);
        if (!id)
        {
            action.mRefusal = "the document has no free node id";
            return action;
        }

        PrefabAuthoredKey key;
        key.mPrefab = *mActiveDocument;
        key.mAddress = { *id };
        action.mEntries.push_back(PrefabAuthoredEntry{ key,
            NewNodeRecord(*document, *id, parent, siblingIndex, preset).dump() });
        return action;
    }

    PrefabAuthoredAction PrefabAuthoring::PlacePrefabNode(PrefabNodeId parent, PrefabId prefab,
        std::size_t siblingIndex, std::optional<WorldVec3> position)
    {
        PrefabAuthoredAction action;
        if (!mActiveDocument)
        {
            action.mRefusal = "no prefab document is active";
            return action;
        }

        if (prefab == *mActiveDocument)
        {
            action.mRefusal = "a prefab cannot nest itself";
            return action;
        }

        const PrefabRegistry &registry = mCoordinator.Registry();
        const PrefabDocument *inner = registry.Find(prefab);
        if (inner == nullptr)
        {
            action.mRefusal = "the prefab is not registered";
            return action;
        }

        std::vector<PrefabId> visited;
        if (DocumentReaches(registry, prefab, *mActiveDocument, visited))
        {
            action.mRefusal = "the prefab nests this document";
            return action;
        }

        const PrefabDocument *document = ActivePrefabDocument();
        const PrefabNode *parentNode = document != nullptr ? FindNode(*document, SceneNodeAddress{ parent }) : nullptr;
        if (parentNode == nullptr)
        {
            action.mRefusal = "the parent node is missing";
            return action;
        }
        if (parentNode->mKind != PrefabNodeKind::Entity)
        {
            action.mRefusal = "a nested Prefab occurrence cannot hold nodes";
            return action;
        }

        PrefabDocument copy = *document;
        const std::optional<PrefabNodeId> id = AllocateNodeId(copy);
        if (!id)
        {
            action.mRefusal = "the document has no free node id";
            return action;
        }

        PrefabAuthoredKey key;
        key.mPrefab = *mActiveDocument;
        key.mAddress = { *id };
        action.mEntries.push_back(PrefabAuthoredEntry{ key,
            NewOccurrenceRecord(*document, *id, parent, prefab, siblingIndex, position, inner->mRootNode).dump() });
        return action;
    }

    PrefabAuthoredAction PrefabAuthoring::DeleteNodes(std::span<const PrefabNodeId> nodes)
    {
        PrefabAuthoredAction action;
        const PrefabDocument *document = ActivePrefabDocument();
        if (!mActiveDocument || document == nullptr || nodes.empty())
        {
            action.mRefusal = "no prefab document is active";
            return action;
        }

        std::vector<PrefabNodeId> doomed;
        for (PrefabNodeId id : nodes)
        {
            if (id == document->mRootNode)
            {
                action.mRefusal = "the prefab root cannot be deleted";
                return action;
            }
            if (FindNodeIndex(*document, id) == std::nullopt)
            {
                action.mRefusal = "the node is missing";
                return action;
            }
            doomed.push_back(id);
            const std::vector<PrefabNodeId> descendants = DescendantsOf(*document, id);
            doomed.insert(doomed.end(), descendants.begin(), descendants.end());
        }

        std::set<PrefabNodeId> seen;
        for (PrefabNodeId id : doomed)
        {
            if (!seen.insert(id).second)
                continue;
            PrefabAuthoredKey key;
            key.mPrefab = *mActiveDocument;
            key.mAddress = { id };
            action.mEntries.push_back(PrefabAuthoredEntry{ key, std::nullopt });
        }
        // References in surviving source nodes are part of this undoable deletion.
        for (const PrefabNode &node : document->mNodes)
        {
            if (seen.contains(node.mId))
                continue;
            for (const PrefabComponentInstance &component : node.mComponents)
            {
                const ComponentOps *ops = ComponentRegistry::Instance().Find(component.mType);
                if (ops == nullptr)
                    continue;
                for (const FieldDesc &field : ops->mFields)
                {
                    if (field.mKind != FieldKind::EntityRef)
                        continue;
                    const auto value = component.mFields.find(std::string(field.mName));
                    if (value == component.mFields.end() || !value->is_array() || value->empty())
                        continue;
                    const bool removed = std::any_of(seen.begin(), seen.end(), [&](PrefabNodeId id)
                        { return (*value)[0] == std::to_string(id); });
                    if (!removed)
                        continue;
                    PrefabAuthoredKey key;
                    key.mPrefab = mActiveDocument;
                    key.mAddress = { node.mId };
                    key.mComponent = component.mType;
                    key.mField = field.mName;
                    action.mEntries.push_back(PrefabAuthoredEntry{ key, std::string("null") });
                }
            }
        }
        return action;
    }

    PrefabAuthoredAction PrefabAuthoring::ReparentNode(PrefabNodeId node, PrefabNodeId parent,
        std::size_t siblingIndex)
    {
        return ReparentNodes(std::span<const PrefabNodeId>(&node, 1), parent, siblingIndex);
    }

    PrefabAuthoredAction PrefabAuthoring::ReparentNodes(std::span<const PrefabNodeId> nodes,
        PrefabNodeId parent, std::size_t siblingIndex)
    {
        PrefabAuthoredAction action;
        const PrefabDocument *document = ActivePrefabDocument();
        if (!mActiveDocument || document == nullptr || nodes.empty())
        {
            action.mRefusal = "no prefab document or moving node is active";
            return action;
        }
        const PrefabNode *parentNode = FindNode(*document, SceneNodeAddress{ parent });
        if (parentNode == nullptr)
        {
            action.mRefusal = "the parent node is missing";
            return action;
        }
        if (parentNode->mKind != PrefabNodeKind::Entity)
        {
            action.mRefusal = "a nested Prefab occurrence cannot hold nodes";
            return action;
        }
        std::set<PrefabNodeId> moving;
        std::vector<PrefabNode> moved;
        for (PrefabNodeId id : nodes)
        {
            const auto index = FindNodeIndex(*document, id);
            const auto descendants = DescendantsOf(*document, id);
            if (!index || id == document->mRootNode || id == parent ||
                std::find(descendants.begin(), descendants.end(), parent) != descendants.end())
            {
                action.mRefusal = "a node cannot be moved under itself or its descendant";
                return action;
            }
            if (moving.insert(id).second)
                moved.push_back(document->mNodes[*index]);
        }
        PrefabDocument after = *document;
        std::erase_if(after.mNodes, [&](const PrefabNode &node) { return moving.contains(node.mId); });
        for (PrefabNode &node : moved)
        {
            if (node.mParent != parent && !KeepDocumentPose(mCoordinator.Registry(), *document, node, parent))
            {
                action.mRefusal = "the node's world position does not fit under the new parent";
                return action;
            }
            node.mParent = parent;
            const auto index = InsertionIndex(after, parent, siblingIndex++, node.mId);
            after.mNodes.insert(after.mNodes.begin() + static_cast<std::ptrdiff_t>(index), node);
        }
        // Publish final absolute positions together, including siblings shifted by the move.
        // Read/Write can then restore the same ordering atomically for undo and redo.
        for (const PrefabNode &node : after.mNodes)
        {
            const json record = NodeRecord(after, node);
            if (record == NodeRecord(*document, document->mNodes[*FindNodeIndex(*document, node.mId)]))
                continue;
            PrefabAuthoredKey key;
            key.mPrefab = mActiveDocument;
            key.mAddress = { node.mId };
            action.mEntries.push_back(PrefabAuthoredEntry{ key, record.dump() });
        }
        return action;
    }
}
