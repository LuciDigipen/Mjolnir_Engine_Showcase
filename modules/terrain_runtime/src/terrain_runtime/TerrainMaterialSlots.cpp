/**
 * @file TerrainMaterialSlots.cpp
 * @author Jackey Cen
 * @brief Binds a release's weight layer names to one instance's material layers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainMaterialSlots.h>

#include <renderer/TerrainDrawData.h>
#include <terrain/TerrainWeights.h>

#include <algorithm>

namespace mir
{
    namespace
    {
        bool SameLayers(const std::vector<std::string> &cached, std::span<const std::string> layers)
        {
            return cached.size() == layers.size() && std::equal(cached.begin(), cached.end(), layers.begin());
        }
    }

    bool TerrainMaterialSlotBinding::Update(std::span<const std::string> releaseLayers,
                                            std::span<const std::string> materialLayers)
    {
        if (SameLayers(mReleaseLayers, releaseLayers) && SameLayers(mMaterialLayers, materialLayers))
            return false;

        mReleaseLayers.assign(releaseLayers.begin(), releaseLayers.end());
        mMaterialLayers.assign(materialLayers.begin(), materialLayers.end());
        mLayers.assign(size_t(kMaxTerrainMaterialLayers), 0);
        mUnnamed.clear();
        const bool layered = !mMaterialLayers.empty();
        for (size_t i = 0; i < releaseLayers.size() && i < mLayers.size(); ++i)
        {
            const auto named = std::find(mMaterialLayers.begin(), mMaterialLayers.end(), releaseLayers[i]);
            if (named == mMaterialLayers.end())
            {
                if (layered)
                    mUnnamed.push_back(releaseLayers[i]);
                continue;
            }
            mLayers[i] = std::min<uint8_t>(static_cast<uint8_t>(named - mMaterialLayers.begin()),
                                           kMaxTerrainDrawMaterialLayer);
        }
        return true;
    }

    std::string TerrainMaterialSlotBinding::ReportOnce(std::string_view materialPath)
    {
        if (mUnnamed.empty() || mMaterialLayers.empty())
            return {};

        std::string key(materialPath);
        for (const std::string &name : mReleaseLayers)
        {
            key += '\n';
            key += name;
        }
        if (key == mReported)
            return {};
        mReported = std::move(key);

        std::string unnamed;
        for (const std::string &name : mUnnamed)
        {
            if (!unnamed.empty())
                unnamed += ", ";
            unnamed += '\'' + name + '\'';
        }
        return "Terrain instance material '" + std::string(materialPath) + "': release layers " + unnamed +
               " have no material layer; drawn with layer '" + mMaterialLayers.front() + "'";
    }
}
