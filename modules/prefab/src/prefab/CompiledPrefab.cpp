/**
 * @file CompiledPrefab.cpp
 * @author Jackey Cen
 * @brief Immutable component defaults and EntityRef relocations for one prefab revision.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/CompiledPrefab.h"

#include "core/ecs/ComponentFields.h"
#include "prefab/PrefabRegistry.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Entity.h"
#include "scene/ComponentJson.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        constexpr std::size_t kNoTemplateNode = std::numeric_limits<std::size_t>::max();

        std::string NodeName(PrefabNodeId id)
        {
            return "node " + std::to_string(id);
        }

        std::string AddressText(const PrefabNodeAddress &address)
        {
            std::string text;
            for (PrefabNodeId segment : address)
            {
                if (!text.empty())
                    text += '/';
                text += std::to_string(segment);
            }
            return text;
        }

        void AddError(std::vector<PrefabDiagnostic> &diagnostics, std::string message)
        {
            diagnostics.push_back({ PrefabDiagnosticSeverity::Error, {}, std::move(message) });
        }

        void AddWarning(std::vector<PrefabDiagnostic> &diagnostics, std::string message)
        {
            diagnostics.push_back({ PrefabDiagnosticSeverity::Warning, {}, std::move(message) });
        }

        std::string FieldContext(const PrefabNode &node, const PrefabComponentInstance &component,
            const FieldDesc &field)
        {
            return NodeName(node.mId) + " component \"" + component.mType + "\" field \"" +
                std::string(field.mName) + "\"";
        }

        std::string ChainText(const std::vector<PrefabId> &stack)
        {
            std::string text;
            for (PrefabId id : stack)
            {
                if (!text.empty())
                    text += " -> ";
                text += ToString(id);
            }
            return text;
        }

        std::optional<PrefabNodeAddress> ReadAddress(const json &value)
        {
            if (!value.is_array())
                return std::nullopt;

            PrefabNodeAddress address;
            for (const json &segment : value)
            {
                if (!segment.is_string())
                    return std::nullopt;

                const std::string &text = segment.get_ref<const std::string &>();
                if (text.empty() || (text.size() > 1 && text.front() == '0'))
                    return std::nullopt;

                PrefabNodeId id = 0;
                for (char c : text)
                {
                    if (c < '0' || c > '9')
                        return std::nullopt;
                    const PrefabNodeId digit = static_cast<PrefabNodeId>(c - '0');
                    if (id > (std::numeric_limits<PrefabNodeId>::max() - digit) / 10)
                        return std::nullopt;
                    id = id * 10 + digit;
                }
                if (id == kNullPrefabNodeId)
                    return std::nullopt;
                address.push_back(id);
            }

            if (address.empty())
                return std::nullopt;
            return address;
        }

        struct PendingRelocation
        {
            std::size_t mNode = 0;
            std::string mComponent;
            std::size_t mField = 0;
            PrefabNodeAddress mTarget;
            std::string mContext;
            bool mMissingIsError = true;
        };

        using RelocationKey = std::tuple<std::size_t, std::string, std::size_t>;

        struct TemplateBuild
        {
            std::vector<CompiledPrefabNode> mNodes;
            std::map<PrefabNodeAddress, std::size_t> mIndexByAddress;
            std::map<RelocationKey, PendingRelocation> mRelocations;
            std::vector<PrefabId> mDependencies;
        };

        void AddDependency(TemplateBuild &build, PrefabId id)
        {
            if (std::find(build.mDependencies.begin(), build.mDependencies.end(), id) == build.mDependencies.end())
                build.mDependencies.push_back(id);
        }

        std::size_t FindComponentIndex(const CompiledPrefabNode &node, std::string_view type)
        {
            for (std::size_t i = 0; i < node.mComponents.size(); ++i)
            {
                if (node.mComponents[i].mOps->mType.name == type)
                    return i;
            }
            return kNoTemplateNode;
        }

        void EraseComponentRelocations(TemplateBuild &build, std::size_t node, std::string_view type)
        {
            for (auto it = build.mRelocations.begin(); it != build.mRelocations.end();)
            {
                if (it->second.mNode == node && it->second.mComponent == type)
                    it = build.mRelocations.erase(it);
                else
                    ++it;
            }
        }

        CompiledPrefabComponent MakeComponent(const ComponentOps &ops)
        {
            CompiledPrefabComponent component;
            component.mOps = &ops;
            component.mBytes.resize(ops.mSize);
            if (!component.mBytes.empty() && ops.mDefaultValue.size() == component.mBytes.size())
                std::memcpy(component.mBytes.data(), ops.mDefaultValue.data(), component.mBytes.size());

            for (const FieldDesc &field : ops.mFields)
            {
                if (field.mKind != FieldKind::EntityRef)
                    continue;
                field.Write(component.mBytes.data(), &kNullEntity);
            }
            return component;
        }

        bool DecodeField(const FieldDesc &field, const json &value, std::vector<std::byte> &bytes)
        {
            const std::size_t units = (FieldSize(field.mKind) + sizeof(std::max_align_t) - 1) /
                sizeof(std::max_align_t);
            std::vector<std::max_align_t> storage(units);
            if (!JsonToField(field.mKind, value, storage.data()))
                return false;
            return field.Write(bytes.data(), storage.data());
        }

        void AddRelocation(TemplateBuild &build, std::size_t node, const ComponentOps &ops, std::size_t field,
            const json &value, const std::string &context, bool errorOnInvalid,
            std::vector<PrefabDiagnostic> &diagnostics)
        {
            const RelocationKey key{ node, std::string(ops.mType.name), field };
            if (value.is_null())
            {
                build.mRelocations.erase(key);
                return;
            }

            std::optional<PrefabNodeAddress> target = ReadAddress(value);
            if (!target)
            {
                if (errorOnInvalid)
                    AddError(diagnostics, context + " is not a node address");
                else
                    AddWarning(diagnostics, context + " is not a node address; skipped");
                return;
            }

            build.mRelocations.insert_or_assign(key, PendingRelocation{ node, std::string(ops.mType.name), field,
                std::move(*target), context, errorOnInvalid });
        }

        void ApplyPatch(TemplateBuild &build, std::size_t target, const PrefabPatch &patch,
            const PrefabNodeAddress &targetAddress, std::vector<PrefabDiagnostic> &diagnostics)
        {
            ComponentRegistry &registry = ComponentRegistry::Instance();
            const std::string context = "patch on \"" + AddressText(targetAddress) + "\"";
            CompiledPrefabNode &node = build.mNodes[target];

            for (const PrefabFieldOverride &entry : patch.mFields)
            {
                const ComponentOps *ops = registry.Find(entry.mComponent);
                if (ops == nullptr)
                {
                    AddWarning(diagnostics, context + " names unregistered component \"" + entry.mComponent +
                        "\"; skipped");
                    continue;
                }

                const std::size_t componentIndex = FindComponentIndex(node, entry.mComponent);
                if (componentIndex == kNoTemplateNode)
                {
                    AddWarning(diagnostics, context + " names component \"" + entry.mComponent +
                        "\" the target lacks; skipped");
                    continue;
                }

                const FieldDesc *field = ops->FindField(entry.mField);
                if (field == nullptr)
                {
                    AddWarning(diagnostics, context + " names unknown field \"" + entry.mComponent + "." +
                        entry.mField + "\"; skipped");
                    continue;
                }

                const std::size_t fieldIndex = static_cast<std::size_t>(field - ops->mFields.data());
                const std::string fieldContext =
                    context + " field \"" + entry.mComponent + "." + entry.mField + "\"";
                if (field->mKind == FieldKind::EntityRef)
                {
                    AddRelocation(build, target, *ops, fieldIndex, entry.mValue, fieldContext, false, diagnostics);
                    continue;
                }

                if (!DecodeField(*field, entry.mValue, node.mComponents[componentIndex].mBytes))
                    AddWarning(diagnostics, fieldContext + " value does not fit this field; skipped");
            }

            for (const PrefabComponentInstance &added : patch.mAddedComponents)
            {
                const ComponentOps *ops = registry.Find(added.mType);
                if (ops == nullptr)
                {
                    AddWarning(diagnostics, context + " adds unregistered component \"" + added.mType +
                        "\"; skipped");
                    continue;
                }

                CompiledPrefabComponent component = MakeComponent(*ops);
                for (auto authored = added.mFields.begin(); authored != added.mFields.end(); ++authored)
                {
                    const FieldDesc *field = ops->FindField(authored.key());
                    if (field == nullptr)
                    {
                        AddWarning(diagnostics, context + " added component \"" + added.mType +
                            "\" has unknown field \"" + authored.key() + "\"; skipped");
                        continue;
                    }

                    const std::size_t fieldIndex = static_cast<std::size_t>(field - ops->mFields.data());
                    if (field->mKind == FieldKind::EntityRef)
                    {
                        AddRelocation(build, target, *ops, fieldIndex, authored.value(),
                            context + " added component \"" + added.mType + "\" field \"" + authored.key() + "\"",
                            false, diagnostics);
                        continue;
                    }

                    if (!DecodeField(*field, authored.value(), component.mBytes))
                        AddWarning(diagnostics, context + " added component \"" + added.mType + "\" field \"" +
                            authored.key() + "\" value does not fit this field; skipped");
                }

                const std::size_t existing = FindComponentIndex(node, added.mType);
                if (existing != kNoTemplateNode)
                {
                    EraseComponentRelocations(build, target, added.mType);
                    node.mComponents[existing] = std::move(component);
                }
                else
                {
                    node.mComponents.push_back(std::move(component));
                }
            }

            for (const std::string &removed : patch.mRemovedComponents)
            {
                const std::size_t componentIndex = FindComponentIndex(node, removed);
                if (componentIndex == kNoTemplateNode)
                {
                    AddWarning(diagnostics, context + " removes component \"" + removed +
                        "\" the target lacks; skipped");
                    continue;
                }

                EraseComponentRelocations(build, target, removed);
                node.mComponents.erase(node.mComponents.begin() + static_cast<std::ptrdiff_t>(componentIndex));
            }
        }

        bool BuildTemplate(const PrefabDocument &doc, const PrefabDocumentLookup &lookup,
            std::vector<PrefabId> &stack, std::size_t depth, std::vector<PrefabDiagnostic> &diagnostics,
            TemplateBuild &build)
        {
            stack.push_back(doc.mId);

            std::unordered_map<PrefabNodeId, std::size_t> docIndexById;
            std::unordered_map<PrefabNodeId, std::vector<std::size_t>> childrenByParent;
            docIndexById.reserve(doc.mNodes.size());

            for (std::size_t i = 0; i < doc.mNodes.size(); ++i)
            {
                const PrefabNode &node = doc.mNodes[i];
                docIndexById.emplace(node.mId, i);
                if (node.mId != doc.mRootNode)
                    childrenByParent[node.mParent].push_back(i);
            }

            const auto rootIt = docIndexById.find(doc.mRootNode);
            if (rootIt == docIndexById.end())
            {
                stack.pop_back();
                return false;
            }

            std::vector<std::size_t> preorder;
            preorder.reserve(doc.mNodes.size());
            std::vector<std::size_t> pending{ rootIt->second };
            while (!pending.empty())
            {
                const std::size_t docIndex = pending.back();
                pending.pop_back();
                preorder.push_back(docIndex);

                const auto childrenIt = childrenByParent.find(doc.mNodes[docIndex].mId);
                if (childrenIt == childrenByParent.end())
                    continue;
                for (auto child = childrenIt->second.rbegin(); child != childrenIt->second.rend(); ++child)
                    pending.push_back(*child);
            }

            std::unordered_map<PrefabNodeId, std::size_t> templateByDocNode;
            bool ok = true;

            for (const std::size_t docIndex : preorder)
            {
                const PrefabNode &node = doc.mNodes[docIndex];
                if (node.mKind == PrefabNodeKind::Entity)
                {
                    const std::size_t index = build.mNodes.size();
                    CompiledPrefabNode outNode;
                    outNode.mAddress = PrefabNodeAddress{ node.mId };

                    if (node.mId != doc.mRootNode)
                    {
                        const auto parent = templateByDocNode.find(node.mParent);
                        if (parent != templateByDocNode.end())
                            outNode.mParent = parent->second;
                    }

                    for (const PrefabComponentInstance &instance : node.mComponents)
                    {
                        // ValidatePrefab has already warned about an unregistered type
                        const ComponentOps *ops = ComponentRegistry::Instance().Find(instance.mType);
                        if (ops == nullptr)
                            continue;

                        CompiledPrefabComponent component = MakeComponent(*ops);
                        for (auto authored = instance.mFields.begin(); authored != instance.mFields.end(); ++authored)
                        {
                            const FieldDesc *field = ops->FindField(authored.key());
                            if (field == nullptr)
                            {
                                AddWarning(diagnostics, NodeName(node.mId) + " component \"" + instance.mType +
                                    "\" has unknown field \"" + authored.key() + "\"; ignored");
                                continue;
                            }

                            const std::size_t fieldIndex =
                                static_cast<std::size_t>(field - ops->mFields.data());
                            if (field->mKind == FieldKind::EntityRef)
                            {
                                AddRelocation(build, index, *ops, fieldIndex, authored.value(),
                                    FieldContext(node, instance, *field), true, diagnostics);
                                continue;
                            }

                            if (!DecodeField(*field, authored.value(), component.mBytes))
                            {
                                AddError(diagnostics, FieldContext(node, instance, *field) + " value is not a " +
                                    std::string(FieldKindName(field->mKind)) + " this field can hold");
                            }
                        }

                        outNode.mComponents.push_back(std::move(component));
                    }

                    build.mNodes.push_back(std::move(outNode));
                    build.mIndexByAddress.emplace(build.mNodes.back().mAddress, index);
                    templateByDocNode.emplace(node.mId, index);
                }
                else
                {
                    const PrefabId target = node.mPrefab;
                    if (std::find(stack.begin(), stack.end(), target) != stack.end())
                    {
                        AddError(diagnostics, "prefab chain " + ChainText(stack) + " -> " + ToString(target) +
                            " is a cycle");
                        ok = false;
                        break;
                    }
                    if (depth + 1 > kMaxPrefabNestingDepth)
                    {
                        AddError(diagnostics, "prefab chain " + ChainText(stack) + " -> " + ToString(target) +
                            " exceeds the nesting depth limit " + std::to_string(kMaxPrefabNestingDepth));
                        ok = false;
                        break;
                    }

                    const PrefabDocument *inner = lookup ? lookup(target) : nullptr;
                    if (inner == nullptr)
                    {
                        AddError(diagnostics, "prefab chain " + ChainText(stack) + " -> " + ToString(target) +
                            " names a document that is not registered");
                        ok = false;
                        break;
                    }

                    TemplateBuild innerBuild;
                    if (!BuildTemplate(*inner, lookup, stack, depth + 1, diagnostics, innerBuild))
                    {
                        ok = false;
                        break;
                    }

                    std::optional<std::size_t> occurrenceParent;
                    if (node.mId != doc.mRootNode)
                    {
                        const auto parent = templateByDocNode.find(node.mParent);
                        if (parent != templateByDocNode.end())
                            occurrenceParent = parent->second;
                    }

                    const std::size_t base = build.mNodes.size();
                    for (CompiledPrefabNode &innerNode : innerBuild.mNodes)
                    {
                        innerNode.mAddress.insert(innerNode.mAddress.begin(), node.mId);
                        if (innerNode.mParent.has_value())
                            innerNode.mParent = base + *innerNode.mParent;
                        else
                            innerNode.mParent = occurrenceParent;

                        build.mNodes.push_back(std::move(innerNode));
                        if (build.mNodes.size() > kMaxCompiledPrefabNodes)
                        {
                            AddError(diagnostics, "expanded template for prefab " + ToString(doc.mId) +
                                " exceeds the node limit " + std::to_string(kMaxCompiledPrefabNodes));
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        break;

                    for (std::size_t i = base; i < build.mNodes.size(); ++i)
                        build.mIndexByAddress.emplace(build.mNodes[i].mAddress, i);

                    for (auto &[key, pendingRelocation] : innerBuild.mRelocations)
                    {
                        PendingRelocation copy = pendingRelocation;
                        copy.mNode += base;
                        copy.mTarget.insert(copy.mTarget.begin(), node.mId);
                        build.mRelocations.insert_or_assign(
                            RelocationKey{ copy.mNode, copy.mComponent, copy.mField }, std::move(copy));
                    }

                    for (PrefabId dependency : innerBuild.mDependencies)
                        AddDependency(build, dependency);
                    AddDependency(build, target);

                    for (const PrefabPatch &patch : node.mPatches)
                    {
                        if (patch.mTarget.empty())
                            continue;

                        PrefabNodeAddress targetAddress{ node.mId };
                        targetAddress.insert(targetAddress.end(), patch.mTarget.begin(), patch.mTarget.end());

                        const auto targetIt = build.mIndexByAddress.find(targetAddress);
                        if (targetIt == build.mIndexByAddress.end())
                        {
                            AddWarning(diagnostics, "patch on \"" + AddressText(targetAddress) +
                                "\" is stale; skipped");
                            continue;
                        }
                        ApplyPatch(build, targetIt->second, patch, targetAddress, diagnostics);
                    }
                }

                if (build.mNodes.size() > kMaxCompiledPrefabNodes)
                {
                    AddError(diagnostics, "expanded template for prefab " + ToString(doc.mId) +
                        " exceeds the node limit " + std::to_string(kMaxCompiledPrefabNodes));
                    ok = false;
                    break;
                }
            }

            stack.pop_back();
            return ok;
        }
    }

    PrefabDocumentLookup RegistryLookup(const PrefabRegistry &registry)
    {
        return [&registry](PrefabId id) { return registry.Find(id); };
    }

    CompiledPrefab::CompiledPrefab(PrefabId id, uint64_t revision, std::vector<CompiledPrefabNode> nodes,
        std::vector<PrefabEntityRefRelocation> relocations, std::vector<PrefabId> dependencies)
        : mId(id)
        , mRevision(revision)
        , mNodes(std::move(nodes))
        , mRelocations(std::move(relocations))
        , mDependencies(std::move(dependencies))
    {
    }

    PrefabCompileResult CompilePrefab(const PrefabDocument &doc, const PrefabDocumentLookup &lookup)
    {
        PrefabCompileResult result;
        result.mDiagnostics = ValidatePrefab(doc);
        if (HasPrefabErrors(result.mDiagnostics))
            return result;

        TemplateBuild build;
        std::vector<PrefabId> stack;
        if (!BuildTemplate(doc, lookup, stack, 0, result.mDiagnostics, build))
            return result;
        if (HasPrefabErrors(result.mDiagnostics) || build.mNodes.empty())
            return result;

        std::vector<PrefabEntityRefRelocation> relocations;
        for (const auto &[key, pendingRelocation] : build.mRelocations)
        {
            (void)key;
            const CompiledPrefabNode &node = build.mNodes[pendingRelocation.mNode];
            const std::size_t component = FindComponentIndex(node, pendingRelocation.mComponent);
            if (component == kNoTemplateNode)
                continue;

            const auto target = build.mIndexByAddress.find(pendingRelocation.mTarget);
            if (target == build.mIndexByAddress.end())
            {
                const std::string message = pendingRelocation.mContext + " target \"" +
                    AddressText(pendingRelocation.mTarget) + "\" is not in the expanded template";
                if (pendingRelocation.mMissingIsError)
                    AddError(result.mDiagnostics, message);
                else
                    AddWarning(result.mDiagnostics, message + "; skipped");
                continue;
            }

            relocations.push_back(PrefabEntityRefRelocation{ pendingRelocation.mNode, component,
                pendingRelocation.mField, target->second });
        }
        if (HasPrefabErrors(result.mDiagnostics))
            return result;

        result.mPrefab = std::shared_ptr<const CompiledPrefab>(new CompiledPrefab(doc.mId, 0,
            std::move(build.mNodes), std::move(relocations), std::move(build.mDependencies)));
        return result;
    }

    std::vector<PrefabDiagnostic> CompiledPrefabCache::Compile(const PrefabDocument &doc,
        const PrefabDocumentLookup &lookup)
    {
        PrefabCompileResult result = CompilePrefab(doc, lookup);
        if (!result.mPrefab)
            return result.mDiagnostics;

        uint64_t revision = 1;
        const auto existing = mEntries.find(doc.mId);
        if (existing != mEntries.end())
            revision = existing->second->Revision() + 1;

        std::shared_ptr<const CompiledPrefab> prefab(new CompiledPrefab(doc.mId, revision,
            result.mPrefab->mNodes, result.mPrefab->mRelocations, result.mPrefab->mDependencies));
        mEntries.insert_or_assign(doc.mId, std::move(prefab));
        return result.mDiagnostics;
    }

    std::shared_ptr<const CompiledPrefab> CompiledPrefabCache::Find(PrefabId id) const
    {
        const auto it = mEntries.find(id);
        return it == mEntries.end() ? nullptr : it->second;
    }

    void CompiledPrefabCache::Remove(PrefabId id)
    {
        mEntries.erase(id);
    }

    std::vector<PrefabId> CompiledPrefabCache::Dependents(PrefabId id) const
    {
        std::vector<PrefabId> dependents;
        for (const auto &[entryId, prefab] : mEntries)
        {
            if (entryId == id)
                continue;
            const std::span<const PrefabId> dependencies = prefab->Dependencies();
            if (std::find(dependencies.begin(), dependencies.end(), id) != dependencies.end())
                dependents.push_back(entryId);
        }
        return dependents;
    }
}
