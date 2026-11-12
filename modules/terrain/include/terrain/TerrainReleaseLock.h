/**
 * @file TerrainReleaseLock.h
 * @author Jackey Cen
 * @brief Shares one release's reader lock between a reader and a pruner.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace mir
{
    // Kept outside the release directory, so a release's files and its listing never change.
    inline constexpr std::string_view kTerrainReleaseLocksDirName = "locks";

    std::filesystem::path TerrainReleaseLockDir(const std::filesystem::path &assetDir);
    std::filesystem::path TerrainReleaseLockPath(const std::filesystem::path &assetDir, TerrainReleaseId release);

    enum class TerrainReleaseLockStatus : uint8_t
    {
        Held,
        // Another process holds the other mode.
        Busy,
        // No lock file could be opened; the caller goes on without a lock.
        Unavailable,
    };

    // The OS drops both modes when the holding process dies, so there is no stale lease.
    class TerrainReleaseLock
    {
      public:
        // Many are held at once; Busy while a pruner holds this release.
        [[nodiscard]] static TerrainReleaseLockStatus AcquireShared(const std::filesystem::path &assetDir,
                                                                   TerrainReleaseId release,
                                                                   std::shared_ptr<const TerrainReleaseLock> &lock);
        // One at a time, without waiting; Busy while any reader holds this release.
        [[nodiscard]] static TerrainReleaseLockStatus TryAcquireExclusive(const std::filesystem::path &assetDir,
                                                                         TerrainReleaseId release,
                                                                         std::unique_ptr<TerrainReleaseLock> &lock);

        TerrainReleaseLock(const TerrainReleaseLock &) = delete;
        TerrainReleaseLock &operator=(const TerrainReleaseLock &) = delete;
        ~TerrainReleaseLock();

      private:
        TerrainReleaseLock(std::filesystem::path path, intptr_t handle, bool removeOnClose);
        void Release();

        std::filesystem::path mPath;
        intptr_t mHandle = -1; // a HANDLE on Windows, a file descriptor elsewhere
        bool mRemoveOnClose = false;
    };
}
