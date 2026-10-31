/**
 * @file MaterialRecipe.cpp
 * @author Jackey Cen
 * @brief Source .material.json reading and field-level partial saves.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/MaterialRecipe.h>

#include <assets/AssetPath.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <system_error>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        std::string DescribeSaveFailure(const std::filesystem::path &path)
        {
            std::error_code existsError;
            if (!std::filesystem::exists(path, existsError))
                return "the source material file no longer exists: " + path.string();

            std::string readError;
            const std::optional<Json> root = ReadJsonFile(path, &readError);
            if (!root || !root->is_object())
                return "the source material file is not a readable JSON object: " + path.string() +
                    (readError.empty() ? std::string() : " (" + readError + ")");
            return "the source material file could not be written: " + path.string();
        }
    } // namespace

    std::optional<MaterialRecipe> LoadMaterialRecipe(const std::filesystem::path &path, std::string *error)
    {
        std::string readError;
        const std::optional<Json> document = ReadJsonFile(path, &readError);
        if (!document)
        {
            SetError(error, readError == "file does not exist" ? "the material source file does not exist: " +
                                                                      path.string()
                                                                : "the material source file could not be read: " +
                                                                      path.string() + " (" + readError + ")");
            return std::nullopt;
        }
        if (!document->is_object())
        {
            SetError(error, "the material source root must be an object: " + path.string());
            return std::nullopt;
        }
        if (document->value("version", 0) != 1)
        {
            SetError(error, "the material source needs version 1: " + path.string());
            return std::nullopt;
        }

        const auto shader = document->find("shader");
        if (shader == document->end() || !shader->is_object() || !shader->contains("file") ||
            !shader->at("file").is_string() || !shader->contains("type") || !shader->at("type").is_string())
        {
            SetError(error, "the material source needs shader.file and shader.type: " + path.string());
            return std::nullopt;
        }

        const auto frontends = document->find("frontends");
        if (frontends == document->end() || !frontends->is_array() || frontends->empty())
        {
            SetError(error, "the material source needs a non-empty frontends array: " + path.string());
            return std::nullopt;
        }

        MaterialRecipe recipe;
        recipe.path = path;
        recipe.shaderFile = shader->at("file").get<std::string>();
        recipe.shaderType = shader->at("type").get<std::string>();
        for (const Json &frontend : *frontends)
        {
            if (!frontend.is_string())
            {
                SetError(error, "the material frontends must be strings: " + path.string());
                return std::nullopt;
            }
            recipe.frontends.push_back(frontend.get<std::string>());
        }

        const auto parameters = document->find("parameters");
        if (parameters != document->end())
        {
            if (!parameters->is_object())
            {
                SetError(error, "the material parameters must be an object: " + path.string());
                return std::nullopt;
            }
            recipe.parametersJson = parameters->dump();
        }

        const auto textures = document->find("textures");
        if (textures != document->end())
        {
            if (!textures->is_object())
            {
                SetError(error, "the material textures must be an object: " + path.string());
                return std::nullopt;
            }
            for (const auto &[role, texture] : textures->items())
            {
                if (!texture.is_object() || !texture.contains("path") || !texture.at("path").is_string() ||
                    !texture.contains("colorSpace") || !texture.at("colorSpace").is_string())
                {
                    SetError(error, "material texture '" + role + "' needs path and colorSpace: " + path.string());
                    return std::nullopt;
                }
                recipe.textures.push_back(
                    {role, texture.at("path").get<std::string>(), texture.at("colorSpace").get<std::string>()});
            }
            std::sort(recipe.textures.begin(), recipe.textures.end(),
                [](const MaterialRecipeTexture &left, const MaterialRecipeTexture &right)
                { return left.role < right.role; });
        }

        const auto resolution = document->find("resolution");
        if (resolution != document->end())
        {
            if (!resolution->is_number_integer() || resolution->get<int64_t>() <= 0 ||
                (resolution->is_number_unsigned() && resolution->get<uint64_t>() > UINT32_MAX) ||
                (!resolution->is_number_unsigned() && resolution->get<int64_t>() > UINT32_MAX))
            {
                SetError(error, "the material resolution must be a positive integer: " + path.string());
                return std::nullopt;
            }
            recipe.resolution = resolution->get<uint32_t>();
        }
        return recipe;
    }

    bool SaveMaterialRecipe(const std::filesystem::path &path, std::string_view parametersPatchJson,
        const std::map<std::string, std::string> &texturePaths, std::string *error)
    {
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError))
        {
            SetError(error, "the source material file no longer exists: " + path.string());
            return false;
        }

        std::string readError;
        const std::optional<Json> existing = ReadJsonFile(path, &readError);
        if (!existing || !existing->is_object())
        {
            SetError(error, DescribeSaveFailure(path));
            return false;
        }

        Json patch;
        try
        {
            patch = Json::parse(parametersPatchJson);
        }
        catch (const Json::exception &exception)
        {
            SetError(error, std::string("the material save values are not JSON: ") + exception.what());
            return false;
        }
        if (!patch.is_object())
        {
            SetError(error, "the material save values must be an object");
            return false;
        }

        const auto textures = existing->find("textures");
        for (const auto &[role, replacement] : texturePaths)
        {
            if (textures == existing->end() || !textures->is_object() || !textures->contains(role) ||
                !textures->at(role).is_object() || !textures->at(role).contains("path"))
            {
                SetError(error, "texture '" + role + "' is not authored in " + path.string());
                return false;
            }
            const AssetPathResult normalized = NormalizeAssetPath(replacement);
            if (!normalized.Ok())
            {
                SetError(error, "texture '" + role + "' path '" + replacement + "' is not a relative asset path: " +
                        std::string(Describe(normalized.error)));
                return false;
            }
        }

        const bool written = UpdateJsonFile(path, [&](Json &root)
            {
                if (!root.contains("parameters") || !root["parameters"].is_object())
                    root["parameters"] = Json::object();
                for (const auto &[field, value] : patch.items())
                    root["parameters"][field] = value;
                for (const auto &[role, replacement] : texturePaths)
                    root["textures"][role]["path"] = replacement;
            });
        if (!written)
        {
            SetError(error, DescribeSaveFailure(path));
            return false;
        }
        return true;
    }

    std::optional<std::filesystem::path> MaterialSourceFile(const std::filesystem::path &sourceAssetRoot,
        std::string_view assetIdRoot, std::string_view logicalPath)
    {
        const AssetPathResult root = NormalizeAssetPath(assetIdRoot);
        const AssetPathResult logical = NormalizeAssetPath(logicalPath);
        if (!root.Ok() || !logical.Ok())
            return std::nullopt;

        const std::string prefix = root.path + "/";
        if (logical.path.size() <= prefix.size() || logical.path.compare(0, prefix.size(), prefix) != 0)
            return std::nullopt;
        return sourceAssetRoot / std::filesystem::path(logical.path.substr(prefix.size()));
    }
}
