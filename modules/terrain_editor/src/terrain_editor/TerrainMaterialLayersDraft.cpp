/**
 * @file TerrainMaterialLayersDraft.cpp
 * @author Jackey Cen
 * @brief A terrain asset's material layers as the panel edits them, and the files Apply writes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainMaterialLayersDraft.h>

#include <terrain_editor/TerrainLayerDocument.h>

#include <core/fs/FileIO.h>
#include <renderer/TerrainMaterialAsset.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        const char *BlendText(TerrainLayerBlendMode blend)
        {
            switch (blend)
            {
            case TerrainLayerBlendMode::Weight:
                return "weight";
            case TerrainLayerBlendMode::Alpha:
                return "alpha";
            case TerrainLayerBlendMode::Height:
            default:
                return "height";
            }
        }

        bool HasRules(const TerrainMaterialRules &rules)
        {
            return rules.height.has_value() || rules.slope.has_value() || rules.noise.has_value();
        }

        std::string RecipeStem(const std::filesystem::path &recipePath)
        {
            std::string name = recipePath.filename().string();
            if (name.ends_with(kTerrainRecipeSuffix))
                name.resize(name.size() - kTerrainRecipeSuffix.size());
            else
                name = recipePath.stem().string();
            return name;
        }
    } // namespace

    bool TerrainMaterialLayersDraft::Load(const TerrainMaterialLayersInput &input, std::string &error)
    {
        Clear();
        mInput = input;

        const std::optional<std::string> text = ReadText(input.recipePath);
        if (!text)
        {
            error = std::format("{}: the recipe cannot be read", input.recipePath.generic_string());
            return false;
        }

        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(*text);
        }
        catch (const nlohmann::json::exception &exception)
        {
            error = std::format("{}: the recipe is not valid JSON ({})", input.recipePath.generic_string(),
                exception.what());
            return false;
        }
        if (!root.is_object())
        {
            error = std::format("{}: the recipe must be a JSON object", input.recipePath.generic_string());
            return false;
        }

        const nlohmann::json *materials = nullptr;
        if (const auto found = root.find("materials"); found != root.end())
        {
            materials = &*found;
            mRecipeHasMaterials = true;
        }

        const TerrainRecipeMaterialsParseResult parsed = ParseTerrainRecipeMaterials(materials);
        for (const TerrainRecipeMaterialLayer &layer : parsed.layers)
        {
            mLayers.push_back({ layer.name, layer.rules, {}, layer.name });
            mLoadedNames.push_back(layer.name);
        }

        mBoundLayered = IsTerrainMaterialPath(input.boundMaterial);
        if (mBoundLayered)
        {
            if (const std::optional<std::string> json = ReadText(input.assetsRoot / input.boundMaterial))
            {
                if (const std::optional<TerrainMaterialAsset> material =
                        ReadTerrainMaterial(*json, input.boundMaterial, nullptr))
                {
                    mBlend = material->blend;
                    mHeightContrast = material->heightContrast;
                    if (!mRecipeHasMaterials)
                    {
                        mLayers.clear();
                        mLoadedNames.clear();
                        for (const TerrainMaterialLayerAsset &layer : material->layers)
                        {
                            mLayers.push_back({ layer.name, {}, layer.material, layer.name });
                            mLoadedNames.push_back(layer.name);
                        }
                    }
                    else
                    {
                        for (TerrainMaterialLayerRow &row : mLayers)
                        {
                            const auto found = std::ranges::find(material->layers, row.name,
                                &TerrainMaterialLayerAsset::name);
                            if (found != material->layers.end())
                                row.material = found->material;
                        }
                    }
                }
            }
        }
        else if (!input.boundMaterial.empty())
        {
            for (TerrainMaterialLayerRow &row : mLayers)
                row.material = input.boundMaterial;
        }

        mLoaded = true;
        return true;
    }

    void TerrainMaterialLayersDraft::Clear()
    {
        mInput = {};
        mLayers.clear();
        mLoadedNames.clear();
        mBlend = TerrainLayerBlendMode::Height;
        mHeightContrast = 0.1f;
        mBoundLayered = false;
        mRecipeHasMaterials = false;
        mLoaded = false;
    }

    std::string TerrainMaterialLayersDraft::DefaultLayerName() const
    {
        for (std::size_t number = 1;; ++number)
        {
            const std::string name = number == 1 ? "base" : std::format("layer_{}", number);
            const bool used = std::ranges::any_of(mLayers, [&name](const TerrainMaterialLayerRow &row) {
                return row.name == name || row.originalName == name;
            });
            const bool loaded = std::ranges::find(mLoadedNames, name) != mLoadedNames.end();
            if (!used && !loaded)
                return name;
        }
    }

    bool TerrainMaterialLayersDraft::AddLayer()
    {
        if (!CanAddLayer())
            return false;
        const std::string material = mLayers.empty() ? std::string() : mLayers.front().material;
        mLayers.push_back({ DefaultLayerName(), {}, material, {} });
        return true;
    }

    void TerrainMaterialLayersDraft::RemoveLayer(std::size_t index)
    {
        if (index < mLayers.size())
            mLayers.erase(mLayers.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void TerrainMaterialLayersDraft::MoveLayer(std::size_t index, int offset)
    {
        if (offset == 0 || index >= mLayers.size())
            return;
        const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(index) + offset;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(mLayers.size()))
            return;
        std::swap(mLayers[index], mLayers[static_cast<std::size_t>(target)]);
    }

    std::vector<TerrainRecipeMaterialLayer> TerrainMaterialLayersDraft::RecipeLayers() const
    {
        std::vector<TerrainRecipeMaterialLayer> layers;
        layers.reserve(mLayers.size());
        for (const TerrainMaterialLayerRow &row : mLayers)
            layers.push_back({ row.name, row.rules });
        return layers;
    }

    std::string TerrainMaterialLayersDraft::Validate() const
    {
        if (!mLoaded)
            return "The recipe has not loaded.";
        if (mLayers.empty() || mLayers.size() > kMaxTerrainLayers)
            return std::format("A terrain needs between 1 and {} material layers.", kMaxTerrainLayers);

        std::set<std::string> names;
        for (std::size_t index = 0; index < mLayers.size(); ++index)
        {
            const TerrainMaterialLayerRow &row = mLayers[index];
            if (!IsValidTerrainMaterialName(row.name))
                return std::format("Layer {} name '{}' is not valid; use lowercase letters, digits or underscores.",
                    index + 1, row.name);
            if (!names.insert(row.name).second)
                return std::format("Two layers share the name '{}'.", row.name);
        }

        if (HasRules(mLayers.front().rules))
            return "The first layer is the fallback and takes no rules.";

        for (const TerrainMaterialLayerRow &row : mLayers)
        {
            if (row.material.empty())
                return std::format("Layer '{}' needs a surface material.", row.name);
            std::error_code ignored;
            if (!std::filesystem::is_regular_file(mInput.assetsRoot / row.material, ignored))
                return std::format("Surface material '{}' does not exist under the assets root.", row.material);
        }
        return {};
    }

    TerrainMaterialLayersPlan TerrainMaterialLayersDraft::MakePlan() const
    {
        TerrainMaterialLayersPlan plan;
        std::set<std::string> live;
        for (const TerrainMaterialLayerRow &row : mLayers)
        {
            if (row.originalName.empty())
            {
                plan.additions.push_back(row.name);
                continue;
            }
            live.insert(row.originalName);
            if (row.originalName != row.name)
                plan.renames.emplace_back(row.originalName, row.name);
        }
        for (const std::string &name : mLoadedNames)
        {
            if (!live.contains(name))
                plan.deletions.push_back(name);
        }
        return plan;
    }

    std::string TerrainMaterialLayersDraft::LayeredMaterialPath() const
    {
        if (mBoundLayered)
            return mInput.boundMaterial;
        return "materials/" + RecipeStem(mInput.recipePath) + ".terrainmaterial.json";
    }

    TerrainMaterialLayersWriteResult WriteTerrainMaterialLayersFiles(const TerrainMaterialLayersDraft &draft)
    {
        TerrainMaterialLayersWriteResult result;
        if (const std::string invalid = draft.Validate(); !invalid.empty())
        {
            result.error = invalid;
            return result;
        }

        const std::optional<std::string> text = ReadText(draft.RecipePath());
        if (!text)
        {
            result.error = std::format("{}: the recipe cannot be read", draft.RecipePath().generic_string());
            return result;
        }

        const std::vector<TerrainRecipeMaterialLayer> layers = draft.RecipeLayers();
        const TerrainRecipeMaterialsWriteResult replaced = ReplaceTerrainRecipeMaterials(*text, layers);
        if (!replaced.text)
        {
            result.error = replaced.error;
            return result;
        }
        if (!WriteTextAtomic(draft.RecipePath(), *replaced.text))
        {
            result.error = std::format("{}: the recipe cannot be written", draft.RecipePath().generic_string());
            return result;
        }

        if (draft.Layers().size() < 2)
            return result;

        const std::string relative = draft.LayeredMaterialPath();
        nlohmann::ordered_json root;
        root["version"] = 1;
        root["blend"] = BlendText(draft.Blend());
        root["heightContrast"] = draft.HeightContrast();
        nlohmann::ordered_json materialLayers = nlohmann::ordered_json::array();
        for (const TerrainMaterialLayerRow &row : draft.Layers())
            materialLayers.push_back({ { "name", row.name }, { "material", row.material } });
        root["layers"] = std::move(materialLayers);

        const std::filesystem::path path = draft.AssetsRoot() / relative;
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        if (!WriteTextAtomic(path, root.dump(2) + "\n"))
        {
            result.error = std::format("{}: the layered material cannot be written", path.generic_string());
            return result;
        }

        result.layeredMaterial = relative;
        return result;
    }

    TerrainMaterialChannelMigration MigrateTerrainMaterialChannels(TerrainLayerDocument &document,
                                                                  const TerrainMaterialLayersPlan &plan)
    {
        TerrainMaterialChannelMigration migration;
        std::map<std::string, std::string> renames(plan.renames.begin(), plan.renames.end());
        const std::set<std::string> deletions(plan.deletions.begin(), plan.deletions.end());

        std::vector<uint32_t> sequences;
        sequences.reserve(document.Layers().size());
        for (const TerrainEditLayer &layer : document.Layers())
            sequences.push_back(layer.info.sequence);

        bool changed = false;
        for (const uint32_t sequence : sequences)
        {
            TerrainEditLayer *layer = document.MutableLayer(sequence);
            if (layer == nullptr)
                continue;

            for (auto tile = layer->tiles.begin(); tile != layer->tiles.end();)
            {
                TerrainEditTile &edit = tile->second;
                const auto removed = std::ranges::remove_if(edit.weights, [&deletions](const TerrainEditWeight &weight) {
                    return deletions.contains(weight.layer);
                });
                const bool deleted = removed.begin() != removed.end();
                if (deleted)
                {
                    migration.deleted += static_cast<std::size_t>(removed.end() - removed.begin());
                    edit.weights.erase(removed.begin(), removed.end());
                }

                bool renamed = false;
                for (TerrainEditWeight &weight : edit.weights)
                {
                    if (const auto found = renames.find(weight.layer); found != renames.end())
                    {
                        weight.layer = found->second;
                        ++migration.renamed;
                        renamed = true;
                    }
                }
                if (renamed)
                {
                    std::ranges::sort(edit.weights, {}, &TerrainEditWeight::layer);
                }

                if (edit.values.empty() && edit.weights.empty())
                {
                    tile = layer->tiles.erase(tile);
                    ++migration.droppedTiles;
                    changed = true;
                    continue;
                }
                changed = changed || renamed || deleted;
                ++tile;
            }
        }

        if (changed)
            document.MarkChanged();
        document.BumpMaterialGeneration();
        return migration;
    }
} // namespace mir
