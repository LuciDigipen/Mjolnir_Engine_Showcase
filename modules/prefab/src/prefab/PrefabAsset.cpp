/**
 * @file PrefabAsset.cpp
 * @author Jackey Cen
 * @brief Cooked prefab payload: canonical document JSON plus its nested UUIDs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        void AddError(std::vector<PrefabDiagnostic> &diagnostics, std::string message)
        {
            diagnostics.push_back({ PrefabDiagnosticSeverity::Error, {}, std::move(message) });
        }
    }

    std::vector<PrefabId> CollectPrefabDependencies(const PrefabDocument &doc)
    {
        std::vector<PrefabId> dependencies;
        for (const PrefabNode &node : doc.mNodes)
        {
            if (node.mKind != PrefabNodeKind::Prefab)
                continue;
            if (std::find(dependencies.begin(), dependencies.end(), node.mPrefab) == dependencies.end())
                dependencies.push_back(node.mPrefab);
        }
        return dependencies;
    }

    std::vector<std::byte> BuildPrefabAsset(const PrefabDocument &doc)
    {
        const std::vector<PrefabDiagnostic> diagnostics = ValidatePrefab(doc);
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                throw std::runtime_error("cannot cook an invalid prefab: " + diagnostic.mMessage);
        }

        nlohmann::json payload;
        payload["document"] = PrefabToJson(doc);
        nlohmann::json dependencies = nlohmann::json::array();
        for (const PrefabId dependency : CollectPrefabDependencies(doc))
            dependencies.push_back(ToString(dependency));
        payload["dependencies"] = std::move(dependencies);

        const std::string text = payload.dump();
        return BuildAssetBlob(kPrefabAssetTypeTag, kPrefabAssetContentVersion,
            std::as_bytes(std::span(text)));
    }

    PrefabAssetParseResult ReadPrefabAsset(const AssetBlobView &blob)
    {
        PrefabAssetParseResult result;
        if (blob.header.typeTag != kPrefabAssetTypeTag ||
            blob.header.contentVersion != kPrefabAssetContentVersion)
        {
            AddError(result.mDiagnostics, "not a cooked prefab asset of this version");
            return result;
        }

        const nlohmann::json payload = nlohmann::json::parse(AsStringView(blob), nullptr, false);
        if (payload.is_discarded() || !payload.is_object())
        {
            AddError(result.mDiagnostics, "cooked prefab content is not a JSON object");
            return result;
        }

        const auto document = payload.find("document");
        if (document == payload.end() || !document->is_object())
        {
            AddError(result.mDiagnostics, "cooked prefab has no \"document\" object");
            return result;
        }

        const auto dependencies = payload.find("dependencies");
        if (dependencies == payload.end() || !dependencies->is_array())
        {
            AddError(result.mDiagnostics, "cooked prefab has no \"dependencies\" array");
            return result;
        }

        PrefabAsset asset;
        for (const nlohmann::json &entry : *dependencies)
        {
            if (!entry.is_string())
            {
                AddError(result.mDiagnostics, "cooked prefab dependency must be a UUID string");
                return result;
            }
            const std::optional<PrefabId> dependency = ParsePrefabId(entry.get_ref<const std::string &>());
            if (!dependency.has_value())
            {
                AddError(result.mDiagnostics,
                    "cooked prefab dependency \"" + entry.get_ref<const std::string &>() + "\" is not a valid UUID");
                return result;
            }
            asset.mDependencies.push_back(*dependency);
        }

        PrefabParseResult parsed = ParsePrefab(*document);
        result.mDiagnostics.insert(result.mDiagnostics.end(),
            std::make_move_iterator(parsed.mDiagnostics.begin()),
            std::make_move_iterator(parsed.mDiagnostics.end()));
        if (!parsed.mDocument.has_value())
            return result;

        if (CollectPrefabDependencies(*parsed.mDocument) != asset.mDependencies)
        {
            AddError(result.mDiagnostics, "cooked prefab dependency list does not match its document");
            return result;
        }

        asset.mDocument = std::move(*parsed.mDocument);
        result.mAsset = std::move(asset);
        return result;
    }
}
