/**
 * @file ShaderLibrary.h
 * @author Jackey Cen
 * @brief Build manifest, dependency, ABI and staged-publication contracts for shader hot reload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/MaterialReflection.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct ShaderManifestOutput
    {
        std::string type;
        std::string frontend;
        std::vector<std::string> sources;
        std::string entry;
        std::string stage;
        std::string specialize;
        std::string output;
        std::string reflection;
        std::string depfile;
        std::vector<std::string> arguments;
    };

    struct ShaderFrontendTemplate
    {
        std::string frontend;
        std::vector<std::string> placeholders;
        std::vector<std::string> vertex;
        std::vector<std::string> fragment;
        std::vector<std::string> raytracing;
        std::vector<std::string> layout;
    };

    struct ShaderAssetRoot
    {
        std::filesystem::path root;
        std::string nameSpace;
    };

    struct ShaderManifest
    {
        std::filesystem::path slangc;
        std::filesystem::path workingDirectory;
        std::vector<std::string> commonArguments;
        std::vector<ShaderManifestOutput> outputs;
        std::vector<ShaderFrontendTemplate> frontendTemplates;
        std::vector<ShaderAssetRoot> assetRoots;

        const ShaderManifestOutput *Find(std::string_view outputPath) const;
    };

    // Missing, malformed or version-mismatched manifest is a bounded failure: the
    // development path reports it and keeps the last accepted shader generation.
    std::optional<ShaderManifest> LoadShaderManifest(const std::filesystem::path &path,
        std::string *error = nullptr);

    std::string SerializeShaderManifest(const ShaderManifest &manifest);

    // Makefile-style depfile. Handles escaped spaces, duplicated rules, line
    // continuations and Windows drive paths; the first target is dropped.
    std::optional<std::vector<std::string>> ParseShaderDepfile(std::string_view text,
        std::string *error = nullptr);

    // 0x07230203 magic and a word count that matches the declared length.
    bool ValidateSpirvModule(std::span<const std::byte> bytes, std::string *error = nullptr);

    // The actual OpEntryPoint execution model, independent of what a
    // reflection file or manifest claims. Empty when the entry point is absent.
    std::optional<std::string> SpirvEntryPointStage(std::span<const std::byte> bytes,
        std::string_view entryPoint, std::string *error = nullptr);

    struct ShaderDescriptorBinding
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        uint32_t count = 1;
        std::string type;

        bool operator==(const ShaderDescriptorBinding &) const = default;
        auto operator<=>(const ShaderDescriptorBinding &) const = default;
    };

    struct ShaderPushConstantRange
    {
        uint32_t offset = 0;
        uint32_t size = 0;
        std::string stages;
        // Recursive member offsets, sizes and scalar base types.
        std::string abi;

        bool operator==(const ShaderPushConstantRange &) const = default;
        auto operator<=>(const ShaderPushConstantRange &) const = default;
    };

    struct ShaderStageIo
    {
        uint32_t location = 0;
        std::string semantic;
        std::string type;

        bool operator==(const ShaderStageIo &) const = default;
        auto operator<=>(const ShaderStageIo &) const = default;
    };

    struct ShaderReflectionSignature
    {
        std::string entryPoint;
        std::string stage;
        std::vector<ShaderDescriptorBinding> descriptors;
        std::vector<ShaderPushConstantRange> pushConstants;
        std::vector<ShaderStageIo> inputs;
        std::vector<ShaderStageIo> outputs;
    };

    // expectedStage comes from the manifest: a ray-tracing entry point's
    // reflection may omit "stage", so it is never the fallback source of truth.
    std::optional<ShaderReflectionSignature> ParseShaderReflection(std::string_view json,
        std::string_view expectedStage, std::string *error = nullptr);

    struct ShaderCompatibility
    {
        bool compatible = false;
        // A descriptor, push-constant or stage-IO change needs a restart; the
        // old pipeline layout cannot be reused with the new module.
        bool restartRequired = false;
        std::vector<std::string> differences;
    };

    ShaderCompatibility CompareShaderReflection(const ShaderReflectionSignature &accepted,
        const ShaderReflectionSignature &candidate);

    // Canonical SPIR-V runtime ABI. The signature deliberately excludes IDs,
    // names and instruction bodies: a body-only edit keeps it identical while a
    // matrix major, stride, offset, array or scalar-type change does not.
    struct ShaderSpirvAbi
    {
        std::string entryPoint;
        std::string stage;
        std::vector<std::string> descriptors;
        std::vector<std::string> pushConstants;
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    std::optional<ShaderSpirvAbi> ParseSpirvAbi(std::span<const std::byte> bytes,
        std::string_view entryPoint, std::string *error = nullptr);

    ShaderCompatibility CompareSpirvAbi(const ShaderSpirvAbi &accepted, const ShaderSpirvAbi &candidate);

    struct ShaderSourceFingerprint
    {
        uint64_t size = 0;
        uint64_t writeTime = 0;
        bool valid = false;
        uint64_t contentHash = 0;

        bool operator==(const ShaderSourceFingerprint &) const = default;
    };

    std::optional<ShaderSourceFingerprint> FingerprintShaderSource(const std::filesystem::path &path);

    // Reverse dependency graph over manifest sources and depfiles. A revision is
    // the cancellation token: a compile batch started at an older revision can
    // never publish over a newer one.
    class ShaderDependencyIndex
    {
    public:
        void Clear();
        void AddDependency(std::string source, std::string output);
        void AddOutput(std::string output);
        // Reads each manifest output's depfile when present and adds its sources.
        bool RebuildFromManifest(const ShaderManifest &manifest, std::string *error = nullptr);

        bool Observe(std::string_view source, const ShaderSourceFingerprint &fingerprint);
        // Reads every known source and returns the ones that changed.
        std::vector<std::string> Poll();
        std::vector<std::string> AffectedOutputs(std::span<const std::string> changedSources) const;
        std::vector<std::string> DependenciesOf(std::string_view output) const;
        std::vector<std::string> Sources() const;
        const ShaderSourceFingerprint *FingerprintOf(std::string_view source) const;
        uint64_t Revision() const { return mRevision; }
        bool IsCurrent(uint64_t revision) const { return revision == mRevision; }

    private:
        struct SourceState
        {
            std::string path;
            ShaderSourceFingerprint fingerprint;
        };
        struct Edge
        {
            std::string source;
            std::string output;
        };

        std::vector<SourceState> mSources;
        std::vector<std::string> mOutputs;
        std::vector<Edge> mEdges;
        uint64_t mRevision = 0;
    };

    struct MaterialMigration
    {
        bool ok = false;
        bool recordChanged = false;
        std::string error;
        // Every new field that had no compatible predecessor.
        std::vector<std::string> defaultedFields;
        std::vector<std::byte> bytes;
    };

    // Name-based migration. A same-name field keeps its value only when its kind
    // and size are unchanged; a new name takes `defaults`; a changed type rejects
    // the whole transaction so a shader never reads a stale offset.
    MaterialMigration MigrateMaterialRecord(const MaterialLayout &acceptedLayout,
        const MaterialLayout &candidateLayout, std::span<const std::byte> acceptedBytes,
        std::span<const std::byte> defaultBytes);

    struct ShaderOutputStamp
    {
        uint64_t revision = 0;
        uint64_t sourceFingerprint = 0;
        bool valid = false;
    };

    // Every file is written as a sibling temporary and renamed over its target
    // only at Commit. A newer accepted stamp refuses an older build-copy of the
    // same output, so a background build cannot roll back a live reload.
    class StagedShaderPublisher
    {
    public:
        StagedShaderPublisher(std::filesystem::path directory, uint64_t revision, uint64_t sourceFingerprint);

        bool Stage(std::string_view name, std::span<const std::byte> bytes, std::string *error = nullptr);
        bool StageText(std::string_view name, std::string_view text, std::string *error = nullptr);
        // Publishes every staged file, keeping the previous files as backups so a
        // failed GPU transaction can restore the whole batch. Finalize drops the
        // backups; Rollback restores them.
        bool Commit(std::string *error = nullptr);
        void Finalize();
        void Rollback();
        void Discard();

        bool Empty() const { return mStaged.empty(); }
        const std::vector<std::string> &StagedNames() const { return mStaged; }
        const std::filesystem::path &Directory() const { return mDirectory; }

        static std::optional<ShaderOutputStamp> ReadStamp(const std::filesystem::path &target);

    private:
        struct StagedFile
        {
            std::string name;
            std::filesystem::path temporary;
            std::filesystem::path target;
            std::filesystem::path backup;
            bool mHadOriginal = false;
            bool mPublished = false;
            bool mHadStamp = false;
            std::string mOldStamp;
        };

        std::filesystem::path mDirectory;
        uint64_t mRevision = 0;
        uint64_t mSourceFingerprint = 0;
        std::vector<std::string> mStaged;
        std::vector<StagedFile> mFiles;
    };
}
