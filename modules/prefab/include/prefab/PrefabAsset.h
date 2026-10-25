/**
 * @file PrefabAsset.h
 * @author Jackey Cen
 * @brief Cooked prefab payload: canonical document JSON plus its nested UUIDs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabDocument.h"
#include "prefab/PrefabFormat.h"

#include "assets/AssetBlob.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kPrefabAssetTypeTag = 0x46455250u;
    inline constexpr uint32_t kPrefabAssetContentVersion = 1u;

    struct PrefabAsset
    {
        PrefabDocument mDocument;
        std::vector<PrefabId> mDependencies;
    };

    struct PrefabAssetParseResult
    {
        std::optional<PrefabAsset> mAsset;
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    // Unique nested PrefabIds in node order.
    std::vector<PrefabId> CollectPrefabDependencies(const PrefabDocument &doc);

    std::vector<std::byte> BuildPrefabAsset(const PrefabDocument &doc);

    PrefabAssetParseResult ReadPrefabAsset(const AssetBlobView &blob);
}
