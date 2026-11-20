/**
 * @file TerrainMaterialSlots.h
 * @author Jackey Cen
 * @brief Binds a release's weight layer names to one instance's material layers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // A release layer the material does not name draws with material layer 0.
    class TerrainMaterialSlotBinding final
    {
      public:
        // Recomputes the table only when either layer list changed.
        bool Update(std::span<const std::string> releaseLayers, std::span<const std::string> materialLayers);

        // One entry per release layer and zero past them.
        std::span<const uint8_t> Layers() const
        {
            return mLayers;
        }

        // The release layers the material does not name; empty for a single material.
        const std::vector<std::string> &Unnamed() const
        {
            return mUnnamed;
        }

        // One line for this material and release layer list; empty when every layer is named or it was already
        // reported.
        std::string ReportOnce(std::string_view materialPath);

      private:
        std::vector<std::string> mReleaseLayers;
        std::vector<std::string> mMaterialLayers;
        std::vector<uint8_t> mLayers;
        std::vector<std::string> mUnnamed;
        std::string mReported;
    };
}
