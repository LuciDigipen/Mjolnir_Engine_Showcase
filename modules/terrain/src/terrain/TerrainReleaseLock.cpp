/**
 * @file TerrainReleaseLock.cpp
 * @author Jackey Cen
 * @brief Shares one release's reader lock between a reader and a pruner.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainReleaseLock.h"

#include <terrain/TerrainRelease.h>

#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;

        // A read-only tree accepts no lock file; the caller opens the release without one.
        void MakeLockDir(const fs::path &assetDir)
        {
            std::error_code ec;
            fs::create_directories(TerrainReleaseLockDir(assetDir), ec);
        }
    }

    std::filesystem::path TerrainReleaseLockDir(const std::filesystem::path &assetDir)
    {
        return assetDir / kTerrainReleaseLocksDirName;
    }

    std::filesystem::path TerrainReleaseLockPath(const std::filesystem::path &assetDir, TerrainReleaseId release)
    {
        return TerrainReleaseLockDir(assetDir) / (TerrainIdHex(release.value) + ".lock");
    }

    TerrainReleaseLock::TerrainReleaseLock(std::filesystem::path path, intptr_t handle, bool removeOnClose)
        : mPath(std::move(path)), mHandle(handle), mRemoveOnClose(removeOnClose)
    {
    }

    TerrainReleaseLock::~TerrainReleaseLock()
    {
        Release();
    }

    void TerrainReleaseLock::Release()
    {
        if (mHandle == -1)
            return;
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(mHandle));
#else
        if (mRemoveOnClose)
            unlink(mPath.c_str());
        close(static_cast<int>(mHandle));
#endif
        mHandle = -1;
    }

    TerrainReleaseLockStatus TerrainReleaseLock::AcquireShared(const fs::path &assetDir, TerrainReleaseId release,
                                                               std::shared_ptr<const TerrainReleaseLock> &lock)
    {
        using Status = TerrainReleaseLockStatus;
        MakeLockDir(assetDir);
        const fs::path path = TerrainReleaseLockPath(assetDir, release);
#ifdef _WIN32
        // Readers share read access; a pruner asks for write access and shares nothing. Delete sharing lets a test
        // or a hand remove the asset directory under a live handle.
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            lock = std::shared_ptr<const TerrainReleaseLock>(
                new TerrainReleaseLock(path, reinterpret_cast<intptr_t>(file), false));
            return Status::Held;
        }
        return GetLastError() == ERROR_SHARING_VIOLATION ? Status::Busy : Status::Unavailable;
#else
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0 && flock(fd, LOCK_SH | LOCK_NB) == 0)
        {
            lock = std::shared_ptr<const TerrainReleaseLock>(new TerrainReleaseLock(path, fd, false));
            return Status::Held;
        }
        const bool busy = fd >= 0 && errno == EWOULDBLOCK;
        if (fd >= 0)
            close(fd);
        return busy ? Status::Busy : Status::Unavailable;
#endif
    }

    TerrainReleaseLockStatus TerrainReleaseLock::TryAcquireExclusive(const fs::path &assetDir,
                                                                     TerrainReleaseId release,
                                                                     std::unique_ptr<TerrainReleaseLock> &lock)
    {
        using Status = TerrainReleaseLockStatus;
        MakeLockDir(assetDir);
        const fs::path path = TerrainReleaseLockPath(assetDir, release);
#ifdef _WIN32
        // Closed by the last handle, which also removes the file.
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            lock = std::unique_ptr<TerrainReleaseLock>(
                new TerrainReleaseLock(path, reinterpret_cast<intptr_t>(file), false));
            return Status::Held;
        }
        return GetLastError() == ERROR_SHARING_VIOLATION ? Status::Busy : Status::Unavailable;
#else
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            lock = std::unique_ptr<TerrainReleaseLock>(new TerrainReleaseLock(path, fd, true));
            return Status::Held;
        }
        const bool busy = fd >= 0 && errno == EWOULDBLOCK;
        if (fd >= 0)
            close(fd);
        return busy ? Status::Busy : Status::Unavailable;
#endif
    }
}
