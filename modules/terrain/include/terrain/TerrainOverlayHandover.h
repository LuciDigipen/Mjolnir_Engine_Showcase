/**
 * @file TerrainOverlayHandover.h
 * @author Jackey Cen
 * @brief Overlay snapshots offered before the releases they sit on are loaded.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainScene.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mir
{
    // Applied once their asset has a loaded release; applied snapshots leave, refused ones are reported.
    class TerrainOverlayHandover
    {
      public:
        // A newer snapshot of an asset replaces the one still waiting.
        void Offer(AssetId asset, std::string logicalPath, std::shared_ptr<const TerrainOverlay> overlay);

        bool Empty() const { return mWaiting.empty(); }

        // Returns how many were applied; the rest are waiting or reported as OverlayDropped.
        size_t Apply(TerrainWorld &world, std::vector<TerrainIssue> &issues);

      private:
        struct Waiting
        {
            AssetId asset;
            std::string logicalPath;
            std::shared_ptr<const TerrainOverlay> overlay;
        };

        std::vector<Waiting> mWaiting;
    };
} // namespace mir
