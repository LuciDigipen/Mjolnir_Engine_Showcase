/**
 * @file TerrainOverlayHandover.cpp
 * @author Jackey Cen
 * @brief Overlay snapshots offered before the releases they sit on are loaded.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainOverlayHandover.h>

#include <algorithm>
#include <format>
#include <utility>

namespace mir
{
    void TerrainOverlayHandover::Offer(AssetId asset, std::string logicalPath,
                                       std::shared_ptr<const TerrainOverlay> overlay)
    {
        if (!overlay)
            return;
        const auto found = std::find_if(mWaiting.begin(), mWaiting.end(),
                                        [asset](const Waiting &waiting) { return waiting.asset == asset; });
        if (found != mWaiting.end())
        {
            found->logicalPath = std::move(logicalPath);
            found->overlay = std::move(overlay);
            return;
        }
        mWaiting.push_back({asset, std::move(logicalPath), std::move(overlay)});
    }

    size_t TerrainOverlayHandover::Apply(TerrainWorld &world, std::vector<TerrainIssue> &issues)
    {
        size_t applied = 0;
        for (auto it = mWaiting.begin(); it != mWaiting.end();)
        {
            const auto state = std::find_if(world.Assets().begin(), world.Assets().end(),
                                            [&it](const TerrainWorld::AssetState &asset)
                                            { return asset.asset == it->asset; });
            if (state == world.Assets().end() || !state->release)
            {
                ++it;
                continue;
            }
            const TerrainOverlayApplyResult result = world.SetOverlay(it->asset, it->overlay);
            if (result.applied)
                ++applied;
            else
                issues.push_back({TerrainIssueKind::OverlayDropped, it->logicalPath,
                                  std::format("Terrain '{}' (asset {:016x}): overlay snapshot dropped by the "
                                              "release now loaded: {}",
                                              it->logicalPath, it->asset.value, result.reason)});
            it = mWaiting.erase(it);
        }
        return applied;
    }
} // namespace mir
