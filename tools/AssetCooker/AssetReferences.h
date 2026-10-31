/**
 * @file AssetReferences.h
 * @author sumin.park
 * @brief Lists the assets a cooked blob points at.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>

#include <vector>

namespace mir::cook
{
    // takes one cooked blob and returns the list the AssetIds of other assets that the blob points at
    std::vector<AssetId> CollectAssetReferences(const AssetBlobView &blob);
}
