/**
 * @file AssetResolver.h
 * @author sumin.park
 * @brief Turns the name a script or scene uses for an asset into its AssetId.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetId.h"

#include <string>
#include <string_view>

namespace mir
{
    class AssetManifest;
    class AssetsModule;

    // Finds an asset's id from its path.
    class AssetResolver
    {
    public:
        // idRoot is the game's assets folder
        AssetResolver(const AssetsModule &assets, std::string idRoot);

        // The id of the asset with this path
        AssetId Resolve(std::string_view path, std::string *error = nullptr) const;

        // Like Resolve, against the reload being prepared; only between BeginReload and Commit/AbortReload
        AssetId ResolveCandidate(std::string_view path, std::string *error = nullptr) const;

        // Like Resolve, but for a full path that is not inside the game's folder
        AssetId ResolveFull(std::string_view path) const;

        // The asset's full path, for log messages
        std::string PathOf(AssetId id) const;

    private:
        AssetId ResolveIn(const AssetManifest &manifest, std::string_view path, std::string *error) const;

        const AssetsModule &mAssets;
        std::string mIdRoot;
    };
}
