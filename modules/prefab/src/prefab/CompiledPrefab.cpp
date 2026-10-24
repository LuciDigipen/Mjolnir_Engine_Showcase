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
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/Entity.h"
#include "scene/ComponentJson.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
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

        void AddEntityRefRelocation(const PrefabNode &node, const PrefabComponentInstance &component,
            const FieldDesc &field, const json &value, std::size_t nodeIndex, std::size_t componentIndex,
            std::size_t fieldIndex, std::size_t templateNodeCount,
            const PrefabDocument &doc, const std::unordered_map<std::string, std::size_t> &docIndexByText,
            const std::vector<std::size_t> &templateIndexOfDoc,
            std::vector<PrefabEntityRefRelocation> &relocations, std::vector<PrefabDiagnostic> &diagnostics)
        {
            if (value.is_null())
                return;

            const std::string context = FieldContext(node, component, field);
            if (!value.is_array())
            {
                AddError(diagnostics, context + " must be null or a node address");
                return;
            }
            if (value.empty())
            {
                AddError(diagnostics, context + " has an empty node address");
                return;
            }
            if (value.size() > 1)
            {
                AddError(diagnostics, context +
                    " crosses a nested prefab; multi-segment addresses are not supported yet");
                return;
            }
            if (!value[0].is_string())
            {
                AddError(diagnostics, context + " segment must be a decimal node id string");
                return;
            }

            const std::string &text = value[0].get_ref<const std::string &>();
            const auto targetIt = docIndexByText.find(text);
            if (targetIt == docIndexByText.end())
            {
                AddError(diagnostics, context + " target \"" + text + "\" is not in this document");
                return;
            }
            if (doc.mNodes[targetIt->second].mKind != PrefabNodeKind::Entity)
            {
                AddError(diagnostics, context + " target \"" + text + "\" is a nested prefab, not an entity");
                return;
            }

            PrefabEntityRefRelocation relocation;
            relocation.mNode = nodeIndex;
            relocation.mComponent = componentIndex;
            relocation.mField = fieldIndex;
            relocation.mTargetNode = templateIndexOfDoc[targetIt->second];
            if (relocation.mTargetNode == kNoTemplateNode || relocation.mNode >= templateNodeCount)
                return;
            relocations.push_back(relocation);
        }

        std::vector<CompiledPrefabNode> CompileNodes(const PrefabDocument &doc,
            const std::vector<std::size_t> &preorder,
            const std::vector<std::size_t> &templateIndexOfDoc,
            const std::unordered_map<PrefabNodeId, std::size_t> &docIndexById,
            const std::unordered_map<std::string, std::size_t> &docIndexByText,
            std::vector<PrefabEntityRefRelocation> &relocations,
            std::vector<PrefabDiagnostic> &diagnostics)
        {
            ComponentRegistry &registry = ComponentRegistry::Instance();
            std::vector<CompiledPrefabNode> nodes;
            nodes.reserve(preorder.size());

            for (const std::size_t docIndex : preorder)
            {
                const PrefabNode &node = doc.mNodes[docIndex];
                CompiledPrefabNode outNode;
                outNode.mAddress = PrefabNodeAddress{ node.mId };

                if (node.mId != doc.mRootNode)
                {
                    const auto parentDoc = docIndexById.find(node.mParent);
                    if (parentDoc != docIndexById.end())
                    {
                        const std::size_t parentTemplate = templateIndexOfDoc[parentDoc->second];
                        if (parentTemplate != kNoTemplateNode)
                            outNode.mParent = parentTemplate;
                    }
                }

                for (const PrefabComponentInstance &instance : node.mComponents)
                {
                    // ValidatePrefab has already warned about an unregistered type
                    const ComponentOps *ops = registry.Find(instance.mType);
                    if (ops == nullptr)
                        continue;

                    CompiledPrefabComponent component;
                    component.mOps = ops;
                    component.mBytes.resize(ops->mSize);
                    if (!component.mBytes.empty() && ops->mDefaultValue.size() == component.mBytes.size())
                        std::memcpy(component.mBytes.data(), ops->mDefaultValue.data(), component.mBytes.size());

                    for (const FieldDesc &field : ops->mFields)
                    {
                        if (field.mKind != FieldKind::EntityRef)
                            continue;
                        field.Write(component.mBytes.data(), &kNullEntity);
                    }

                    for (auto authored = instance.mFields.begin(); authored != instance.mFields.end(); ++authored)
                    {
                        const std::string &fieldName = authored.key();
                        const FieldDesc *field = ops->FindField(fieldName);
                        if (field == nullptr)
                        {
                            AddWarning(diagnostics, NodeName(node.mId) + " component \"" + instance.mType +
                                "\" has unknown field \"" + fieldName + "\"; ignored");
                            continue;
                        }

                        const std::size_t fieldIndex =
                            static_cast<std::size_t>(field - ops->mFields.data());

                        if (field->mKind == FieldKind::EntityRef)
                        {
                            AddEntityRefRelocation(node, instance, *field, authored.value(), nodes.size(),
                                outNode.mComponents.size(), fieldIndex, preorder.size(), doc, docIndexByText,
                                templateIndexOfDoc, relocations, diagnostics);
                            continue;
                        }

                        const std::size_t units = (FieldSize(field->mKind) + sizeof(std::max_align_t) - 1) /
                            sizeof(std::max_align_t);
                        std::vector<std::max_align_t> storage(units);
                        if (!JsonToField(field->mKind, authored.value(), storage.data()))
                        {
                            AddError(diagnostics, FieldContext(node, instance, *field) + " value is not a " +
                                std::string(FieldKindName(field->mKind)) + " this field can hold");
                            continue;
                        }
                        field->Write(component.mBytes.data(), storage.data());
                    }

                    outNode.mComponents.push_back(std::move(component));
                }

                nodes.push_back(std::move(outNode));
            }

            return nodes;
        }
    }

    CompiledPrefab::CompiledPrefab(PrefabId id, uint64_t revision, std::vector<CompiledPrefabNode> nodes,
        std::vector<PrefabEntityRefRelocation> relocations)
        : mId(id)
        , mRevision(revision)
        , mNodes(std::move(nodes))
        , mRelocations(std::move(relocations))
    {
    }

    PrefabCompileResult CompilePrefab(const PrefabDocument &doc)
    {
        PrefabCompileResult result;
        result.mDiagnostics = ValidatePrefab(doc);
        if (HasPrefabErrors(result.mDiagnostics))
            return result;

        for (const PrefabNode &node : doc.mNodes)
        {
            if (node.mKind == PrefabNodeKind::Prefab)
            {
                AddError(result.mDiagnostics, NodeName(node.mId) +
                    " is a nested prefab node; nested prefab nodes are not supported yet");
            }
        }
        if (HasPrefabErrors(result.mDiagnostics))
            return result;

        std::unordered_map<PrefabNodeId, std::size_t> docIndexById;
        std::unordered_map<std::string, std::size_t> docIndexByText;
        std::unordered_map<PrefabNodeId, std::vector<std::size_t>> childrenByParent;
        docIndexById.reserve(doc.mNodes.size());
        docIndexByText.reserve(doc.mNodes.size());

        for (std::size_t i = 0; i < doc.mNodes.size(); ++i)
        {
            const PrefabNode &node = doc.mNodes[i];
            docIndexById.emplace(node.mId, i);
            docIndexByText.emplace(std::to_string(node.mId), i);
            if (node.mId != doc.mRootNode)
                childrenByParent[node.mParent].push_back(i);
        }

        const auto rootIt = docIndexById.find(doc.mRootNode);
        if (rootIt == docIndexById.end())
            return result;

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

        std::vector<std::size_t> templateIndexOfDoc(doc.mNodes.size(), kNoTemplateNode);
        for (std::size_t i = 0; i < preorder.size(); ++i)
            templateIndexOfDoc[preorder[i]] = i;

        std::vector<PrefabEntityRefRelocation> relocations;
        std::vector<CompiledPrefabNode> nodes = CompileNodes(doc, preorder, templateIndexOfDoc, docIndexById,
            docIndexByText, relocations, result.mDiagnostics);

        if (HasPrefabErrors(result.mDiagnostics) || nodes.empty())
            return result;

        result.mPrefab = std::shared_ptr<const CompiledPrefab>(new CompiledPrefab(doc.mId, 0, std::move(nodes),
            std::move(relocations)));
        return result;
    }

    std::vector<PrefabDiagnostic> CompiledPrefabCache::Compile(const PrefabDocument &doc)
    {
        PrefabCompileResult result = CompilePrefab(doc);
        if (!result.mPrefab)
            return result.mDiagnostics;

        uint64_t revision = 1;
        const auto existing = mEntries.find(doc.mId);
        if (existing != mEntries.end())
            revision = existing->second->Revision() + 1;

        std::shared_ptr<const CompiledPrefab> prefab(new CompiledPrefab(doc.mId, revision,
            result.mPrefab->mNodes, result.mPrefab->mRelocations));
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
}
