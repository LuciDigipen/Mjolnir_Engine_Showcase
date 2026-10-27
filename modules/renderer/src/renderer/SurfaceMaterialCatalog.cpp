/**
 * @file SurfaceMaterialCatalog.cpp
 * @author Jackey Cen
 * @brief Build-generated surface material registrations and their records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialCatalog.h>

#include <renderer/MaterialReflection.h>

#include <core/fs/Paths.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }
    }

    const SurfaceMaterialRegistration *SurfaceMaterialCatalog::Find(std::string_view stem) const
    {
        for (const SurfaceMaterialRegistration &material : materials)
            if (material.stem == stem)
                return &material;
        return nullptr;
    }

    const SurfaceMaterialRegistration *SurfaceMaterialCatalog::Find(
        std::string_view stem, std::string_view asset) const
    {
        if (asset.empty())
            return Find(stem);
        for (const SurfaceMaterialRegistration &material : materials)
            if (material.stem == stem && material.asset == asset)
                return &material;
        return nullptr;
    }

    const SurfaceMaterialRegistration *SurfaceMaterialCatalog::FindByAsset(std::string_view asset) const
    {
        for (const SurfaceMaterialRegistration &material : materials)
            if (material.asset == asset)
                return &material;
        return nullptr;
    }

    uint32_t SurfaceMaterialCatalog::IndexOf(const SurfaceMaterialRegistration *registration) const
    {
        if (registration == nullptr)
            return UINT32_MAX;
        for (std::size_t i = 0; i < materials.size(); ++i)
            if (&materials[i] == registration)
                return static_cast<uint32_t>(i);
        return UINT32_MAX;
    }

    std::optional<SurfaceMaterialCatalog> LoadSurfaceMaterialCatalog(std::string *error)
    {
        const std::filesystem::path path = ShaderPath("surface_materials.json");
        std::ifstream file(path);
        if (!file)
        {
            SetError(error, "surface material catalog is missing: " + path.string());
            return std::nullopt;
        }

        nlohmann::json root;
        try
        {
            file >> root;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("surface material catalog is not JSON: ") + exception.what());
            return std::nullopt;
        }

        if (!root.is_object() || root.value("version", 0u) != 1 || !root.contains("materials") ||
            !root.at("materials").is_array())
        {
            SetError(error, "surface material catalog has an unsupported shape or version");
            return std::nullopt;
        }

        SurfaceMaterialCatalog catalog;
        for (const nlohmann::json &entry : root.at("materials"))
        {
            if (!entry.is_object())
            {
                SetError(error, "surface material catalog entry is not an object");
                return std::nullopt;
            }
            SurfaceMaterialRegistration material;
            material.stem = entry.value("stem", "");
            material.type = entry.value("type", "");
            material.frontend = entry.value("frontend", "");
            material.asset = entry.value("asset", "");
            material.shader = entry.value("shader", "");
            material.parametersJson = entry.value("parameters", nlohmann::json::object()).dump();
            material.reflectionFile = entry.value("reflection", "");
            if (material.stem.empty() || material.type.empty() || material.frontend != "mesh" ||
                material.asset.empty() || material.shader.empty() || material.reflectionFile.empty())
            {
                SetError(error, "surface material catalog entry is incomplete");
                return std::nullopt;
            }
            catalog.materials.push_back(std::move(material));
        }

        if (catalog.materials.empty())
        {
            SetError(error, "surface material catalog is empty");
            return std::nullopt;
        }
        return catalog;
    }

    std::optional<std::vector<std::byte>> PackSurfaceMaterialRecord(
        const SurfaceMaterialRegistration &registration, std::string *error)
    {
        const std::filesystem::path reflectionPath = ShaderPath(registration.reflectionFile);
        std::ifstream file(reflectionPath);
        if (!file)
        {
            SetError(error, "material reflection is missing: " + reflectionPath.string());
            return std::nullopt;
        }
        const std::string reflection{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        const auto layout = ParseMaterialLayout(reflection, error);
        if (!layout)
            return std::nullopt;
        return PackMaterialRecord(*layout, registration.parametersJson, error);
    }
}
