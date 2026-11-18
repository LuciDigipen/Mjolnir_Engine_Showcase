/**
 * @file TerrainLayerWorkspace.cpp
 * @author Jackey Cen
 * @brief The scene's terrain assets and one Edit Layer document for each asset being edited.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainLayerWorkspace.h>

#include <terrain_editor/TerrainLayerDocument.h>

#include <terrain/TerrainEditStore.h>
#include <terrain/TerrainScene.h>

#include <algorithm>
#include <utility>

namespace mir
{
    TerrainLayerWorkspace::TerrainLayerWorkspace(std::filesystem::path assetsDir) : mAssetsDir(std::move(assetsDir))
    {
    }

    TerrainLayerWorkspace::~TerrainLayerWorkspace() = default;

    void TerrainLayerWorkspace::Sync(const TerrainWorld &world)
    {
        std::string previous;
        if (mSelectedAsset < mAssets.size())
            previous = mAssets[mSelectedAsset].recipePath;

        std::vector<TerrainEditAsset> next;
        for (const TerrainInstanceState &instance : world.Instances())
        {
            if (instance.recipePath.empty() || TerrainEditLayersDir(mAssetsDir / std::filesystem::u8path(instance.recipePath)).empty())
                continue;
            if (std::ranges::any_of(next, [&instance](const TerrainEditAsset &asset) {
                    return asset.recipePath == instance.recipePath;
                }))
                continue;

            std::string logicalPath = instance.recipePath;
            for (const TerrainWorld::AssetState &asset : world.Assets())
            {
                if (asset.asset == instance.placement.asset && !asset.logicalPath.empty())
                {
                    logicalPath = asset.logicalPath;
                    break;
                }
            }
            next.push_back({ instance.recipePath, std::move(logicalPath), instance.placement.asset });
        }

        std::ranges::sort(next, {}, &TerrainEditAsset::recipePath);
        mAssets = std::move(next);

        const auto found = std::ranges::find(mAssets, previous, &TerrainEditAsset::recipePath);
        mSelectedAsset = found == mAssets.end() ? 0 : static_cast<std::size_t>(found - mAssets.begin());
    }

    void TerrainLayerWorkspace::Clear()
    {
        mAssets.clear();
        mSelectedAsset = 0;
        mSelectedLayer.reset();
    }

    void TerrainLayerWorkspace::SelectAsset(std::size_t index)
    {
        if (index >= mAssets.size() || index == mSelectedAsset)
            return;
        mSelectedAsset = index;
        mSelectedLayer.reset();
    }

    TerrainLayerDocument *TerrainLayerWorkspace::SelectedDocument()
    {
        if (mSelectedAsset >= mAssets.size())
            return nullptr;

        const std::string &recipePath = mAssets[mSelectedAsset].recipePath;
        const auto found = mDocuments.find(recipePath);
        if (found != mDocuments.end())
            return found->second.get();

        auto document = std::make_unique<TerrainLayerDocument>(TerrainEditLayersDir(mAssetsDir / std::filesystem::u8path(recipePath)));
        document->Load();
        TerrainLayerDocument *loaded = document.get();
        mDocuments.emplace(recipePath, std::move(document));
        return loaded;
    }
} // namespace mir
