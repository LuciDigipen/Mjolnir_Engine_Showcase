/**
 * @file AssetResolver.cpp
 * @author sumin.park
 * @brief Turns the name a script or scene uses for an asset into its AssetId.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "assets/AssetResolver.h"

#include "assets/AssetManifest.h"
#include "assets/AssetPath.h"
#include "assets/AssetsModule.h"

#include <format>
#include <utility>

namespace mir
{
    AssetResolver::AssetResolver(const AssetsModule &assets, std::string idRoot)
        : mAssets(assets), mIdRoot(std::move(idRoot))
    {}

    AssetId AssetResolver::Resolve(std::string_view path, std::string *error) const
    {
        return ResolveIn(mAssets.Manifest(), path, error);
    }

    AssetId AssetResolver::ResolveCandidate(std::string_view path, std::string *error) const
    {
        return ResolveIn(mAssets.CandidateManifest(), path, error);
    }

    AssetId AssetResolver::ResolveIn(const AssetManifest &manifest, std::string_view path, std::string *error) const
    {
        const AssetPathResult logical = MakeLogicalAssetPath(mIdRoot, path);
        if (!logical.Ok())
        {
            if (error != nullptr)
                *error = std::string(Describe(logical.error));
            return kNullAssetId;
        }

        const AssetManifestEntry *entry = manifest.FindByPath(logical.path);
        const AssetId id = entry != nullptr ? entry->id : kNullAssetId;
        if (id.IsNull() && error != nullptr)
            *error = "no asset has the path '" + logical.path + "'";
        return id;
    }

    AssetId AssetResolver::ResolveFull(std::string_view path) const
    {
        const AssetManifestEntry *entry = mAssets.Manifest().FindByPath(path);
        return entry != nullptr ? entry->id : kNullAssetId;
    }

    std::string AssetResolver::PathOf(AssetId id) const
    {
        const AssetManifest &manifest = mAssets.Manifest();
        if (const AssetManifestEntry *entry = manifest.Find(id))
            if (const std::string_view logical = manifest.LogicalPathOf(*entry); !logical.empty())
                return std::string(logical);
        return std::format("{:016x}", id.value);
    }
}
