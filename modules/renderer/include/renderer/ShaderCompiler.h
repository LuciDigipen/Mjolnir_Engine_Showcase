/**
 * @file ShaderCompiler.h
 * @author Jackey Cen
 * @brief Bounded compiler jobs with content-validated shader artifacts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#pragma once

#include <renderer/ShaderGeneration.h>
#include <renderer/ShaderLibrary.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct ShaderCompilerResult
    {
        bool success = false;
        bool cancelled = false;
        int exitCode = -1;
        std::string diagnostic;
    };

    ShaderCompilerResult RunShaderCompiler(const std::filesystem::path &program,
        std::span<const std::string> arguments, const std::filesystem::path &workingDirectory,
        const std::atomic_bool *cancel = nullptr,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    struct ShaderArtifact
    {
        bool success = false;
        bool cancelled = false;
        bool superseded = false;
        bool cached = false;
        std::string diagnostic;
        ShaderManifestOutput output;
        std::vector<ShaderGenerationFile> inputs;
    };

    ShaderArtifact CompileShaderArtifact(const ShaderManifest &manifest, const ShaderManifestOutput &output,
        const std::filesystem::path &directory, const std::filesystem::path &cacheDirectory = {},
        const std::atomic_bool *cancel = nullptr);

    std::filesystem::path UniqueShaderJobDirectory(const std::filesystem::path &parent);
}
