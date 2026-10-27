/**
 * @file MaterialReflection.cpp
 * @author Jackey Cen
 * @brief Slang material reflection, record packing and output naming.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialReflection.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        uint32_t ExpectedSize(MaterialFieldKind kind)
        {
            switch (kind)
            {
            case MaterialFieldKind::Float:
            case MaterialFieldKind::Uint:
            case MaterialFieldKind::Int:
            case MaterialFieldKind::Bool:
                return 4;
            case MaterialFieldKind::Float2:
                return 8;
            case MaterialFieldKind::Float3:
                return 12;
            case MaterialFieldKind::Float4:
                return 16;
            case MaterialFieldKind::TextureRef:
                return 0;
            }
            return 0;
        }

        std::optional<MaterialFieldKind> ScalarKind(const nlohmann::json &type)
        {
            if (!type.is_object())
                return std::nullopt;
            if (type.value("kind", "") == "scalar")
            {
                const std::string scalar = type.value("scalarType", "");
                if (scalar == "float32")
                    return MaterialFieldKind::Float;
                if (scalar == "uint32")
                    return MaterialFieldKind::Uint;
                if (scalar == "int32")
                    return MaterialFieldKind::Int;
                if (scalar == "bool")
                    return MaterialFieldKind::Bool;
            }

            if (type.value("kind", "") == "vector")
            {
                if (!type.contains("elementType") || !type.at("elementType").is_object())
                    return std::nullopt;
                const std::string scalar = type.at("elementType").value("scalarType", "");
                if (scalar == "float32")
                {
                    switch (type.value("elementCount", 0))
                    {
                    case 2:
                        return MaterialFieldKind::Float2;
                    case 3:
                        return MaterialFieldKind::Float3;
                    case 4:
                        return MaterialFieldKind::Float4;
                    default:
                        return std::nullopt;
                    }
                }
            }

            return std::nullopt;
        }

        std::optional<MaterialFieldLayout> ReadField(const nlohmann::json &field, std::string *error)
        {
            if (!field.is_object() || !field.contains("name") || !field.at("name").is_string() ||
                !field.contains("binding") || !field.at("binding").is_object() || !field.contains("type"))
            {
                SetError(error, "material field must have a name, binding and type");
                return std::nullopt;
            }

            MaterialFieldLayout layout;
            layout.name = field.at("name").get<std::string>();
            if (layout.name.empty())
            {
                SetError(error, "material field has no name");
                return std::nullopt;
            }

            const nlohmann::json &binding = field.at("binding");
            if (!binding.contains("offset") || !binding.at("offset").is_number_unsigned() ||
                !binding.contains("size") || !binding.at("size").is_number_unsigned())
            {
                SetError(error, "material field '" + layout.name + "' has an invalid binding");
                return std::nullopt;
            }
            layout.offset = binding.at("offset").get<uint32_t>();
            layout.size = binding.at("size").get<uint32_t>();
            if (layout.offset % 4 != 0 || layout.size == 0 || layout.offset > UINT32_MAX - layout.size)
            {
                SetError(error, "material field '" + layout.name + "' has an invalid offset or size");
                return std::nullopt;
            }

            const nlohmann::json &type = field.at("type");
            if (const auto scalar = ScalarKind(type))
            {
                layout.kind = *scalar;
                if (layout.size != ExpectedSize(layout.kind))
                {
                    SetError(error, "material field '" + layout.name + "' has the wrong size");
                    return std::nullopt;
                }
                return layout;
            }

            if (type.value("kind", "") == "struct" && type.value("name", "") == "TextureRef" &&
                type.contains("fields") && type.at("fields").is_array())
            {
                layout.kind = MaterialFieldKind::TextureRef;
                uint32_t imageOffset = UINT32_MAX;
                uint32_t samplerOffset = UINT32_MAX;
                for (const nlohmann::json &inner : type.at("fields"))
                {
                    if (!inner.is_object() || !inner.contains("name") || !inner.contains("binding") ||
                        !inner.at("binding").is_object() || !inner.contains("type"))
                    {
                        SetError(error, "material texture field '" + layout.name + "' has an invalid member");
                        return std::nullopt;
                    }
                    if (!inner.at("name").is_string())
                    {
                        SetError(error, "material texture '" + layout.name + "' has an invalid member");
                        return std::nullopt;
                    }
                    const std::string innerName = inner.at("name").get<std::string>();
                    const nlohmann::json &innerBinding = inner.at("binding");
                    const nlohmann::json &innerType = inner.at("type");
                    if ((innerName != "image" && innerName != "sampler") || !innerType.is_object() ||
                        innerType.value("kind", "") != "scalar" ||
                        innerType.value("scalarType", "") != "uint32" ||
                        !innerBinding.contains("offset") || !innerBinding.at("offset").is_number_unsigned() ||
                        !innerBinding.contains("size") || !innerBinding.at("size").is_number_unsigned() ||
                        innerBinding.at("size").get<uint32_t>() != 4)
                    {
                        SetError(error, "material texture '" + layout.name + "' has an unsupported member");
                        return std::nullopt;
                    }
                    const uint32_t offset = innerBinding.at("offset").get<uint32_t>();
                    if (innerName == "image")
                        imageOffset = offset;
                    else
                        samplerOffset = offset;
                }
                if (imageOffset != 0 || (samplerOffset != UINT32_MAX && samplerOffset != 4))
                {
                    SetError(error, "material texture '" + layout.name + "' has an invalid layout");
                    return std::nullopt;
                }
                const uint32_t innerSize = samplerOffset == 4 ? 8u : 4u;
                if (layout.size != innerSize)
                {
                    SetError(error, "material texture '" + layout.name + "' has the wrong size");
                    return std::nullopt;
                }
                return layout;
            }

            SetError(error, "unsupported material field type for '" + layout.name + "'");
            return std::nullopt;
        }
    }

    std::optional<MaterialLayout> ParseMaterialLayout(std::string_view reflectionJson, std::string *error)
    {
        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(reflectionJson);
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("material reflection is not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!root.is_object())
        {
            SetError(error, "material reflection must be a JSON object");
            return std::nullopt;
        }

        try
        {
        std::optional<nlohmann::json> resultType;
        try
        {
            for (const nlohmann::json &entry : root.value("entryPoints", nlohmann::json::array()))
            {
                if (!entry.is_object() || !entry.contains("parameters"))
                    continue;
                for (const nlohmann::json &parameter : entry.at("parameters"))
                {
                    if (!parameter.is_object() || !parameter.contains("type"))
                        continue;
                    const nlohmann::json &type = parameter.at("type");
                    if (type.is_object() && type.value("baseShape", "") == "structuredBuffer" &&
                        type.contains("resultType"))
                        resultType = type.at("resultType");
                }
            }
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("material reflection witness scan failed: ") + exception.what());
            return std::nullopt;
        }

        if (!resultType)
        {
            SetError(error, "material reflection has no typed StructuredBuffer witness");
            return std::nullopt;
        }

        if (!resultType->is_object() || !resultType->contains("fields") || !resultType->at("fields").is_array())
        {
            SetError(error, "material reflection witness has no fields array");
            return std::nullopt;
        }

        MaterialLayout layout;
        uint32_t end = 0;
        for (const nlohmann::json &field : resultType->at("fields"))
        {
            std::optional<MaterialFieldLayout> parsed;
            try
            {
                parsed = ReadField(field, error);
            }
            catch (const std::exception &exception)
            {
                SetError(error, std::string("material field layout failed: ") + exception.what());
                return std::nullopt;
            }
            if (!parsed)
                return std::nullopt;
            if (parsed->offset + parsed->size > (1u << 20))
            {
                SetError(error, "material field '" + parsed->name + "' is out of range");
                return std::nullopt;
            }
            for (const MaterialFieldLayout &existing : layout.fields)
            {
                if (existing.name == parsed->name)
                {
                    SetError(error, "duplicate material field '" + parsed->name + "'");
                    return std::nullopt;
                }
                const bool disjoint = parsed->offset + parsed->size <= existing.offset ||
                    existing.offset + existing.size <= parsed->offset;
                if (!disjoint)
                {
                    SetError(error, "material fields '" + existing.name + "' and '" + parsed->name +
                                        "' overlap");
                    return std::nullopt;
                }
            }
            end = std::max(end, parsed->offset + parsed->size);
            layout.fields.push_back(*parsed);
        }

        if (layout.fields.empty())
        {
            SetError(error, "material reflection has no fields");
            return std::nullopt;
        }
        if (end > (1u << 20) - 16u)
        {
            SetError(error, "material record size is out of range");
            return std::nullopt;
        }

        layout.size = (end + 15u) / 16u * 16u;
        if (layout.size == 0 || layout.size > (1u << 20))
        {
            SetError(error, "material record size is out of range");
            return std::nullopt;
        }
        return layout;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("material reflection layout failed: ") + exception.what());
            return std::nullopt;
        }
    }

    std::optional<std::vector<std::byte>> PackMaterialRecord(const MaterialLayout &layout,
        std::string_view parametersJson, std::string *error)
    {
        nlohmann::json parameters;
        try
        {
            parameters = nlohmann::json::parse(parametersJson);
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("material parameters are not JSON: ") + exception.what());
            return std::nullopt;
        }

        if (!parameters.is_object())
        {
            SetError(error, "material parameters must be an object keyed by reflected field name");
            return std::nullopt;
        }

        if (layout.size == 0 || layout.size > (1u << 20))
        {
            SetError(error, "material record size is out of range");
            return std::nullopt;
        }

        try
        {
        std::vector<std::byte> bytes(layout.size);
        std::vector<std::string> fieldNames;
        for (const MaterialFieldLayout &field : layout.fields)
        {
            const uint32_t expected = ExpectedSize(field.kind);
            if (field.offset > layout.size || field.size > layout.size - field.offset ||
                (field.kind != MaterialFieldKind::TextureRef && field.size != expected) ||
                (field.kind == MaterialFieldKind::TextureRef && field.size != 4 && field.size != 8))
            {
                SetError(error, "material field '" + field.name + "' does not fit the record");
                return std::nullopt;
            }
            fieldNames.push_back(field.name);
            if (!parameters.contains(field.name))
            {
                SetError(error, "material parameter '" + field.name + "' is missing");
                return std::nullopt;
            }

            const nlohmann::json &value = parameters.at(field.name);
            std::byte *const destination = bytes.data() + field.offset;
            const auto writeFloat = [&](const nlohmann::json &number, std::byte *out, const char *name)
            {
                if (!number.is_number())
                {
                    SetError(error, std::string("material parameter '") + name + "' must be a number");
                    return false;
                }
                const double wide = number.get<double>();
                if (!std::isfinite(wide) || std::abs(wide) > std::numeric_limits<float>::max())
                {
                    SetError(error, std::string("material parameter '") + name + "' is not a finite float");
                    return false;
                }
                const float packed = static_cast<float>(wide);
                std::memcpy(out, &packed, sizeof(packed));
                return true;
            };

            switch (field.kind)
            {
            case MaterialFieldKind::Float:
                if (!writeFloat(value, destination, field.name.c_str()))
                    return std::nullopt;
                break;
            case MaterialFieldKind::Float2:
            case MaterialFieldKind::Float3:
            case MaterialFieldKind::Float4:
            {
                const uint32_t count = static_cast<uint32_t>(field.kind == MaterialFieldKind::Float2 ? 2
                    : field.kind == MaterialFieldKind::Float3 ? 3
                                                              : 4);
                if (!value.is_array() || value.size() != count)
                {
                    SetError(error, "material parameter '" + field.name + "' must have " + std::to_string(count) +
                                    " components");
                    return std::nullopt;
                }
                for (uint32_t i = 0; i < count; ++i)
                {
                    if (!writeFloat(value[i], destination + i * sizeof(float), field.name.c_str()))
                        return std::nullopt;
                }
                break;
            }
            case MaterialFieldKind::Uint:
            {
                if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
                {
                    SetError(error, "material parameter '" + field.name + "' must be an unsigned 32-bit integer");
                    return std::nullopt;
                }
                const uint32_t packed = value.get<uint32_t>();
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::Int:
            {
                if (!value.is_number_integer())
                {
                    SetError(error, "material parameter '" + field.name + "' must be an integer");
                    return std::nullopt;
                }
                const int64_t wide = value.get<int64_t>();
                if (wide < INT32_MIN || wide > INT32_MAX)
                {
                    SetError(error, "material parameter '" + field.name + "' is out of int32 range");
                    return std::nullopt;
                }
                const int32_t packed = static_cast<int32_t>(wide);
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::Bool:
            {
                if (!value.is_boolean())
                {
                    SetError(error, "material parameter '" + field.name + "' must be a boolean");
                    return std::nullopt;
                }
                const uint32_t packed = value.get<bool>() ? 1u : 0u;
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::TextureRef:
            {
                if (!value.is_object() || !value.contains("image") || !value.at("image").is_number_unsigned() ||
                    value.at("image").get<uint64_t>() > UINT32_MAX)
                {
                    SetError(error, "material texture '" + field.name + "' must be {image, sampler}");
                    return std::nullopt;
                }
                const uint32_t image = value.at("image").get<uint32_t>();
                std::memcpy(destination, &image, sizeof(image));
                if (field.size == 8)
                {
                    if (value.contains("sampler") &&
                        (!value.at("sampler").is_number_unsigned() ||
                            value.at("sampler").get<uint64_t>() > UINT32_MAX))
                    {
                        SetError(error, "material texture '" + field.name + "' has an invalid sampler");
                        return std::nullopt;
                    }
                    const uint32_t sampler = value.value("sampler", 0u);
                    std::memcpy(destination + sizeof(uint32_t), &sampler, sizeof(sampler));
                }
                else if (value.contains("sampler"))
                {
                    SetError(error, "material texture '" + field.name + "' has no sampler slot");
                    return std::nullopt;
                }
                break;
            }
            }
        }

        for (const auto &item : parameters.items())
        {
            if (std::find(fieldNames.begin(), fieldNames.end(), item.key()) == fieldNames.end())
            {
                SetError(error, "unknown material parameter '" + item.key() + "'");
                return std::nullopt;
            }
        }
        return bytes;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("material parameter packing failed: ") + exception.what());
            return std::nullopt;
        }
    }

    std::string MaterialShaderStem(std::string_view nameSpace, std::string_view type,
        std::string_view shaderPath, std::string_view frontend)
    {
        std::string normalized(shaderPath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        for (char &character : normalized)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        while (normalized.rfind("./", 0) == 0)
            normalized.erase(0, 2);

        const std::string key = normalized + "|" + std::string(type) + "|" + std::string(frontend);
        uint32_t hash = 2166136261u;
        for (const unsigned char byte : key)
        {
            hash ^= byte;
            hash *= 16777619u;
        }

        std::string stem;
        if (!nameSpace.empty())
        {
            stem.assign(nameSpace);
            stem.push_back('_');
        }
        stem.append(type);
        for (char &character : stem)
        {
            const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_';
            if (!allowed)
                character = '_';
        }
        stem.push_back('_');
        stem.append(std::to_string(hash));
        return stem;
    }

    std::string MaterialShaderOutput(std::string_view stem, std::string_view entry)
    {
        return std::string(stem) + "." + std::string(entry) + ".spv";
    }
}
