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

        std::optional<MaterialFieldKind> ScalarKind(const nlohmann::json &type)
        {
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
                const std::string scalar = type.value("elementType", nlohmann::json::object()).value("scalarType", "");
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
            MaterialFieldLayout layout;
            layout.name = field.value("name", "");
            if (layout.name.empty())
            {
                SetError(error, "material field has no name");
                return std::nullopt;
            }

            const nlohmann::json &binding = field.at("binding");
            layout.offset = binding.value("offset", 0u);
            layout.size = binding.value("size", 0u);

            const nlohmann::json &type = field.at("type");
            if (const auto scalar = ScalarKind(type))
            {
                layout.kind = *scalar;
                return layout;
            }

            if (type.value("kind", "") == "struct" && type.value("name", "") == "TextureRef")
            {
                layout.kind = MaterialFieldKind::TextureRef;
                uint32_t innerSize = 0;
                for (const nlohmann::json &inner : type.at("fields"))
                {
                    const nlohmann::json &innerBinding = inner.at("binding");
                    innerSize = std::max(innerSize,
                        innerBinding.value("offset", 0u) + innerBinding.value("size", 0u));
                }
                layout.size = innerSize;
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
                SetError(error, std::string("material field layout failed at '") +
                                    field.dump() + "': " + exception.what());
                return std::nullopt;
            }
            if (!parsed)
                return std::nullopt;
            if (parsed->size == 0)
            {
                SetError(error, "material field '" + parsed->name + "' has zero size");
                return std::nullopt;
            }
            end = std::max(end, parsed->offset + parsed->size);
            layout.fields.push_back(*parsed);
        }

        if (layout.fields.empty())
        {
            SetError(error, "material reflection has no fields");
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

        std::vector<std::byte> bytes(layout.size);
        for (const MaterialFieldLayout &field : layout.fields)
        {
            if (!parameters.contains(field.name))
            {
                SetError(error, "material parameter '" + field.name + "' is missing");
                return std::nullopt;
            }

            const nlohmann::json &value = parameters.at(field.name);
            std::byte *const destination = bytes.data() + field.offset;
            const auto writeFloat = [&](const nlohmann::json &number)
            {
                const float packed = number.get<float>();
                std::memcpy(destination, &packed, sizeof(packed));
            };

            switch (field.kind)
            {
            case MaterialFieldKind::Float:
                if (!value.is_number())
                {
                    SetError(error, "material parameter '" + field.name + "' must be a number");
                    return std::nullopt;
                }
                writeFloat(value);
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
                    if (!value[i].is_number())
                    {
                        SetError(error, "material parameter '" + field.name + "' must contain numbers");
                        return std::nullopt;
                    }
                    const float packed = value[i].get<float>();
                    std::memcpy(destination + i * sizeof(float), &packed, sizeof(packed));
                }
                break;
            }
            case MaterialFieldKind::Uint:
            {
                const uint32_t packed = value.get<uint32_t>();
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::Int:
            {
                const int32_t packed = value.get<int32_t>();
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::Bool:
            {
                const uint32_t packed = value.get<bool>() ? 1u : 0u;
                std::memcpy(destination, &packed, sizeof(packed));
                break;
            }
            case MaterialFieldKind::TextureRef:
            {
                if (!value.is_object() || !value.contains("image") || !value.at("image").is_number_unsigned())
                {
                    SetError(error, "material texture '" + field.name + "' must be {image, sampler}");
                    return std::nullopt;
                }
                const uint32_t image = value.at("image").get<uint32_t>();
                std::memcpy(destination, &image, sizeof(image));
                if (field.size >= 2 * sizeof(uint32_t))
                {
                    const uint32_t sampler = value.value("sampler", 0u);
                    std::memcpy(destination + sizeof(uint32_t), &sampler, sizeof(sampler));
                }
                break;
            }
            }
        }

        return bytes;
    }

    std::string MaterialShaderStem(std::string_view nameSpace, std::string_view type)
    {
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
        return stem;
    }

    std::string MaterialShaderOutput(std::string_view stem, std::string_view entry)
    {
        return std::string(stem) + "." + std::string(entry) + ".spv";
    }
}
