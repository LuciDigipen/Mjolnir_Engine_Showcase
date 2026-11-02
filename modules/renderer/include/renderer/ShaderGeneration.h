/**
 * @file ShaderGeneration.h
 * @author Jackey Cen
 * @brief Immutable shader generations, content snapshots and cross-process publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct ShaderGenerationFile
    {
        std::string mPath;
        uint64_t mSize = 0;
        uint64_t mContentHash = 0;
    };

    struct ShaderGenerationSnapshot
    {
        uint64_t mId = 0;
        uint64_t mParent = 0;
        std::vector<ShaderGenerationFile> mInputs;
        std::vector<ShaderGenerationFile> mOutputs;
    };

    uint64_t HashShaderContent(std::span<const std::byte> bytes);
    std::optional<ShaderGenerationFile> SnapshotShaderFile(const std::filesystem::path &path,
        std::string_view logicalPath);
    bool VerifyShaderSnapshot(const std::vector<ShaderGenerationFile> &files, std::string *error = nullptr);

    // Readers pin one immutable generation directory. Publication holds an OS
    // writer lock over the shared lock file and writes a new generation plus
    // the current.json pointer; a failure before the pointer rename leaves the
    // previous generation live.
    class ShaderGenerationStore
    {
    public:
        explicit ShaderGenerationStore(std::filesystem::path root);
        ~ShaderGenerationStore();

        ShaderGenerationStore(const ShaderGenerationStore &) = delete;
        ShaderGenerationStore &operator=(const ShaderGenerationStore &) = delete;

        const std::filesystem::path &Root() const { return mRoot; }
        std::filesystem::path GenerationPath(uint64_t id) const;
        std::filesystem::path StagingPath() const { return mStaging; }
        const std::vector<ShaderGenerationFile> &Outputs() const { return mOutputs; }
        std::optional<uint64_t> CurrentId() const;
        std::filesystem::path CurrentPath() const;

        bool BeginPublish(uint64_t *outId, std::string *error = nullptr, bool wait = true);
        bool StageFile(std::string_view name, std::span<const std::byte> bytes, std::string *error = nullptr);
        bool StageFromFile(std::string_view name, const std::filesystem::path &source,
            std::string *error = nullptr);
        void SetInputs(std::vector<ShaderGenerationFile> inputs);
        // Recomputes every captured input before the generation can be sealed.
        bool ValidateInputs(std::string *error = nullptr) const;
        bool Finish(std::string *error = nullptr);
        // Fallible pointer promotion. Call after GPU candidates are prepared and
        // before the no-fail GPU commit.
        bool PublishPointer(std::string *error = nullptr);
        void Abort();
        void Unlock();

        // A cold reader resolves the current pointer, falling back to the
        // parent generation when the current one fails its integrity check.
        static std::optional<ShaderGenerationSnapshot> ReadCurrent(const std::filesystem::path &root,
            std::string *error = nullptr);
        static std::optional<ShaderGenerationSnapshot> ReadGeneration(const std::filesystem::path &root,
            uint64_t id, std::string *error = nullptr);

    private:
        bool Lock(std::string *error, bool wait);
        bool WriteMetadata(std::string *error);
        bool VerifyOutputs(const std::filesystem::path &directory, std::string *error) const;
        bool CloneBase(std::string *error);

        std::filesystem::path mRoot;
        std::filesystem::path mStaging;
        uint64_t mParent = 0;
        uint64_t mExpectedCurrent = 0;
        uint64_t mId = 0;
        bool mPublishing = false;
        bool mFinished = false;
        bool mLocked = false;
#if defined(_WIN32)
        void *mLockHandle = nullptr;
#else
        int mLockHandle = -1;
#endif
        std::vector<ShaderGenerationFile> mInputs;
        std::vector<ShaderGenerationFile> mOutputs;
    };
}
