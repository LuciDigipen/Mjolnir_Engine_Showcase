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
    class AssetCache;
    class AssetManifest;

    // "prefab:<uuid>" can never collide with a path-derived id string: ':' is
    // an invalid character in logical asset paths.
    AssetId PrefabAssetId(PrefabId id);

    class PrefabRegistry
    {
    public:
        // Rebuilds the whole disk table from prefabsDir (recursive); malformed
        // files contribute diagnostics only, and a UUID shared by several files
        // resolves from none of them. changed receives the ids whose disk
        // document was added, removed or rewritten.
        std::vector<PrefabDiagnostic> Scan(const std::filesystem::path &prefabsDir,
            std::vector<PrefabId> *changed = nullptr);

        // Rebuilds the disk table from the manifest's cooked Prefab entries,
        // which carry no source path. Bad blobs contribute diagnostics only.
        std::vector<PrefabDiagnostic> LoadCooked(const AssetManifest &manifest, AssetCache &cache);

        // Unsaved in-memory documents, resolved before disk. An invalid
        // document is rejected and the registry is left unchanged.
        bool SetDocument(const PrefabDocument &doc, std::vector<PrefabDiagnostic> *diagnostics = nullptr);

        // The same, with the file the document is meant to be saved to.
        bool SetDocument(const PrefabDocument &doc, const std::filesystem::path &intendedPath,
            std::vector<PrefabDiagnostic> *diagnostics = nullptr);

        // Where a memory-only document is meant to be saved; empty clears it.
        bool SetIntendedPath(PrefabId id, std::optional<std::filesystem::path> path);

        bool RemoveDocument(PrefabId id);

        const PrefabDocument *Find(PrefabId id) const;

        // The file on disk, or the intended path of an unsaved document.
        std::optional<std::filesystem::path> PathOf(PrefabId id) const;
        std::optional<std::filesystem::path> IntendedPathOf(PrefabId id) const;

        // True once a file holds this document.
        bool HasSavedFile(PrefabId id) const { return mDiskPaths.contains(id); }

        // The prefab whose file or intended path is @p path, if any.
        std::optional<PrefabId> IdAtPath(const std::filesystem::path &path) const;

        // Call only after the file at @p path holds @p doc.
        void MarkSaved(const PrefabDocument &doc, const std::filesystem::path &path);

        // Every id in either table, memory first.
        std::vector<PrefabId> Ids() const;
        bool HasMemoryDocument(PrefabId id) const { return mMemory.contains(id); }

        // The disk version only, even when an in-memory document hides it.
        const PrefabDocument *FindOnDisk(PrefabId id) const;

        // Same nodes and high-water mark under a fresh unused PrefabId.
        std::optional<PrefabDocument> CopyAsNew(const PrefabDocument &doc) const;

        // Empty when the AssetId slot is taken: another registered prefab
        // hashes to it, the manifest owns it, or it is the null id.
        std::optional<AssetId> ResolveAssetId(PrefabId id, const AssetManifest *manifest = nullptr,
            std::vector<PrefabDiagnostic> *diagnostics = nullptr) const;

    private:
        std::unordered_map<PrefabId, PrefabDocument> mMemory;
        std::unordered_map<PrefabId, std::filesystem::path> mIntendedPaths;
        std::unordered_map<PrefabId, PrefabDocument> mDisk;
        std::unordered_map<PrefabId, std::filesystem::path> mDiskPaths;
    };
}
