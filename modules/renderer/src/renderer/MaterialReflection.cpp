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

        bool ReadU32(const nlohmann::json &value, uint32_t &out)
        {
            if (!value.is_number_unsigned())
                return false;
            const uint64_t wide = value.get<uint64_t>();
            if (wide > UINT32_MAX)
                return false;
            out = static_cast<uint32_t>(wide);
            return true;
        }

        uint32_t FieldAlignment(MaterialFieldKind kind)
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
            case MaterialFieldKind::Float4:
                return 16;
            case MaterialFieldKind::TextureRef:
                return 4;
            }
            return 4;
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

        nlohmann::json TypeDefault(MaterialFieldKind kind, uint32_t size)
        {
            switch (kind)
            {
            case MaterialFieldKind::Float:
                return 0.0f;
            case MaterialFieldKind::Float2:
                return nlohmann::json::array({ 0.0f, 0.0f });
            case MaterialFieldKind::Float3:
                return nlohmann::json::array({ 0.0f, 0.0f, 0.0f });
            case MaterialFieldKind::Float4:
                return nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
            case MaterialFieldKind::Uint:
            case MaterialFieldKind::Int:
                return 0;
            case MaterialFieldKind::Bool:
                return false;
            case MaterialFieldKind::TextureRef:
                if (size == 8)
                    return nlohmann::json{ { "image", 0 }, { "sampler", 0 } };
                return nlohmann::json{ { "image", 0 } };
            }
            return nullptr;
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
            if (!binding.contains("offset") || !binding.contains("size") ||
                !ReadU32(binding.at("offset"), layout.offset) || !ReadU32(binding.at("size"), layout.size))
            {
                SetError(error, "material field '" + layout.name + "' has an invalid binding");
                return std::nullopt;
            }
            if (layout.size == 0 || layout.offset > UINT32_MAX - layout.size)
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
                if (layout.offset % FieldAlignment(layout.kind) != 0)
                {
                    SetError(error, "material field '" + layout.name + "' has the wrong alignment");
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
                    uint32_t offset = 0;
                    uint32_t size = 0;
                    if ((innerName != "image" && innerName != "sampler") || !innerType.is_object() ||
                        innerType.value("kind", "") != "scalar" ||
                        innerType.value("scalarType", "") != "uint32" || !innerBinding.contains("offset") ||
                        !innerBinding.contains("size") || !ReadU32(innerBinding.at("offset"), offset) ||
                        !ReadU32(innerBinding.at("size"), size) || size != 4 || offset % 4 != 0)
                    {
                        SetError(error, "material texture '" + layout.name + "' has an unsupported member");
                        return std::nullopt;
                    }
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
                if (layout.offset % FieldAlignment(layout.kind) != 0)
                {
                    SetError(error, "material texture '" + layout.name + "' has the wrong alignment");
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
                if (value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX)
                {
                    SetError(error, "material parameter '" + field.name + "' is out of int32 range");
                    return std::nullopt;
                }
                const int64_t wide = value.is_number_unsigned() ? static_cast<int64_t>(value.get<uint64_t>())
                                                                 : value.get<int64_t>();
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

    std::optional<std::string> MergeMaterialParameterDefaults(const MaterialLayout &layout,
        std::string_view parametersJson, std::string_view catalogDefaultsJson, std::string *error)
    {
        nlohmann::json parameters;
        nlohmann::json catalogDefaults;
        try
        {
            parameters = nlohmann::json::parse(parametersJson);
            catalogDefaults = nlohmann::json::parse(catalogDefaultsJson);
        }
        catch (const nlohmann::json::exception &exception)
        {
            SetError(error, std::string("material parameters are not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!parameters.is_object() || !catalogDefaults.is_object())
        {
            SetError(error, "material parameters and catalog defaults must be JSON objects");
            return std::nullopt;
        }

        for (const MaterialFieldLayout &field : layout.fields)
        {
            if (parameters.contains(field.name))
                continue;
            if (catalogDefaults.contains(field.name))
                parameters[field.name] = catalogDefaults.at(field.name);
            else
                parameters[field.name] = TypeDefault(field.kind, field.size);
        }
        return parameters.dump();
    }

    std::optional<std::string> CompleteMaterialParameters(const MaterialLayout &layout,
        std::string_view parametersJson, std::string_view catalogDefaultsJson, std::string *error)
    {
        const std::optional<std::string> merged =
            MergeMaterialParameterDefaults(layout, parametersJson, catalogDefaultsJson, error);
        if (!merged)
            return std::nullopt;

        const nlohmann::json authored = nlohmann::json::parse(*merged);
        nlohmann::json completed = nlohmann::json::object();
        for (const MaterialFieldLayout &field : layout.fields)
            completed[field.name] = authored.at(field.name);
        return completed.dump();
    }

    std::optional<std::string> MaterialRecordParameters(const MaterialLayout &layout,
        std::span<const std::byte> record, std::string *error)
    {
        if (layout.size == 0 || record.size() < layout.size)
        {
            SetError(error, "material record does not cover its layout");
            return std::nullopt;
        }

        nlohmann::json parameters = nlohmann::json::object();
        for (const MaterialFieldLayout &field : layout.fields)
        {
            // Every read below uses the kind's own storage, so a layout whose
            // declared size disagrees with its kind must not be decoded.
            const bool storage = field.kind == MaterialFieldKind::TextureRef
                ? field.size == 4 || field.size == 8
                : field.size == ExpectedSize(field.kind);
            if (!storage || field.offset > record.size() || field.size > record.size() - field.offset)
            {
                SetError(error, "material field '" + field.name + "' does not fit the record");
                return std::nullopt;
            }
            const std::byte *const source = record.data() + field.offset;
            switch (field.kind)
            {
            case MaterialFieldKind::Float:
            {
                float value = 0.0f;
                std::memcpy(&value, source, sizeof(value));
                parameters[field.name] = value;
                break;
            }
            case MaterialFieldKind::Float2:
            case MaterialFieldKind::Float3:
            case MaterialFieldKind::Float4:
            {
                const uint32_t count = static_cast<uint32_t>(field.kind == MaterialFieldKind::Float2 ? 2
                    : field.kind == MaterialFieldKind::Float3 ? 3
                                                              : 4);
                nlohmann::json value = nlohmann::json::array();
                for (uint32_t i = 0; i < count; ++i)
                {
                    float component = 0.0f;
                    std::memcpy(&component, source + i * sizeof(float), sizeof(component));
                    value.push_back(component);
                }
                parameters[field.name] = std::move(value);
                break;
            }
            case MaterialFieldKind::Uint:
            {
                uint32_t value = 0;
                std::memcpy(&value, source, sizeof(value));
                parameters[field.name] = value;
                break;
            }
            case MaterialFieldKind::Int:
            {
                int32_t value = 0;
                std::memcpy(&value, source, sizeof(value));
                parameters[field.name] = value;
                break;
            }
            case MaterialFieldKind::Bool:
            {
                uint32_t value = 0;
                std::memcpy(&value, source, sizeof(value));
                parameters[field.name] = value != 0;
                break;
            }
            case MaterialFieldKind::TextureRef:
            {
                uint32_t image = 0;
                std::memcpy(&image, source, sizeof(image));
                nlohmann::json value{ { "image", image } };
                if (field.size == 8)
                {
                    uint32_t sampler = 0;
                    std::memcpy(&sampler, source + sizeof(uint32_t), sizeof(sampler));
                    value["sampler"] = sampler;
                }
                parameters[field.name] = std::move(value);
                break;
            }
            }
        }
        return parameters.dump();
    }

    std::optional<std::string> FilterMaterialParameterOverlay(const MaterialLayout &layout,
        std::string_view overlayJson, std::string *error)
    {
        nlohmann::json overlay;
        try
        {
            overlay = nlohmann::json::parse(overlayJson);
        }
        catch (const nlohmann::json::exception &exception)
        {
            SetError(error, std::string("material parameter overlay is not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!overlay.is_object())
        {
            SetError(error, "material parameter overlay must be a JSON object");
            return std::nullopt;
        }

        nlohmann::json filtered = nlohmann::json::object();
        for (const MaterialFieldLayout &field : layout.fields)
        {
            const auto authored = overlay.find(field.name);
            if (authored != overlay.end())
                filtered[field.name] = *authored;
        }
        return filtered.dump();
    }

    std::optional<std::string> MergePeerNumericParameters(const MaterialLayout &layout,
        std::string_view parametersJson, const MaterialLayout &peerLayout,
        std::span<const std::byte> peerRecord, std::string *error)
    {
        if (peerLayout.size == 0 || peerRecord.size() < peerLayout.size)
        {
            SetError(error, "the peer material record does not cover its layout");
            return std::nullopt;
        }
        const std::optional<std::string> peerParameters = MaterialRecordParameters(peerLayout, peerRecord, error);
        if (!peerParameters)
            return std::nullopt;

        nlohmann::json parameters;
        nlohmann::json peer;
        try
        {
            parameters = nlohmann::json::parse(parametersJson);
            peer = nlohmann::json::parse(*peerParameters);
        }
        catch (const nlohmann::json::exception &exception)
        {
            SetError(error, std::string("material parameters are not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!parameters.is_object())
        {
            SetError(error, "material parameters must be a JSON object");
            return std::nullopt;
        }

        for (const MaterialFieldLayout &field : layout.fields)
        {
            if (field.kind == MaterialFieldKind::TextureRef)
                continue;
            for (const MaterialFieldLayout &peerField : peerLayout.fields)
            {
                if (peerField.name != field.name || peerField.kind != field.kind ||
                    peerField.size != field.size || !peer.contains(peerField.name))
                    continue;
                parameters[field.name] = peer.at(peerField.name);
                break;
            }
        }
        return parameters.dump();
    }
    std::string MaterialLayoutRevision(const MaterialLayout &layout)
    {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](std::string_view bytes)
        {
            for (const unsigned char byte : bytes)
            {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
        };
        const auto mixValue = [&](uint32_t value)
        {
            char bytes[sizeof(value)];
            std::memcpy(bytes, &value, sizeof(value));
            mix(std::string_view(bytes, sizeof(bytes)));
        };
        mixValue(layout.size);
        for (const MaterialFieldLayout &field : layout.fields)
        {
            mix(field.name);
            mixValue(static_cast<uint32_t>(field.kind));
            mixValue(field.offset);
            mixValue(field.size);
        }
        return std::to_string(hash);
    }

    std::optional<std::string> OverlayMaterialParameters(const MaterialLayout &layout,
        std::string_view resolvedParametersJson, std::string_view overridesJson, std::string *error)
    {
        nlohmann::json resolved;
        nlohmann::json overrides;
        try
        {
            resolved = nlohmann::json::parse(resolvedParametersJson);
            overrides = nlohmann::json::parse(overridesJson);
        }
        catch (const nlohmann::json::exception &exception)
        {
            SetError(error, std::string("material parameters are not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!resolved.is_object() || !overrides.is_object())
        {
            SetError(error, "material parameters and overrides must be JSON objects");
            return std::nullopt;
        }

        for (const auto &[name, value] : overrides.items())
        {
            const auto field = std::find_if(layout.fields.begin(), layout.fields.end(),
                [&](const MaterialFieldLayout &candidate) { return candidate.name == name; });
            if (field == layout.fields.end())
            {
                SetError(error, "material parameter '" + name + "' is not in the reflected layout");
                return std::nullopt;
            }
            if (field->kind == MaterialFieldKind::TextureRef)
            {
                SetError(error, "material texture '" + name + "' is edited through the source recipe");
                return std::nullopt;
            }
            resolved[name] = value;
        }

        const std::optional<std::vector<std::byte>> packed = PackMaterialRecord(layout, resolved.dump(), error);
        if (!packed)
            return std::nullopt;
        return resolved.dump();
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
