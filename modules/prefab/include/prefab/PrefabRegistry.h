/**
 * @file PrefabRegistry.h
 * @author Jackey Cen
 * @brief Resolves prefabs by PrefabId from memory and disk; AssetId adapter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabDocument.h"
#include "prefab/PrefabFormat.h"

#include "assets/AssetId.h"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetManifest;

    // "prefab:<uuid>" can never collide with a path-derived id string: ':' is
    // an invalid character in logical asset paths.
    AssetId PrefabAssetId(PrefabId id);

    class PrefabRegistry
    {
    public:
        // Rebuilds the whole disk table from prefabsDir (recursive); malformed
        // files contribute diagnostics only, and a UUID shared by several files
        // resolves from none of them.
        std::vector<PrefabDiagnostic> Scan(const std::filesystem::path &prefabsDir);

        // Unsaved in-memory documents, resolved before disk. An invalid
        // document is rejected and the registry is left unchanged.
        bool SetDocument(const PrefabDocument &doc, std::vector<PrefabDiagnostic> *diagnostics = nullptr);
        bool RemoveDocument(PrefabId id);

        const PrefabDocument *Find(PrefabId id) const;
        std::optional<std::filesystem::path> PathOf(PrefabId id) const;

        // The scanned prefab whose file is @p path, if any.
        std::optional<PrefabId> IdAtPath(const std::filesystem::path &path) const;

        // Call only after the file at @p path holds @p doc.
        void MarkSaved(const PrefabDocument &doc, const std::filesystem::path &path);

        // Same nodes and high-water mark under a fresh unused PrefabId.
        std::optional<PrefabDocument> CopyAsNew(const PrefabDocument &doc) const;

        // Empty when the AssetId slot is taken: another registered prefab
        // hashes to it, the manifest owns it, or it is the null id.
        std::optional<AssetId> ResolveAssetId(PrefabId id, const AssetManifest *manifest = nullptr,
            std::vector<PrefabDiagnostic> *diagnostics = nullptr) const;

    private:
        std::unordered_map<PrefabId, PrefabDocument> mMemory;
        std::unordered_map<PrefabId, PrefabDocument> mDisk;
        std::unordered_map<PrefabId, std::filesystem::path> mDiskPaths;
    };
}
