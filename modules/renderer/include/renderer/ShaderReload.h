/**
 * @file ShaderReload.h
 * @author Jackey Cen
 * @brief All-or-none shader reload stages and the call-site coverage registry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/ShaderLibrary.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mir
{
    // Outputs without transactional owners must stay byte-identical across a live reload.
    ShaderCompatibility ValidateShaderOwnerCoverage(const std::vector<std::string> &managed,
        const std::map<std::string, uint64_t> &accepted, const std::map<std::string, uint64_t> &candidate);

    struct ShaderReloadBatch
    {
        uint64_t revision = 0;
        std::vector<std::string> changedSources;
        std::vector<std::string> outputs;
    };

    // Every owner builds a candidate off to the side first; only after every
    // prepare succeeded does any commit run. A failed prepare discards the
    // candidates that did succeed, leaving the whole previous generation live.
    class ShaderReloadTransaction
    {
    public:
        using Prepare = std::function<bool(std::string &error)>;
        using Publish = std::function<void()>;
        using Discard = std::function<void()>;

        void AddStage(std::string name, Prepare prepare, Publish publish, Discard discard);
        bool PrepareAll(std::string *error = nullptr);
        // Only valid after a successful PrepareAll.
        void CommitAll();
        void DiscardAll();

        bool IsPrepared() const { return mPrepared == mStages.size() && !mStages.empty(); }
        std::size_t StageCount() const { return mStages.size(); }
        const std::vector<std::string> &StageNames() const;
        void Clear();

    private:
        struct Stage
        {
            std::string name;
            Prepare prepare;
            Publish publish;
            Discard discard;
        };

        std::vector<Stage> mStages;
        std::vector<std::string> mNames;
        std::size_t mPrepared = 0;
        bool mCommitted = false;
    };

    struct ShaderLibraryConsumer
    {
        std::string name;
        std::vector<std::string> outputs;
        std::vector<std::string> stages;
    };

    // The registry the renderer's owners register with. It proves that every
    // actual ShaderPath call site is described by the manifest before a reload
    // may touch any pipeline.
    class ShaderLibrary
    {
    public:
        void Register(std::string name, std::vector<std::string> outputs, std::vector<std::string> stages);
        bool ValidateCoverage(const ShaderManifest &manifest, std::string *error = nullptr) const;
        const std::vector<ShaderLibraryConsumer> &Consumers() const { return mConsumers; }
        std::vector<std::string> Outputs() const;
        void Clear();

    private:
        std::vector<ShaderLibraryConsumer> mConsumers;
    };
}
