/**
 * @file MaterialReflection.h
 * @author Jackey Cen
 * @brief Slang material reflection, record packing and output naming.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class MaterialFieldKind : uint32_t
    {
        Float,
        Float2,
        Float3,
        Float4,
        Uint,
        Int,
        Bool,
        TextureRef
    };

    struct MaterialFieldLayout
    {
        std::string name;
        MaterialFieldKind kind = MaterialFieldKind::Float;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct MaterialLayout
    {
        std::vector<MaterialFieldLayout> fields;
        // Rounded up to the 16-byte record alignment MaterialRecordTable uses.
        uint32_t size = 0;
    };

    // Reads the typed StructuredBuffer witness's resultType.fields. A raw
    // ByteAddressBuffer.Load<M> reflection has no fields, so callers must
    // compile SurfaceLayout.slang and pass that JSON.
    std::optional<MaterialLayout> ParseMaterialLayout(std::string_view reflectionJson, std::string *error = nullptr);

    // Packs a .material.json "parameters" object by reflected field name.
    std::optional<std::vector<std::byte>> PackMaterialRecord(const MaterialLayout &layout,
        std::string_view parametersJson, std::string *error = nullptr);

    // "<namespace>_<type>_<hash>". The hash covers the normalized shader source,
    // type and frontend, so two same-named types or punctuation/case variants
    // cannot alias; CMake uses the same key and stem.
    std::string MaterialShaderStem(std::string_view nameSpace, std::string_view type,
        std::string_view shaderPath, std::string_view frontend);

    // "<stem>.<entry>.spv", the name ShaderPath and the manifest agree on.
    std::string MaterialShaderOutput(std::string_view stem, std::string_view entry);
}
