/**
 * @file TerrainMaterialLayersDraft.h
 * @author Jackey Cen
 * @brief A terrain asset's material layers as the panel edits them, and the files Apply writes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/TerrainLayerBlend.h>
#include <terrain/TerrainRecipeMaterials.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    class TerrainLayerDocument;

    struct TerrainMaterialLayerRow
    {
        std::string name;
        TerrainMaterialRules rules;
        // Relative to the assets root.
        std::string material;
        // The name the row loaded with; empty for a row the draft added.
        std::string originalName;

        bool operator==(const TerrainMaterialLayerRow &) const = default;
    };

    struct TerrainMaterialLayersInput
    {
        std::filesystem::path assetsRoot;
        std::filesystem::path recipePath;
        // The target entity's materialPath, relative to the assets root; empty when it has none.
        std::string boundMaterial;
    };

    // What Apply does to the painted weight channels, found by row identity rather than by name.
    struct TerrainMaterialLayersPlan
    {
        std::vector<std::pair<std::string, std::string>> renames; // old, new
        std::vector<std::string> deletions;
        std::vector<std::string> additions;

        bool Empty() const { return renames.empty() && deletions.empty() && additions.empty(); }
    };

    // UI free: the panel edits this, Apply writes it out.
    class TerrainMaterialLayersDraft
    {
    public:
        // False with error set when the recipe cannot be read or is not a JSON object.
        bool Load(const TerrainMaterialLayersInput &input, std::string &error);
        void Clear();

        bool Loaded() const { return mLoaded; }
        const std::filesystem::path &RecipePath() const { return mInput.recipePath; }
        const std::filesystem::path &AssetsRoot() const { return mInput.assetsRoot; }
        bool RecipeHasMaterials() const { return mRecipeHasMaterials; }
        bool HasBoundLayeredMaterial() const { return mBoundLayered; }

        const std::vector<TerrainMaterialLayerRow> &Layers() const { return mLayers; }
        TerrainMaterialLayerRow &Row(std::size_t index) { return mLayers[index]; }
        TerrainMaterialRules &MutableRules(std::size_t index) { return mLayers[index].rules; }

        bool CanAddLayer() const { return mLayers.size() < kMaxTerrainLayers; }
        // False at kMaxTerrainLayers.
        bool AddLayer();
        void RemoveLayer(std::size_t index);
        // One step up or down; out of range does nothing.
        void MoveLayer(std::size_t index, int offset);
        std::string DefaultLayerName() const;

        // Empty when the draft can be applied.
        std::string Validate() const;
        TerrainMaterialLayersPlan MakePlan() const;
        std::vector<TerrainRecipeMaterialLayer> RecipeLayers() const;

        TerrainLayerBlendMode Blend() const { return mBlend; }
        float HeightContrast() const { return mHeightContrast; }
        // The file the entity already uses when it is layered, else the recipe's own file.
        std::string LayeredMaterialPath() const;

    private:
        TerrainMaterialLayersInput mInput;
        std::vector<TerrainMaterialLayerRow> mLayers;
        // The recipe's layer names at load, so a rename is not read as a delete.
        std::vector<std::string> mLoadedNames;
        TerrainLayerBlendMode mBlend = TerrainLayerBlendMode::Height;
        float mHeightContrast = 0.1f;
        bool mBoundLayered = false;
        bool mRecipeHasMaterials = false;
        bool mLoaded = false;
    };

    struct TerrainMaterialLayersWriteResult
    {
        // Relative to the assets root; empty when the draft holds a single layer.
        std::string layeredMaterial;
        std::string error;
    };

    // Publishes the recipe's materials and its optional layered material together.
    TerrainMaterialLayersWriteResult WriteTerrainMaterialLayersFiles(const TerrainMaterialLayersDraft &draft);

    struct TerrainMaterialChannelMigration
    {
        std::size_t renamed = 0;
        std::size_t deleted = 0;
        std::size_t droppedTiles = 0;
    };

    // Renames or deletes painted channels on every Edit Layer; brush steps from before refuse to undo.
    TerrainMaterialChannelMigration MigrateTerrainMaterialChannels(TerrainLayerDocument &document,
                                                                  const TerrainMaterialLayersPlan &plan);
} // namespace mir
