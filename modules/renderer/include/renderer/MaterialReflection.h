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
#include <functional>
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

    using MaterialRecordValidator = std::function<bool(std::string_view frontend,
        const MaterialLayout &, std::span<const std::byte>, std::string &)>;

    // Reads the typed StructuredBuffer witness's resultType.fields. A raw
    // ByteAddressBuffer.Load<M> reflection has no fields, so callers must
    // compile SurfaceLayout.slang and pass that JSON.
    std::optional<MaterialLayout> ParseMaterialLayout(std::string_view reflectionJson, std::string *error = nullptr);

    // Changes whenever a field name, kind, offset, size or the record size
    // changes, so a same-sized offset reorder cannot reuse an old layout.
    std::string MaterialLayoutRevision(const MaterialLayout &layout);

    // Packs a .material.json "parameters" object by reflected field name.
    std::optional<std::vector<std::byte>> PackMaterialRecord(const MaterialLayout &layout,
        std::string_view parametersJson, std::string *error = nullptr);

    // Resolves a parameters object against one reflected layout: a field the
    // object already holds keeps its value, a field it lacks takes the catalog's
    // authored value and then the typed default, and a key the layout no longer
    // declares is dropped so a shader-only field deletion cannot fail packing.
    std::optional<std::string> CompleteMaterialParameters(const MaterialLayout &layout,
        std::string_view parametersJson, std::string_view catalogDefaultsJson,
        std::string *error = nullptr);

    // The same defaults without projecting away authored keys. A viewer keeps
    // reporting keys the layout no longer declares while the packed record uses
    // CompleteMaterialParameters.
    std::optional<std::string> MergeMaterialParameterDefaults(const MaterialLayout &layout,
        std::string_view parametersJson, std::string_view catalogDefaultsJson,
        std::string *error = nullptr);

    // The parameters object a packed record holds for its layout, TextureRef
    // image/sampler rows included. Nullopt for a record that does not cover it.
    std::optional<std::string> MaterialRecordParameters(const MaterialLayout &layout,
        std::span<const std::byte> record, std::string *error = nullptr);

    // Keeps only the overlay keys this layout declares, so one asset's fronts
    // each receive the fields they reflect.
    std::optional<std::string> FilterMaterialParameterOverlay(const MaterialLayout &layout,
        std::string_view overlayJson, std::string *error = nullptr);

    // Copies same-name, same-kind, same-size numeric values from a peer record
    // into one parameters object of this layout. TextureRef rows and fields the
    // peer lacks keep their own values.
    std::optional<std::string> MergePeerNumericParameters(const MaterialLayout &layout,
        std::string_view parametersJson, const MaterialLayout &peerLayout,
        std::span<const std::byte> peerRecord, std::string *error = nullptr);

    // Overlays reflected numeric fields from a second parameters object onto
    // one already resolved by CreateFromAsset. TextureRef rows and fields the
    // overlay leaves out keep their resolved values; an unknown name, a
    // TextureRef override or an unusable number is rejected.
    std::optional<std::string> OverlayMaterialParameters(const MaterialLayout &layout,
        std::string_view resolvedParametersJson, std::string_view overridesJson, std::string *error = nullptr);

    // "<namespace>_<type>_<hash>". The hash covers the normalized shader source,
    // type and frontend, so two same-named types or punctuation/case variants
    // cannot alias; CMake uses the same key and stem.
    std::string MaterialShaderStem(std::string_view nameSpace, std::string_view type,
        std::string_view shaderPath, std::string_view frontend);

    // "<stem>.<entry>.spv", the name ShaderPath and the manifest agree on.
    std::string MaterialShaderOutput(std::string_view stem, std::string_view entry);
}
