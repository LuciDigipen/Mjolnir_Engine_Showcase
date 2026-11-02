/**
 * @file main.cpp
 * @author Jackey Cen
 * @brief Content-validated build-time shader compilation and publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <renderer/ShaderCompiler.h>
#include <renderer/ShaderGeneration.h>
#include <renderer/ShaderLibrary.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    using Json = nlohmann::json;

    std::string Utf8(const std::filesystem::path &path)
    {
        const auto bytes = path.generic_u8string();
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    struct JobDirectory
    {
        std::filesystem::path path;
        ~JobDirectory()
        {
            if (!path.empty())
            {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        }
    };

    void Require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    std::string Name(const std::string &path) { return Utf8(std::filesystem::u8path(path).filename()); }

    bool SameOutputs(const mir::ShaderGenerationSnapshot &before, const std::vector<mir::ShaderGenerationFile> &after)
    {
        if (before.mOutputs.size() != after.size())
            return false;
        for (const auto &file : after)
        {
            const auto found = std::find_if(before.mOutputs.begin(), before.mOutputs.end(),
                [&](const auto &candidate) { return candidate.mPath == file.mPath; });
            if (found == before.mOutputs.end() || found->mSize != file.mSize ||
                found->mContentHash != file.mContentHash)
                return false;
        }
        return true;
    }

    int Run(int argc, char **argv)
    {
        std::map<std::string, std::string> options;
        Require((argc % 2) == 1, "every option requires a value");
        for (int index = 1; index < argc; index += 2)
        {
            const std::string option = argv[index];
            Require(option == "--manifest" || option == "--catalog" || option == "--root" ||
                option == "--cache" || option == "--compile", "unknown option: " + option);
            Require(options.emplace(option, argv[index + 1]).second, "duplicate option: " + option);
        }
        Require(options.contains("--manifest"), "usage: ShaderPublisher --manifest <json> --cache <dir> "
            "[--compile <output-name> | --catalog <json> --root <runtime-dir>]");
        const auto manifestPath = std::filesystem::absolute(std::filesystem::u8path(options.at("--manifest")));
        const auto cache = options.contains("--cache")
            ? std::filesystem::absolute(std::filesystem::u8path(options.at("--cache"))) : manifestPath.parent_path();
        const auto jobs = cache / ".jobs";
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            std::string error;
            const auto manifestInput = mir::SnapshotShaderFile(manifestPath, Utf8(manifestPath));
            const auto manifest = mir::LoadShaderManifest(manifestPath, &error);
            Require(manifest.has_value() && manifestInput.has_value(), error);
            JobDirectory job{mir::UniqueShaderJobDirectory(jobs)};
            std::vector<mir::ShaderGenerationFile> inputs{*manifestInput};
            std::vector<mir::ShaderArtifact> artifacts;
            std::optional<uint64_t> parent;
            std::filesystem::path root;
            Json catalog;
            if (!options.contains("--compile"))
            {
                Require(options.contains("--root") && options.contains("--catalog"), "catalog and root are required");
                root = std::filesystem::absolute(std::filesystem::u8path(options.at("--root")));
                mir::ShaderGenerationStore reader(root);
                parent = reader.CurrentId();
                const auto catalogPath = std::filesystem::absolute(std::filesystem::u8path(options.at("--catalog")));
                const auto snapshot = mir::SnapshotShaderFile(catalogPath, Utf8(catalogPath));
                const auto text = mir::ReadText(catalogPath);
                Require(snapshot.has_value() && text.has_value(), "surface catalog is missing");
                inputs.push_back(*snapshot);
                catalog = Json::parse(*text);
                for (auto &entry : catalog.at("materials"))
                {
                    auto source = std::filesystem::u8path(entry.at("asset").get<std::string>());
                    if (source.is_relative()) source = manifest->workingDirectory / source;
                    const auto sourceSnapshot = mir::SnapshotShaderFile(source, Utf8(source));
                    const auto sourceText = mir::ReadText(source);
                    Require(sourceSnapshot.has_value() && sourceText.has_value(), "material source is missing: " + Utf8(source));
                    const auto material = Json::parse(*sourceText);
                    Require(material.at("shader").at("type") == entry.at("type"),
                        "material type changed; reconfigure before publishing the build");
                    const auto shaderFile = std::filesystem::u8path(material.at("shader").at("file").get<std::string>());
                    Require(!shaderFile.is_absolute() && std::none_of(shaderFile.begin(), shaderFile.end(),
                        [](const auto &part) { return part == ".."; }), "material shader leaves approved roots");
                    const auto expectedShader = std::filesystem::weakly_canonical(manifest->workingDirectory /
                        std::filesystem::u8path(entry.at("shader").get<std::string>()));
                    bool matchedShader = false;
                    for (const auto &assetRoot : manifest->assetRoots)
                    {
                        auto approved = assetRoot.root;
                        if (approved.is_relative()) approved = manifest->workingDirectory / approved;
                        const auto candidate = std::filesystem::weakly_canonical(approved / shaderFile);
                        matchedShader |= candidate == expectedShader;
                    }
                    Require(matchedShader, "material shader changed; reconfigure before publishing the build");
                    const auto &frontends = material.at("frontends");
                    Require(std::find(frontends.begin(), frontends.end(), entry.at("frontend")) != frontends.end(),
                        "material frontend changed; reconfigure before publishing the build");
                    inputs.push_back(*sourceSnapshot);
                    entry["parameters"] = material.value("parameters", Json::object());
                }
            }
            bool retry = false;
            for (const auto &definition : manifest->outputs)
            {
                if (options.contains("--compile") && Name(definition.output) != options.at("--compile"))
                    continue;
                auto artifact = mir::CompileShaderArtifact(*manifest, definition, job.path, cache);
                if (!artifact.success)
                {
                    if (artifact.superseded && attempt != 2)
                    { retry = true; break; }
                    throw std::runtime_error(Name(definition.output) + ": " + artifact.diagnostic);
                }
                inputs.insert(inputs.end(), artifact.inputs.begin(), artifact.inputs.end());
                if (!artifact.cached)
                    std::cout << "Compiled " << Name(definition.output) << '\n';
                artifacts.push_back(std::move(artifact));
            }
            if (retry) continue;
            Require(!artifacts.empty(), "manifest output is missing");
            if (!mir::VerifyShaderSnapshot(inputs, &error))
            {
                if (attempt != 2) continue;
                throw std::runtime_error(error);
            }
            if (options.contains("--compile"))
                return 0;
            mir::ShaderGenerationStore store(root);
            uint64_t generation = 0;
            Require(store.BeginPublish(&generation, &error), error);
            if (store.CurrentId() != parent)
            {
                store.Abort();
                continue;
            }
            auto publishedManifest = *manifest;
            for (auto &definition : publishedManifest.outputs)
            {
                definition.output = Name(definition.output);
                definition.reflection = Name(definition.reflection);
                definition.depfile = Name(definition.depfile);
            }
            for (const auto &artifact : artifacts)
                for (const auto &path : {artifact.output.output, artifact.output.reflection, artifact.output.depfile})
                    Require(store.StageFromFile(Name(path), std::filesystem::u8path(path), &error), error);
            const auto stageText = [&](const std::string &name, const std::string &text)
                {
                    Require(store.StageFile(name, std::as_bytes(std::span(text.data(), text.size())), &error), error);
                };
            stageText("shader_manifest.json", mir::SerializeShaderManifest(publishedManifest));
            stageText("surface_materials.json", catalog.dump(2));
            if (const auto current = mir::ShaderGenerationStore::ReadCurrent(root);
                current && SameOutputs(*current, store.Outputs()))
            {
                store.Abort();
                std::cout << "Shader generation unchanged\n";
                return 0;
            }
            store.SetInputs(std::move(inputs));
            Require(store.Finish(&error), error);
            Require(store.PublishPointer(&error), error);
            store.Unlock();
            std::cout << "Published shader generation " << generation << '\n';
            return 0;
        }
        throw std::runtime_error("shader inputs kept changing; no generation was published");
    }
}

int main(int argc, char **argv)
{
    mir::InitLog();
    try { return Run(argc, argv); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
