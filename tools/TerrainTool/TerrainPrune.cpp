/**
 * @file TerrainPrune.cpp
 * @author Jackey Cen
 * @brief Removes an asset's old releases that no running reader holds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainPrune.h"

#include "TerrainPublish.h"

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainReleaseLock.h>

#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        void Fail(TerrainReport &report, std::string_view check, std::string field, std::string message)
        {
            report.Add({std::string(check), std::move(field), {}, std::nullopt, std::move(message)});
        }

        std::string Join(const std::vector<std::string> &items)
        {
            std::string text;
            for (const std::string &item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text;
        }

        bool IsPartialReleaseDir(const fs::directory_entry &entry)
        {
            const std::string name = PathUtf8(entry.path().filename());
            std::error_code ec;
            return entry.is_directory(ec) && name.size() == 16 + 8 && name.ends_with(".partial") &&
                   ParseTerrainIdHex(std::string_view(name).substr(0, 16));
        }

        // Staging directories are provably stale: only the publish lock holder writes in an asset directory.
        size_t RemovePartialDirs(const fs::path &assetDir)
        {
            size_t removed = 0;
            std::error_code ec;
            for (const fs::directory_entry &entry : fs::directory_iterator(assetDir, ec))
            {
                if (!IsPartialReleaseDir(entry))
                    continue;
                std::error_code removeEc;
                fs::remove_all(entry.path(), removeEc);
                removed += removeEc ? 0 : 1;
            }
            return removed;
        }
    }

    void PruneTerrainReleases(const std::filesystem::path &releasesRoot, AssetId asset, TerrainReport &report)
    {
        const fs::path assetDir = TerrainAssetReleasesDir(releasesRoot, asset);
        const std::optional<TerrainReleaseId> current = PeekTerrainCurrent(releasesRoot, asset);
        if (!current)
        {
            report.Ran(check::kPrune, "no current release, so nothing was removed");
            return;
        }

        const std::string currentHex = TerrainIdHex(current->value);
        std::vector<std::string> deleted;
        std::vector<std::string> kept;
        uint64_t freed = 0;
        for (const TerrainStoredRelease &stored : ListTerrainReleases(releasesRoot, asset))
        {
            const std::string hex = TerrainIdHex(stored.release.value);
            if (stored.release == *current)
            {
                kept.push_back(hex + " (current)");
                continue;
            }
            std::unique_ptr<TerrainReleaseLock> lock;
            const TerrainReleaseLockStatus status =
                TerrainReleaseLock::TryAcquireExclusive(assetDir, stored.release, lock);
            if (status != TerrainReleaseLockStatus::Held)
            {
                kept.push_back(status == TerrainReleaseLockStatus::Busy ? hex + " (in use)" : hex + " (no lock file)");
                continue;
            }
            std::error_code ec;
            fs::remove_all(TerrainReleaseDir(releasesRoot, asset, stored.release), ec);
            if (ec)
                kept.push_back(std::format("{} ({})", hex, ec.message()));
            else
            {
                deleted.push_back(hex);
                freed += stored.bytes;
            }
        }
        const size_t partials = RemovePartialDirs(assetDir);

        std::string detail = std::format("current {}; deleted {}", currentHex,
                                         deleted.empty() ? std::string("nothing") : Join(deleted));
        if (!kept.empty())
            detail += "; kept " + Join(kept);
        detail += std::format("; freed {} bytes", freed);
        if (partials != 0)
            detail += std::format("; removed {} stale .partial directories", partials);
        report.Ran(check::kPrune, std::move(detail));
    }

    TerrainReport RunTerrainPrune(const TerrainPruneRequest &request)
    {
        TerrainReport report("prune", {check::kArguments, check::kLock, check::kPrune});
        const fs::path releases = request.gameDir.empty() ? fs::path() : TerrainReleasesRoot(request.gameDir);

        const TerrainAssetPath asset = ResolveTerrainAssetPath(request.idRoot, request.sourcePath);
        if (!asset.Ok())
            Fail(report, check::kArguments,
                 asset.error == TerrainAssetPathError::IdRoot ? "--id-root" : "--source-path",
                 DescribeTerrainAssetPathError(asset, request.idRoot, request.sourcePath));
        if (request.gameDir.empty())
            Fail(report, check::kArguments, "--game-dir", "no game directory");
        if (report.Status(check::kArguments) == CheckStatus::Failed)
            return report;
        report.assetPath = asset.logicalPath;
        report.Ran(check::kArguments, std::format("asset {} ({}), releases {}", asset.logicalPath,
                                                  TerrainIdHex(asset.asset.value), PathUtf8(releases)));

        std::string error;
        std::optional<TerrainPublishLock> lock =
            TerrainPublishLock::Acquire(TerrainAssetReleasesDir(releases, asset.asset), request.lockWait, error);
        if (!lock)
        {
            Fail(report, check::kLock, std::string(kTerrainPublishLockFileName), error);
            return report;
        }
        report.Ran(check::kLock, PathUtf8(lock->AssetDir() / kTerrainPublishLockFileName));

        PruneTerrainReleases(releases, asset.asset, report);
        return report;
    }
}
