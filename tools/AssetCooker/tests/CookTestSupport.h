/**
 * @file CookTestSupport.h
 * @author sumin.park
 * @brief Helpers shared by the cooker tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetManifest.h>

#include <string_view>

namespace mir::test
{
    // The id of the asset with this logical path in a cooked manifest, or null
    inline AssetId PathId(const AssetManifest &manifest, std::string_view logicalPath)
    {
        const AssetManifestEntry *entry = manifest.FindByPath(logicalPath);
        return entry != nullptr ? entry->id : kNullAssetId;
    }
}
