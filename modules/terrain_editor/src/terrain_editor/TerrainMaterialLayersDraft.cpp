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
#include <core/fs/Paths.h>
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

        namespace fs = std::filesystem;

        std::string RecipeLogicalPath(const TerrainMaterialLayersDraft &draft)
        {
            return GenericPathUtf8(draft.RecipePath().lexically_normal().lexically_relative(
                draft.AssetsRoot().lexically_normal()));
        }

        bool IsAssetRelative(const fs::path &path)
        {
            const fs::path normalized = path.lexically_normal();
            return !normalized.empty() && !normalized.has_root_path() && *normalized.begin() != "..";
        }

        // The sibling directory reserves the destination for this Apply and keeps renames on its filesystem.
        struct StagedFile
        {
            explicit StagedFile(const fs::path &path) : target(path), directory(path) { directory += ".applying"; }
            StagedFile(const StagedFile &) = delete;
            StagedFile &operator=(const StagedFile &) = delete;

            ~StagedFile()
            {
                if (reserved)
                {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                }
            }

            bool Reserve(std::string &error)
            {
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                if (!ec)
                    reserved = fs::create_directory(directory, ec);
                if (!reserved)
                    error = std::format("Cannot stage {} in {} ({}).", GenericPathUtf8(target),
                        GenericPathUtf8(directory), ec ? ec.message() : "another or interrupted Apply");
                return reserved;
            }

            bool Stage(std::string_view text, std::string &error) const
            {
                if (WriteText(directory / "next", text))
                    return true;
                error = std::format("Cannot stage {}.", GenericPathUtf8(target));
                return false;
            }

            bool Publish(std::string &error) const
            {
                std::error_code ec;
                fs::rename(directory / "next", target, ec);
                if (ec)
                    error = std::format("Cannot replace {} ({}).", GenericPathUtf8(target), ec.message());
                return !ec;
            }

            fs::path target;
            fs::path directory;
            bool reserved = false;
        };
    } // namespace

    bool TerrainMaterialLayersDraft::Load(const TerrainMaterialLayersInput &input, std::string &error)
    {
        Clear();
        mInput = input;

        const std::optional<std::string> text = ReadText(input.recipePath);
        if (!text)
        {
            error = std::format("{}: the recipe cannot be read", GenericPathUtf8(input.recipePath));
            return false;
        }

        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(*text);
        }
        catch (const nlohmann::json::exception &exception)
        {
            error = std::format("{}: the recipe is not valid JSON ({})", GenericPathUtf8(input.recipePath),
                exception.what());
            return false;
        }
        if (!root.is_object())
        {
            error = std::format("{}: the recipe must be a JSON object", GenericPathUtf8(input.recipePath));
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
            if (const std::optional<std::string> json = ReadText(input.assetsRoot / fs::u8path(input.boundMaterial)))
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
        const std::string recipe = RecipeLogicalPath(*this);
        if (!IsAssetRelative(fs::u8path(recipe)) || !recipe.ends_with(kTerrainRecipeSuffix))
            return "The terrain recipe must be under the assets root.";
        if (mBoundLayered && !IsAssetRelative(fs::u8path(mInput.boundMaterial)))
            return "The layered material must be under the assets root.";
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
            const fs::path material = fs::u8path(row.material);
            if (!IsAssetRelative(material) || !fs::is_regular_file(mInput.assetsRoot / material, ignored))
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
        std::string recipe = RecipeLogicalPath(*this);
        if (recipe.ends_with(kTerrainRecipeSuffix))
            recipe.resize(recipe.size() - kTerrainRecipeSuffix.size());
        return "materials/" + recipe + ".terrainmaterial.json";
    }

    TerrainMaterialLayersWriteResult WriteTerrainMaterialLayersFiles(const TerrainMaterialLayersDraft &draft)
    {
        TerrainMaterialLayersWriteResult result;
        if (const std::string invalid = draft.Validate(); !invalid.empty())
        {
            result.error = invalid;
            return result;
        }

        StagedFile recipe(draft.RecipePath());
        if (!recipe.Reserve(result.error))
            return result;
        const std::optional<std::string> text = ReadText(recipe.target);
        if (!text)
        {
            result.error = std::format("{}: the recipe cannot be read", GenericPathUtf8(recipe.target));
            return result;
        }

        const std::vector<TerrainRecipeMaterialLayer> layers = draft.RecipeLayers();
        const TerrainRecipeMaterialsWriteResult replaced = ReplaceTerrainRecipeMaterials(*text, layers);
        if (!replaced.text)
        {
            result.error = replaced.error;
            return result;
        }
        if (!recipe.Stage(*replaced.text, result.error))
            return result;
        if (draft.Layers().size() < 2)
        {
            recipe.Publish(result.error);
            return result;
        }

        const std::string relative = draft.LayeredMaterialPath();
        StagedFile material(draft.AssetsRoot() / fs::u8path(relative));
        if (!material.Reserve(result.error))
            return result;
        const std::string owner = RecipeLogicalPath(draft);
        bool generated = !draft.HasBoundLayeredMaterial();
        std::error_code ec;
        const bool exists = fs::exists(material.target, ec);
        if (ec || (exists && !fs::is_regular_file(material.target, ec)))
        {
            result.error = std::format("{}: cannot inspect the layered material.", relative);
            return result;
        }
        if (exists)
        {
            const auto content = ReadText(material.target);
            const auto previous = content ? nlohmann::json::parse(*content, nullptr, false) : nlohmann::json{};
            if (!previous.is_object())
            {
                result.error = std::format("{}: cannot read the existing material's ownership.", relative);
                return result;
            }
            const auto source = previous.find("sourceRecipe");
            if (source != previous.end())
            {
                if (*source != owner)
                {
                    result.error = std::format("{}: this material belongs to a different terrain recipe.", relative);
                    return result;
                }
                generated = true;
            }
            else if (generated)
            {
                result.error = std::format("{}: an existing material already uses this generated path.", relative);
                return result;
            }
        }
        nlohmann::ordered_json root;
        root["version"] = 1;
        if (generated)
            root["sourceRecipe"] = owner;
        root["blend"] = BlendText(draft.Blend());
        root["heightContrast"] = draft.HeightContrast();
        nlohmann::ordered_json materialLayers = nlohmann::ordered_json::array();
        for (const TerrainMaterialLayerRow &row : draft.Layers())
            materialLayers.push_back({ { "name", row.name }, { "material", row.material } });
        root["layers"] = std::move(materialLayers);

        if (!material.Stage(root.dump(2) + "\n", result.error))
            return result;
        const fs::path backup = recipe.directory / "previous";
        if (!fs::copy_file(recipe.target, backup, ec))
        {
            result.error = std::format("Cannot back up {} ({}).", GenericPathUtf8(recipe.target), ec.message());
            return result;
        }
        if (!recipe.Publish(result.error))
            return result;
        if (!material.Publish(result.error))
        {
            fs::rename(backup, recipe.target, ec);
            if (ec)
            {
                recipe.reserved = false;
                result.error += std::format(" Recipe restore failed ({}). Its original is at {}.",
                    ec.message(), GenericPathUtf8(backup));
            }
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
