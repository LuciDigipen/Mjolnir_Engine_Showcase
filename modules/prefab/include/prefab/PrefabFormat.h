/**
 * @file PrefabFormat.h
 * @author Jackey Cen
 * @brief Versioned .prefab JSON source format: parse, validate, write.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabDocument.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr int kPrefabFormatVersion = 1;
    inline constexpr std::string_view kPrefabExtension = ".prefab";
    inline constexpr std::string_view kPrefabDirectoryName = "prefabs";

    enum class PrefabDiagnosticSeverity : uint8_t
    {
        Error,
        Warning,
    };

    struct PrefabDiagnostic
    {
        PrefabDiagnosticSeverity mSeverity = PrefabDiagnosticSeverity::Error;
        std::string mPath; // empty when no file is known
        std::string mMessage;
    };

    bool HasPrefabErrors(std::span<const PrefabDiagnostic> diagnostics);

    struct PrefabParseResult
    {
        std::optional<PrefabDocument> mDocument;
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    PrefabParseResult ParsePrefab(const nlohmann::json &root, std::string_view path = {});
    nlohmann::json PrefabToJson(const PrefabDocument &doc);

    // One node in the .prefab node shape, without the document envelope.
    nlohmann::json PrefabNodeToJson(const PrefabDocument &doc, const PrefabNode &node);

    // Parses one PrefabNodeToJson record; errors are appended to diagnostics.
    std::optional<PrefabNode> ParsePrefabNode(const nlohmann::json &blob,
        std::vector<PrefabDiagnostic> &diagnostics, std::string_view path = {},
        std::string_view context = "node record");

    std::vector<PrefabDiagnostic> ValidatePrefab(const PrefabDocument &doc, std::string_view path = {});

    PrefabParseResult ReadPrefabFile(const std::filesystem::path &path);

    // Validates first and refuses to write an invalid document.
    bool WritePrefabFile(const std::filesystem::path &path, const PrefabDocument &doc,
        std::vector<PrefabDiagnostic> *diagnostics = nullptr);
}
