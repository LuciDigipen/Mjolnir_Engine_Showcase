/**
 * @file PrefabFormat.cpp
 * @author Jackey Cen
 * @brief Versioned .prefab JSON source format: parse, validate, write.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabFormat.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"

#include "serial/JsonFile.h"

#include <iterator>
#include <limits>
#include <set>
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
        using NodeTable = std::unordered_map<PrefabNodeId, const PrefabNode *>;

        struct Reader
        {
            std::vector<PrefabDiagnostic> &mDiagnostics;
            std::string_view mPath;

            void Error(std::string message) const
            {
                mDiagnostics.push_back({ PrefabDiagnosticSeverity::Error, std::string(mPath), std::move(message) });
            }

            void Warning(std::string message) const
            {
                mDiagnostics.push_back({ PrefabDiagnosticSeverity::Warning, std::string(mPath), std::move(message) });
            }
        };

        std::string NodeName(PrefabNodeId id)
        {
            return "node " + std::to_string(id);
        }

        std::optional<PrefabNodeId> ParseNodeIdText(std::string_view text)
        {
            if (text.empty() || (text.size() > 1 && text.front() == '0'))
                return std::nullopt;

            PrefabNodeId value = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                const PrefabNodeId digit = static_cast<PrefabNodeId>(c - '0');
                if (value > (std::numeric_limits<PrefabNodeId>::max() - digit) / 10)
                    return std::nullopt;
                value = value * 10 + digit;
            }
            if (value == kNullPrefabNodeId)
                return std::nullopt;
            return value;
        }

        std::optional<PrefabNodeId> ReadNodeId(const json &value, const Reader &reader, const std::string &context)
        {
            if (value.is_number())
            {
                reader.Error(context + " must be written as a string; JSON numbers lose precision above 2^53");
                return std::nullopt;
            }
            if (!value.is_string())
            {
                reader.Error(context + " must be a decimal string");
                return std::nullopt;
            }
            const std::string &text = value.get_ref<const std::string &>();
            std::optional<PrefabNodeId> parsed = ParseNodeIdText(text);
            if (!parsed.has_value())
                reader.Error(context + " \"" + text + "\" is not a valid node id (strict decimal string, non-zero, fits 64 bits)");
            return parsed;
        }

        std::optional<std::string> ReadString(const json &object, std::string_view key, const Reader &reader,
            const std::string &context)
        {
            const auto it = object.find(key);
            if (it == object.end())
            {
                reader.Error(context + " is missing \"" + std::string(key) + "\"");
                return std::nullopt;
            }
            if (!it->is_string())
            {
                reader.Error(context + " \"" + std::string(key) + "\" must be a string");
                return std::nullopt;
            }
            return it->get<std::string>();
        }

        std::optional<PrefabComponentInstance> ReadComponent(const json &blob, const Reader &reader,
            const std::string &context)
        {
            if (!blob.is_object())
            {
                reader.Error(context + " must be an object");
                return std::nullopt;
            }
            std::optional<std::string> type = ReadString(blob, "type", reader, context);
            if (!type.has_value())
                return std::nullopt;
            if (type->empty())
            {
                reader.Error(context + " has an empty \"type\"");
                return std::nullopt;
            }

            PrefabComponentInstance component;
            component.mType = std::move(*type);
            const auto fields = blob.find("fields");
            if (fields != blob.end())
            {
                if (!fields->is_object())
                {
                    reader.Error(context + " \"" + component.mType + "\" fields must be an object");
                    return std::nullopt;
                }
                component.mFields = *fields;
            }
            return component;
        }

        std::optional<PrefabPatch> ReadPatch(const json &blob, const Reader &reader, const std::string &context)
        {
            if (!blob.is_object())
            {
                reader.Error(context + " must be an object");
                return std::nullopt;
            }

            PrefabPatch patch;
            const auto target = blob.find("target");
            if (target == blob.end())
            {
                reader.Error(context + " is missing \"target\"");
                return std::nullopt;
            }
            if (!target->is_array())
            {
                reader.Error(context + " \"target\" must be an array of node id strings");
                return std::nullopt;
            }
            bool targetOk = true;
            for (const json &segment : *target)
            {
                std::optional<PrefabNodeId> id = ReadNodeId(segment, reader, context + " target segment");
                targetOk = targetOk && id.has_value();
                if (id.has_value())
                    patch.mTarget.push_back(*id);
            }
            if (!targetOk)
                return std::nullopt;

            const auto fields = blob.find("fields");
            if (fields != blob.end())
            {
                if (!fields->is_array())
                {
                    reader.Error(context + " \"fields\" must be an array");
                    return std::nullopt;
                }
                for (const json &entry : *fields)
                {
                    if (!entry.is_object())
                    {
                        reader.Error(context + " field override must be an object");
                        return std::nullopt;
                    }
                    std::optional<std::string> component = ReadString(entry, "component", reader, context + " field override");
                    std::optional<std::string> field = ReadString(entry, "field", reader, context + " field override");
                    if (!component.has_value() || !field.has_value())
                        return std::nullopt;
                    if (component->empty() || field->empty())
                    {
                        reader.Error(context + " field override names must not be empty");
                        return std::nullopt;
                    }
                    const auto value = entry.find("value");
                    if (value == entry.end())
                    {
                        reader.Error(context + " field override is missing \"value\"");
                        return std::nullopt;
                    }
                    patch.mFields.push_back({ std::move(*component), std::move(*field), *value });
                }
            }

            const auto added = blob.find("addedComponents");
            if (added != blob.end())
            {
                if (!added->is_array())
                {
                    reader.Error(context + " \"addedComponents\" must be an array");
                    return std::nullopt;
                }
                for (const json &entry : *added)
                {
                    std::optional<PrefabComponentInstance> component = ReadComponent(entry, reader, context + " added component");
                    if (!component.has_value())
                        return std::nullopt;
                    patch.mAddedComponents.push_back(std::move(*component));
                }
            }

            const auto removed = blob.find("removedComponents");
            if (removed != blob.end())
            {
                if (!removed->is_array())
                {
                    reader.Error(context + " \"removedComponents\" must be an array");
                    return std::nullopt;
                }
                for (const json &entry : *removed)
                {
                    if (!entry.is_string() || entry.get_ref<const std::string &>().empty())
                    {
                        reader.Error(context + " removed component names must be non-empty strings");
                        return std::nullopt;
                    }
                    patch.mRemovedComponents.push_back(entry.get<std::string>());
                }
            }
            return patch;
        }

        std::optional<PrefabNode> ReadNode(const json &blob, const Reader &reader, std::size_t index)
        {
            const std::string context = "nodes[" + std::to_string(index) + "]";
            if (!blob.is_object())
            {
                reader.Error(context + " must be an object");
                return std::nullopt;
            }

            PrefabNode node;
            const auto id = blob.find("id");
            if (id == blob.end())
            {
                reader.Error(context + " is missing \"id\"");
                return std::nullopt;
            }
            const std::optional<PrefabNodeId> nodeId = ReadNodeId(*id, reader, context + " id");
            if (!nodeId.has_value())
                return std::nullopt;
            node.mId = *nodeId;
            const std::string name = NodeName(node.mId);

            const std::optional<std::string> kind = ReadString(blob, "kind", reader, name);
            if (!kind.has_value())
                return std::nullopt;
            if (*kind == "entity")
                node.mKind = PrefabNodeKind::Entity;
            else if (*kind == "prefab")
                node.mKind = PrefabNodeKind::Prefab;
            else
            {
                reader.Error(name + " kind \"" + *kind + "\" must be \"entity\" or \"prefab\"");
                return std::nullopt;
            }

            const auto parent = blob.find("parent");
            if (parent != blob.end())
            {
                const std::optional<PrefabNodeId> parentId = ReadNodeId(*parent, reader, name + " parent");
                if (!parentId.has_value())
                    return std::nullopt;
                node.mParent = *parentId;
            }

            if (node.mKind == PrefabNodeKind::Entity)
            {
                const auto components = blob.find("components");
                if (components != blob.end())
                {
                    if (!components->is_array())
                    {
                        reader.Error(name + " \"components\" must be an array");
                        return std::nullopt;
                    }
                    for (const json &entry : *components)
                    {
                        std::optional<PrefabComponentInstance> component = ReadComponent(entry, reader, name + " component");
                        if (!component.has_value())
                            return std::nullopt;
                        node.mComponents.push_back(std::move(*component));
                    }
                }
            }
            else
            {
                const auto prefab = blob.find("prefab");
                if (prefab == blob.end())
                {
                    reader.Error(name + " is missing \"prefab\"");
                    return std::nullopt;
                }
                if (!prefab->is_string())
                {
                    reader.Error(name + " \"prefab\" must be a UUID string");
                    return std::nullopt;
                }
                const std::optional<PrefabId> target = ParsePrefabId(prefab->get_ref<const std::string &>());
                if (!target.has_value())
                {
                    reader.Error(name + " \"prefab\" is not a valid non-nil UUID");
                    return std::nullopt;
                }
                node.mPrefab = *target;

                const auto patches = blob.find("patches");
                if (patches != blob.end())
                {
                    if (!patches->is_array())
                    {
                        reader.Error(name + " \"patches\" must be an array");
                        return std::nullopt;
                    }
                    for (const json &entry : *patches)
                    {
                        std::optional<PrefabPatch> patch = ReadPatch(entry, reader, name + " patch");
                        if (!patch.has_value())
                            return std::nullopt;
                        node.mPatches.push_back(std::move(*patch));
                    }
                }
            }
            return node;
        }

        void CheckAddressStart(const PrefabNodeAddress &address, const Reader &reader, const std::string &context,
            const NodeTable &byId)
        {
            const auto it = byId.find(address.front());
            if (it == byId.end())
            {
                reader.Error(context + " starts at " + NodeName(address.front()) + ", which is not in this document");
                return;
            }
            if (address.size() == 1)
            {
                if (it->second->mKind != PrefabNodeKind::Entity)
                    reader.Error(context + " must end at an entity node; " + NodeName(address.front()) + " is a nested reference");
            }
            else if (it->second->mKind != PrefabNodeKind::Prefab)
            {
                reader.Error(context + " passes through " + NodeName(address.front()) + ", which is not a nested reference");
            }
        }

        void CheckEntityRefValue(const json &value, const Reader &reader, const std::string &context,
            const NodeTable &byId)
        {
            if (value.is_null())
                return;
            if (!value.is_array())
            {
                reader.Error(context + " must be null or a node address; runtime Entity handles are never persisted");
                return;
            }

            PrefabNodeAddress address;
            bool ok = true;
            for (const json &segment : value)
            {
                std::optional<PrefabNodeId> id = ReadNodeId(segment, reader, context + " segment");
                ok = ok && id.has_value();
                if (id.has_value())
                    address.push_back(*id);
            }
            if (!ok)
                return;
            if (address.empty())
            {
                reader.Error(context + " is an empty node address");
                return;
            }
            CheckAddressStart(address, reader, context, byId);
        }

        void CheckComponent(const PrefabComponentInstance &component, const Reader &reader, const std::string &context,
            const NodeTable &byId)
        {
            if (!component.mFields.is_object())
            {
                reader.Error(context + " \"" + component.mType + "\" fields must be an object");
                return;
            }
            const ComponentOps *ops = ComponentRegistry::Instance().Find(component.mType);
            if (ops == nullptr)
            {
                reader.Warning(context + " component type \"" + component.mType +
                    "\" is not registered in this build; it is kept verbatim");
                return;
            }
            for (const FieldDesc &field : ops->mFields)
            {
                if (field.mKind != FieldKind::EntityRef)
                    continue;
                const auto it = component.mFields.find(std::string(field.mName));
                if (it == component.mFields.end())
                    continue;
                CheckEntityRefValue(*it, reader,
                    context + " \"" + component.mType + "." + std::string(field.mName) + "\"", byId);
            }
        }

        void CheckPatch(const PrefabPatch &patch, const Reader &reader, const std::string &nodeName,
            const NodeTable &byId)
        {
            const std::string context = nodeName + " patch";
            // Relative to the referenced prefab, so only the shape is checked.
            if (patch.mTarget.empty())
            {
                reader.Error(context + " has an empty target address");
            }
            else
            {
                for (PrefabNodeId segment : patch.mTarget)
                {
                    if (segment == kNullPrefabNodeId)
                        reader.Error(context + " target has a zero segment; node ids start at 1");
                }
            }

            std::unordered_set<std::string_view> addedTypes;
            for (const PrefabComponentInstance &component : patch.mAddedComponents)
            {
                if (!addedTypes.insert(component.mType).second)
                    reader.Error(context + " adds component type \"" + component.mType + "\" twice");
                CheckComponent(component, reader, context + " added", byId);
            }

            std::unordered_set<std::string_view> removedTypes;
            for (const std::string &type : patch.mRemovedComponents)
            {
                if (!removedTypes.insert(type).second)
                    reader.Error(context + " removes component type \"" + type + "\" twice");
            }

            for (const PrefabFieldOverride &entry : patch.mFields)
            {
                const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mComponent);
                if (ops == nullptr)
                    continue;
                const FieldDesc *field = ops->FindField(entry.mField);
                if (field == nullptr || field->mKind != FieldKind::EntityRef)
                    continue;
                CheckEntityRefValue(entry.mValue, reader,
                    context + " override \"" + entry.mComponent + "." + entry.mField + "\"", byId);
            }
        }

        json ComponentToJson(const PrefabComponentInstance &component)
        {
            json out;
            out["type"] = component.mType;
            out["fields"] = component.mFields.is_object() ? component.mFields : json::object();
            return out;
        }

        json PatchToJson(const PrefabPatch &patch)
        {
            json out;
            json target = json::array();
            for (PrefabNodeId segment : patch.mTarget)
                target.push_back(std::to_string(segment));
            out["target"] = std::move(target);

            if (!patch.mFields.empty())
            {
                json fields = json::array();
                for (const PrefabFieldOverride &entry : patch.mFields)
                {
                    json overrideJson;
                    overrideJson["component"] = entry.mComponent;
                    overrideJson["field"] = entry.mField;
                    overrideJson["value"] = entry.mValue;
                    fields.push_back(std::move(overrideJson));
                }
                out["fields"] = std::move(fields);
            }
            if (!patch.mAddedComponents.empty())
            {
                json added = json::array();
                for (const PrefabComponentInstance &component : patch.mAddedComponents)
                    added.push_back(ComponentToJson(component));
                out["addedComponents"] = std::move(added);
            }
            if (!patch.mRemovedComponents.empty())
                out["removedComponents"] = patch.mRemovedComponents;
            return out;
        }
    }

    bool HasPrefabErrors(std::span<const PrefabDiagnostic> diagnostics)
    {
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                return true;
        }
        return false;
    }

    PrefabParseResult ParsePrefab(const json &root, std::string_view path)
    {
        PrefabParseResult result;
        const Reader reader{ result.mDiagnostics, path };

        if (!root.is_object())
        {
            reader.Error("prefab file root must be a JSON object");
            return result;
        }

        bool decodeOk = true;
        const std::string versionText = std::to_string(kPrefabFormatVersion);
        const auto version = root.find("formatVersion");
        if (version == root.end())
        {
            reader.Error("missing \"formatVersion\"; expected " + versionText);
            decodeOk = false;
        }
        else if (!version->is_number_integer())
        {
            reader.Error("\"formatVersion\" must be the number " + versionText);
            decodeOk = false;
        }
        else
        {
            const bool matches = version->is_number_unsigned()
                ? version->get<uint64_t>() == static_cast<uint64_t>(kPrefabFormatVersion)
                : version->get<int64_t>() == kPrefabFormatVersion;
            const bool newer = version->is_number_unsigned()
                ? version->get<uint64_t>() > static_cast<uint64_t>(kPrefabFormatVersion)
                : version->get<int64_t>() > kPrefabFormatVersion;
            if (!matches)
            {
                if (newer)
                {
                    reader.Error("this file is from a newer prefab format (version " + version->dump() +
                        "); this build reads version " + versionText);
                }
                else
                {
                    reader.Error("unsupported prefab format version " + version->dump() + "; expected " + versionText);
                }
                decodeOk = false;
            }
        }

        PrefabDocument doc;
        const auto prefabId = root.find("prefabId");
        if (prefabId == root.end() || !prefabId->is_string())
        {
            reader.Error("\"prefabId\" must be a non-nil UUID string");
            decodeOk = false;
        }
        else
        {
            const std::optional<PrefabId> id = ParsePrefabId(prefabId->get_ref<const std::string &>());
            if (!id.has_value())
            {
                reader.Error("\"prefabId\" \"" + prefabId->get_ref<const std::string &>() + "\" is not a valid non-nil UUID");
                decodeOk = false;
            }
            else
            {
                doc.mId = *id;
            }
        }

        const auto nextNode = root.find("nextNodeId");
        if (nextNode == root.end())
        {
            reader.Error("missing \"nextNodeId\"");
            decodeOk = false;
        }
        else
        {
            const std::optional<PrefabNodeId> parsed = ReadNodeId(*nextNode, reader, "\"nextNodeId\"");
            if (parsed.has_value())
                doc.mNextNodeId = *parsed;
            else
                decodeOk = false;
        }

        const auto rootNode = root.find("rootNode");
        if (rootNode == root.end())
        {
            reader.Error("missing \"rootNode\"");
            decodeOk = false;
        }
        else
        {
            const std::optional<PrefabNodeId> parsed = ReadNodeId(*rootNode, reader, "\"rootNode\"");
            if (parsed.has_value())
                doc.mRootNode = *parsed;
            else
                decodeOk = false;
        }

        const auto nodes = root.find("nodes");
        if (nodes == root.end() || !nodes->is_array())
        {
            reader.Error("\"nodes\" must be an array");
            decodeOk = false;
        }
        else
        {
            for (std::size_t i = 0; i < nodes->size(); ++i)
            {
                std::optional<PrefabNode> node = ReadNode((*nodes)[i], reader, i);
                decodeOk = decodeOk && node.has_value();
                if (node.has_value())
                    doc.mNodes.push_back(std::move(*node));
            }
        }

        if (!decodeOk)
            return result;

        std::vector<PrefabDiagnostic> semantic = ValidatePrefab(doc, path);
        result.mDiagnostics.insert(result.mDiagnostics.end(),
            std::make_move_iterator(semantic.begin()), std::make_move_iterator(semantic.end()));
        if (!HasPrefabErrors(result.mDiagnostics))
            result.mDocument = std::move(doc);
        return result;
    }

    std::vector<PrefabDiagnostic> ValidatePrefab(const PrefabDocument &doc, std::string_view path)
    {
        std::vector<PrefabDiagnostic> diagnostics;
        const Reader reader{ diagnostics, path };

        if (doc.mId.IsNull())
            reader.Error("prefab id is the nil UUID; generate a fresh one");

        NodeTable byId;
        for (const PrefabNode &node : doc.mNodes)
        {
            if (node.mId == kNullPrefabNodeId)
                reader.Error("a node uses id 0; node ids start at 1");
            else if (node.mId >= doc.mNextNodeId)
                reader.Error(NodeName(node.mId) + " is at or above nextNodeId " + std::to_string(doc.mNextNodeId) +
                    "; the high-water mark is corrupt, raise it or renumber the node");
            if (!byId.emplace(node.mId, &node).second)
                reader.Error("duplicate node id " + std::to_string(node.mId));
        }

        const PrefabNode *root = nullptr;
        if (doc.mRootNode == kNullPrefabNodeId)
        {
            reader.Error("rootNode is missing; name the root entity node");
        }
        else
        {
            const auto it = byId.find(doc.mRootNode);
            if (it == byId.end())
            {
                reader.Error("rootNode " + std::to_string(doc.mRootNode) + " does not name a node in this document");
            }
            else
            {
                root = it->second;
                if (root->mKind != PrefabNodeKind::Entity)
                    reader.Error("root " + NodeName(doc.mRootNode) + " must be an entity node, not a nested reference");
                if (root->mParent != kNullPrefabNodeId)
                    reader.Error("root " + NodeName(doc.mRootNode) + " must not have a parent");
            }
        }

        for (const PrefabNode &node : doc.mNodes)
        {
            if (node.mId == doc.mRootNode)
                continue;
            if (node.mParent == kNullPrefabNodeId)
            {
                reader.Error(NodeName(node.mId) + " has no parent; every node but the root needs one");
                continue;
            }
            const auto it = byId.find(node.mParent);
            if (it == byId.end())
                reader.Error(NodeName(node.mId) + " parent " + std::to_string(node.mParent) +
                    " does not name a node in this document");
            else if (it->second->mKind != PrefabNodeKind::Entity)
                reader.Error(NodeName(node.mId) + " is parented to nested-reference " + NodeName(node.mParent) +
                    "; only entity nodes have children");
        }

        std::set<PrefabNodeId> cycleNodes;
        for (const PrefabNode &node : doc.mNodes)
        {
            std::vector<PrefabNodeId> chain;
            std::unordered_set<PrefabNodeId> inChain;
            PrefabNodeId current = node.mId;
            bool closed = false;
            while (current != kNullPrefabNodeId)
            {
                if (!inChain.insert(current).second)
                {
                    closed = true;
                    break;
                }
                chain.push_back(current);
                const auto it = byId.find(current);
                if (it == byId.end())
                    break;
                current = it->second->mParent;
            }
            if (!closed)
                continue;
            bool inCycle = false;
            for (PrefabNodeId link : chain)
            {
                inCycle = inCycle || link == current;
                if (inCycle)
                    cycleNodes.insert(link);
            }
        }
        for (PrefabNodeId id : cycleNodes)
            reader.Error(NodeName(id) + " is in a parent cycle; reparent it under the root");

        if (root != nullptr && root->mKind == PrefabNodeKind::Entity)
        {
            std::unordered_map<PrefabNodeId, std::vector<PrefabNodeId>> children;
            for (const PrefabNode &node : doc.mNodes)
            {
                if (node.mId == doc.mRootNode || node.mParent == kNullPrefabNodeId)
                    continue;
                if (byId.contains(node.mParent))
                    children[node.mParent].push_back(node.mId);
            }

            std::unordered_set<PrefabNodeId> reached{ doc.mRootNode };
            std::vector<PrefabNodeId> pending{ doc.mRootNode };
            while (!pending.empty())
            {
                const PrefabNodeId current = pending.back();
                pending.pop_back();
                for (PrefabNodeId child : children[current])
                {
                    if (reached.insert(child).second)
                        pending.push_back(child);
                }
            }
            for (const PrefabNode &node : doc.mNodes)
            {
                if (!reached.contains(node.mId) && !cycleNodes.contains(node.mId))
                    reader.Error(NodeName(node.mId) + " is unreachable from the root; reparent or delete it");
            }
        }

        for (const PrefabNode &node : doc.mNodes)
        {
            const std::string name = NodeName(node.mId);
            if (node.mKind == PrefabNodeKind::Entity)
            {
                std::unordered_set<std::string_view> types;
                for (const PrefabComponentInstance &component : node.mComponents)
                {
                    if (!types.insert(component.mType).second)
                        reader.Error(name + " has duplicate component type \"" + component.mType + "\"");
                    CheckComponent(component, reader, name + " component", byId);
                }
            }
            else
            {
                if (node.mPrefab.IsNull())
                    reader.Error(name + " has an invalid (nil) prefab target UUID");
                else if (node.mPrefab == doc.mId)
                    reader.Error(name + " references this prefab itself; nested targets must be other prefabs");
                for (const PrefabPatch &patch : node.mPatches)
                    CheckPatch(patch, reader, name, byId);
            }
        }

        return diagnostics;
    }

    nlohmann::json PrefabToJson(const PrefabDocument &doc)
    {
        json root;
        root["formatVersion"] = kPrefabFormatVersion;
        root["prefabId"] = ToString(doc.mId);
        root["nextNodeId"] = std::to_string(doc.mNextNodeId);
        root["rootNode"] = std::to_string(doc.mRootNode);

        json nodes = json::array();
        for (const PrefabNode &node : doc.mNodes)
        {
            json out;
            out["id"] = std::to_string(node.mId);
            out["kind"] = node.mKind == PrefabNodeKind::Entity ? "entity" : "prefab";
            if (node.mId != doc.mRootNode)
                out["parent"] = std::to_string(node.mParent);

            if (node.mKind == PrefabNodeKind::Entity)
            {
                json components = json::array();
                for (const PrefabComponentInstance &component : node.mComponents)
                    components.push_back(ComponentToJson(component));
                out["components"] = std::move(components);
            }
            else
            {
                out["prefab"] = ToString(node.mPrefab);
                json patches = json::array();
                for (const PrefabPatch &patch : node.mPatches)
                    patches.push_back(PatchToJson(patch));
                out["patches"] = std::move(patches);
            }
            nodes.push_back(std::move(out));
        }
        root["nodes"] = std::move(nodes);
        return root;
    }

    PrefabParseResult ReadPrefabFile(const std::filesystem::path &path)
    {
        PrefabParseResult result;
        const std::optional<json> root = ReadJsonFile(path);
        if (!root.has_value())
        {
            result.mDiagnostics.push_back({ PrefabDiagnosticSeverity::Error, path.string(),
                "could not be read as JSON" });
            return result;
        }
        return ParsePrefab(*root, path.string());
    }

    bool WritePrefabFile(const std::filesystem::path &path, const PrefabDocument &doc,
        std::vector<PrefabDiagnostic> *diagnostics)
    {
        std::vector<PrefabDiagnostic> local = ValidatePrefab(doc, path.string());
        if (HasPrefabErrors(local))
        {
            if (diagnostics != nullptr)
            {
                diagnostics->insert(diagnostics->end(),
                    std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
            }
            return false;
        }
        if (WriteJsonFile(path, PrefabToJson(doc)))
            return true;
        if (diagnostics != nullptr)
            diagnostics->push_back({ PrefabDiagnosticSeverity::Error, path.string(), "failed to write the file" });
        return false;
    }
}
