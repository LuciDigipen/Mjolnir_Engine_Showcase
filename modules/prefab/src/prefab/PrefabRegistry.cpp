/**
 * @file PrefabRegistry.cpp
 * @author Jackey Cen
 * @brief Resolves prefabs by PrefabId from memory and disk; AssetId adapter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabRegistry.h"

#include "assets/AssetManifest.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace mir
{
    AssetId PrefabAssetId(PrefabId id)
    {
        if (id.IsNull())
            return kNullAssetId;
        return MakeAssetId("prefab:" + ToString(id));
    }

    std::vector<PrefabDiagnostic> PrefabRegistry::Scan(const std::filesystem::path &prefabsDir)
    {
        std::vector<PrefabDiagnostic> diagnostics;

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        const bool dirExists = std::filesystem::exists(prefabsDir, ec);
        if (dirExists && !ec)
        {
            for (std::filesystem::recursive_directory_iterator it(prefabsDir, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                if (it->is_regular_file(ec) && it->path().extension() == kPrefabExtension)
                    files.push_back(it->path());
            }
        }
        if (ec)
        {
            diagnostics.push_back({ PrefabDiagnosticSeverity::Error, prefabsDir.string(),
                "could not scan the prefabs directory: " + ec.message() });
            return diagnostics;
        }
        std::sort(files.begin(), files.end());

        std::unordered_map<PrefabId, PrefabDocument> disk;
        std::unordered_map<PrefabId, std::filesystem::path> diskPaths;
        std::unordered_map<PrefabId, std::vector<std::filesystem::path>> pathsById;
        std::vector<PrefabId> idOrder;
        for (const std::filesystem::path &file : files)
        {
            PrefabParseResult parsed = ReadPrefabFile(file);
            diagnostics.insert(diagnostics.end(),
                std::make_move_iterator(parsed.mDiagnostics.begin()), std::make_move_iterator(parsed.mDiagnostics.end()));
            if (!parsed.mDocument.has_value())
                continue;

            const PrefabId id = parsed.mDocument->mId;
            if (!pathsById.contains(id))
                idOrder.push_back(id);
            pathsById[id].push_back(file);
            disk.emplace(id, std::move(*parsed.mDocument));
        }

        for (PrefabId id : idOrder)
        {
            const std::vector<std::filesystem::path> &paths = pathsById[id];
            if (paths.size() > 1)
            {
                std::string listing;
                for (const std::filesystem::path &path : paths)
                {
                    if (!listing.empty())
                        listing += ", ";
                    listing += path.generic_string();
                }
                for (const std::filesystem::path &path : paths)
                {
                    diagnostics.push_back({ PrefabDiagnosticSeverity::Error, path.string(),
                        "prefab UUID " + ToString(id) + " is shared by " + listing +
                            "; delete the copy or create copies through the copy API - none of these files resolve" });
                }
                disk.erase(id);
                continue;
            }
            diskPaths.emplace(id, paths.front());
        }

        mDisk = std::move(disk);
        mDiskPaths = std::move(diskPaths);
        return diagnostics;
    }

    bool PrefabRegistry::SetDocument(const PrefabDocument &doc, std::vector<PrefabDiagnostic> *diagnostics)
    {
        std::vector<PrefabDiagnostic> local = ValidatePrefab(doc);
        if (diagnostics != nullptr)
        {
            diagnostics->insert(diagnostics->end(),
                std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
        }
        if (HasPrefabErrors(local))
            return false;
        mMemory[doc.mId] = doc;
        return true;
    }

    bool PrefabRegistry::RemoveDocument(PrefabId id)
    {
        return mMemory.erase(id) > 0;
    }

    const PrefabDocument *PrefabRegistry::Find(PrefabId id) const
    {
        const auto memory = mMemory.find(id);
        if (memory != mMemory.end())
            return &memory->second;
        const auto disk = mDisk.find(id);
        if (disk != mDisk.end())
            return &disk->second;
        return nullptr;
    }

    std::optional<std::filesystem::path> PrefabRegistry::PathOf(PrefabId id) const
    {
        const auto it = mDiskPaths.find(id);
        if (it == mDiskPaths.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<PrefabId> PrefabRegistry::IdAtPath(const std::filesystem::path &path) const
    {
        const std::filesystem::path wanted = path.lexically_normal();
        for (const auto &[id, candidate] : mDiskPaths)
        {
            if (candidate.lexically_normal() == wanted)
                return id;
        }
        return std::nullopt;
    }

    void PrefabRegistry::MarkSaved(const PrefabDocument &doc, const std::filesystem::path &path)
    {
        mDisk[doc.mId] = doc;
        mDiskPaths[doc.mId] = path;
    }

    std::optional<PrefabDocument> PrefabRegistry::CopyAsNew(const PrefabDocument &doc) const
    {
        if (HasPrefabErrors(ValidatePrefab(doc)))
            return std::nullopt;

        PrefabDocument copy = doc;
        do
        {
            copy.mId = GeneratePrefabId();
        } while (mMemory.contains(copy.mId) || mDisk.contains(copy.mId));
        return copy;
    }

    std::optional<AssetId> PrefabRegistry::ResolveAssetId(PrefabId id, const AssetManifest *manifest,
        std::vector<PrefabDiagnostic> *diagnostics) const
    {
        if (Find(id) == nullptr)
            return std::nullopt;

        const std::string uuid = ToString(id);
        const AssetId mine = PrefabAssetId(id);
        const auto fail = [&](std::string message)
            {
                if (diagnostics != nullptr)
                    diagnostics->push_back({ PrefabDiagnosticSeverity::Error, "", std::move(message) });
                return std::optional<AssetId>{};
            };

        if (mine.IsNull())
            return fail("prefab " + uuid + " maps to the null AssetId; regenerate its UUID");

        const auto clash = [id, mine](const std::unordered_map<PrefabId, PrefabDocument> &table) -> const PrefabId *
            {
                for (const auto &[otherId, document] : table)
                {
                    if (otherId != id && PrefabAssetId(otherId) == mine)
                        return &otherId;
                }
                return nullptr;
            };
        if (const PrefabId *other = clash(mMemory))
            return fail("prefab " + uuid + " hashes to the same AssetId as prefab " + ToString(*other) +
                "; regenerate one of the two UUIDs");
        if (const PrefabId *other = clash(mDisk))
            return fail("prefab " + uuid + " hashes to the same AssetId as prefab " + ToString(*other) +
                "; regenerate one of the two UUIDs");

        if (manifest != nullptr)
        {
            if (const AssetManifestEntry *entry = manifest->Find(mine))
                return fail("prefab " + uuid + " hashes to the AssetId of cooked asset \"" +
                    std::string(manifest->PathOf(*entry)) + "\"; regenerate the prefab UUID");
        }
        return mine;
    }
}
