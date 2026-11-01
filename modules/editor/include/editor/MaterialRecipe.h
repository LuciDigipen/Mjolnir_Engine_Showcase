/**
 * @file MaterialRecipe.h
 * @author Jackey Cen
 * @brief Source .material.json reading and field-level partial saves.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct MaterialRecipeTexture
    {
        std::string role;
        std::string path;
        std::string colorSpace;
    };

    // The authored recipe as the material editor reads it. Root keys the schema
    // does not know stay in the file; only reflected fields are written back.
    struct MaterialRecipe
    {
        std::filesystem::path path;
        std::string shaderFile;
        std::string shaderType;
        std::vector<std::string> frontends;
        std::string parametersJson = "{}";
        std::vector<MaterialRecipeTexture> textures;
        uint32_t resolution = 2048;
    };

    std::optional<MaterialRecipe> LoadMaterialRecipe(const std::filesystem::path &path, std::string *error = nullptr);

    // parametersPatchJson is an object of reflected field names to values and
    // texturePaths maps authored texture roles to replacement source paths. A
    // missing file, an unreadable root, an unknown texture role or a failed
    // write returns false and leaves the file alone.
    bool SaveMaterialRecipe(const std::filesystem::path &path, std::string_view parametersPatchJson,
        const std::map<std::string, std::string> &texturePaths, std::string *error = nullptr);

    // The source file of a logical asset path under the project's asset root.
    std::optional<std::filesystem::path> MaterialSourceFile(const std::filesystem::path &sourceAssetRoot,
        std::string_view assetIdRoot, std::string_view logicalPath);
}
