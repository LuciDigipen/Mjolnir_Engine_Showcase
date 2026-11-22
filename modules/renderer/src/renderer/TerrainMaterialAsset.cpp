/**
 * @file TerrainMaterialAsset.cpp
 * @author Jackey Cen
 * @brief A layered terrain material's source JSON and its resolved layer materials.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/TerrainMaterialAsset.h>

#include <assets/AssetBlob.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        constexpr size_t kMaxLayerNameLength = 32;

        // Must accept exactly the names a terrain release can carry (IsValidTerrainMaterialLayerName).
        bool ValidLayerName(std::string_view name)
        {
            if (name.empty() || name.size() > kMaxLayerNameLength)
                return false;
            for (const char character : name)
            {
                const bool valid = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                    character == '_';
                if (!valid)
                    return false;
            }
            return true;
        }

        bool SafeRelativePath(std::string_view path)
        {
            if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos)
                return false;
            if (path.size() >= 2 && path[1] == ':')
                return false;
            std::size_t start = 0;
            while (start <= path.size())
            {
                const std::size_t end = path.find('/', start);
                const std::string_view part = path.substr(start, end == std::string_view::npos ? path.size() - start
                                                                                               : end - start);
                if (part.empty() || part == "." || part == "..")
                    return false;
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
            return true;
        }

        std::string AssetsRootOf(std::string_view ownerAsset)
        {
            const std::size_t found = ownerAsset.rfind("/materials/");
            return found == std::string_view::npos ? std::string() : std::string(ownerAsset.substr(0, found));
        }

        bool EndsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }

        const SurfaceMaterialRegistration *FindTerrain(const SurfaceMaterialCatalog &catalog, std::string_view asset)
        {
            return catalog.FindByAsset(asset, "terrain");
        }

        std::optional<std::string> ResolveLayerMaterial(const SurfaceMaterialCatalog &catalog, std::string_view root,
            std::string_view reference, std::string &error)
        {
            std::string rooted(reference);
            if (!root.empty())
                rooted = std::string(root) + "/" + rooted;

            if (FindTerrain(catalog, rooted) != nullptr)
                return rooted;
            if (rooted != reference && FindTerrain(catalog, reference) != nullptr)
                return std::string(reference);

            bool otherFrontend = catalog.FindByAsset(rooted) != nullptr || catalog.FindByAsset(reference) != nullptr;

            std::string match;
            std::size_t matches = 0;
            for (const SurfaceMaterialRegistration &registration : catalog.materials)
            {
                const bool suffix = registration.asset == reference ||
                    (registration.asset.size() > reference.size() &&
                        registration.asset.compare(registration.asset.size() - reference.size(), reference.size(),
                            reference) == 0 &&
                        registration.asset[registration.asset.size() - reference.size() - 1] == '/');
                if (!suffix)
                    continue;
                if (registration.frontend == "terrain")
                {
                    match = registration.asset;
                    ++matches;
                }
                else
                    otherFrontend = true;
            }

            if (matches == 1)
                return match;
            if (matches > 1)
            {
                error = "material '" + std::string(reference) + "' matches more than one cooked surface material";
                return std::nullopt;
            }
            if (otherFrontend)
            {
                error = "material '" + std::string(reference) + "' does not declare the terrain frontend";
                return std::nullopt;
            }
            error = "material '" + std::string(reference) + "' is not a cooked surface material";
            return std::nullopt;
        }

        bool DecodeTerrainMaterial(const AssetBlobView &root, const AssetDependencies &, TerrainMaterialSource &out,
            std::string &error)
        {
            if (root.header.typeTag != kRawAssetTypeTag || root.header.contentVersion != kRawAssetContentVersion)
            {
                error = "the layered material asset is not a raw JSON source";
                return false;
            }
            out.json.assign(reinterpret_cast<const char *>(root.content.data()), root.content.size());
            return true;
        }

        const AssetDecoder<TerrainMaterialSource> kTerrainMaterialDecoder{ DecodeTerrainMaterial,
            std::span<const uint32_t>{} };
    }

    const AssetDecoder<TerrainMaterialSource> &TerrainMaterialDecoder()
    {
        return kTerrainMaterialDecoder;
    }

    bool IsTerrainMaterialPath(std::string_view path)
    {
        return EndsWith(path, ".terrainmaterial.json");
    }

    uint32_t TerrainLayerBlendModeValue(TerrainLayerBlendMode mode)
    {
        switch (mode)
        {
        case TerrainLayerBlendMode::Height:
            return 1;
        case TerrainLayerBlendMode::Alpha:
            return 2;
        case TerrainLayerBlendMode::Weight:
        default:
            return 0;
        }
    }

    std::optional<TerrainMaterialAsset> ReadTerrainMaterial(std::string_view json, std::string_view ownerAsset,
        const SurfaceMaterialCatalog *catalog, std::string *error)
    {
        std::string localError;
        std::string &reported = error != nullptr ? *error : localError;

        nlohmann::json document;
        try
        {
            document = nlohmann::json::parse(json);
        }
        catch (const nlohmann::json::exception &exception)
        {
            SetError(&reported, std::string("the layered material is not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!document.is_object())
        {
            SetError(&reported, "the layered material must be a JSON object");
            return std::nullopt;
        }
        if (!document.contains("version") || !document.at("version").is_number_integer() ||
            document.at("version").get<int64_t>() != 1)
        {
            SetError(&reported, "the layered material version must be 1");
            return std::nullopt;
        }

        TerrainMaterialAsset asset;
        if (document.contains("blend"))
        {
            const nlohmann::json &blend = document.at("blend");
            if (!blend.is_string())
            {
                SetError(&reported, "the layered material blend must be a string");
                return std::nullopt;
            }
            const std::string value = blend.get<std::string>();
            if (value == "weight")
                asset.blend = TerrainLayerBlendMode::Weight;
            else if (value == "height")
                asset.blend = TerrainLayerBlendMode::Height;
            else if (value == "alpha")
                asset.blend = TerrainLayerBlendMode::Alpha;
            else
            {
                SetError(&reported, "unknown layered material blend '" + value + "'");
                return std::nullopt;
            }
        }
        if (document.contains("heightContrast"))
        {
            const nlohmann::json &contrast = document.at("heightContrast");
            if (!contrast.is_number())
            {
                SetError(&reported, "the layered material heightContrast must be a number");
                return std::nullopt;
            }
            const double value = contrast.get<double>();
            if (!std::isfinite(value) || !(value > 0.0))
            {
                SetError(&reported, "the layered material heightContrast must be positive");
                return std::nullopt;
            }
            asset.heightContrast = static_cast<float>(value);
        }

        if (!document.contains("layers") || !document.at("layers").is_array())
        {
            SetError(&reported, "the layered material layers must be an array");
            return std::nullopt;
        }
        const nlohmann::json &layers = document.at("layers");
        if (layers.size() < 2 || layers.size() > kMaxTerrainLayers)
        {
            SetError(&reported, "the layered material needs between two and four layers");
            return std::nullopt;
        }

        const std::string root = AssetsRootOf(ownerAsset);
        for (const nlohmann::json &entry : layers)
        {
            if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string() ||
                !entry.contains("material") || !entry.at("material").is_string())
            {
                SetError(&reported, "every layered material layer needs a name and a material");
                return std::nullopt;
            }
            TerrainMaterialLayerAsset layer;
            layer.name = entry.at("name").get<std::string>();
            if (!ValidLayerName(layer.name))
            {
                SetError(&reported, "layer name '" + layer.name +
                                        "' must use lowercase letters, digits or underscores, at most " +
                                        std::to_string(kMaxLayerNameLength) + " characters");
                return std::nullopt;
            }
            if (std::any_of(asset.layers.begin(), asset.layers.end(),
                    [&](const TerrainMaterialLayerAsset &existing) { return existing.name == layer.name; }))
            {
                SetError(&reported, "duplicate layer name '" + layer.name + "'");
                return std::nullopt;
            }

            const std::string reference = entry.at("material").get<std::string>();
            if (IsTerrainMaterialPath(reference))
            {
                SetError(&reported, "layer '" + layer.name + "' names a layered material; a layer must be a single "
                                    "surface material");
                return std::nullopt;
            }
            if (!EndsWith(reference, ".material.json"))
            {
                SetError(&reported, "layer '" + layer.name + "' material '" + reference +
                                        "' must name a .material.json");
                return std::nullopt;
            }
            if (!SafeRelativePath(reference))
            {
                SetError(&reported, "layer '" + layer.name + "' material '" + reference +
                                        "' must stay under the assets root");
                return std::nullopt;
            }

            if (catalog != nullptr)
            {
                std::string resolveError;
                const std::optional<std::string> resolved = ResolveLayerMaterial(*catalog, root, reference, resolveError);
                if (!resolved)
                {
                    SetError(&reported, "layer '" + layer.name + "' " + resolveError);
                    return std::nullopt;
                }
                layer.material = *resolved;
            }
            else
                layer.material = reference;
            layer.asset = MakeAssetId(layer.material);
            asset.layers.push_back(std::move(layer));
        }
        return asset;
    }
}
