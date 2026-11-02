/**
 * @file CookedMeshDir.h
 * @author sumin.park
 * @brief A temporary cooked directory holding raw JSON mesh blobs, for tests of the async mesh path.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>
#include <assets/AssetManifest.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir::test
{
    inline constexpr std::string_view kTriangle = R"({
        "vertices": [
            { "position": [0, 0, 0], "color": [1, 1, 1], "normal": [0, 1, 0] },
            { "position": [1, 0, 0], "color": [1, 1, 1], "normal": [0, 1, 0] },
            { "position": [1, 1, 0], "color": [1, 1, 1], "normal": [0, 1, 0] }
        ],
        "indices": [0, 1, 2]
    })";

    struct CookedMesh
    {
        // the logical path the manifest lists, so it may hold slashes
        std::string name;
        std::string json;
    };

    // A cooked directory removed afterwards. Cook writes the blobs and the manifest, and a directory that
    // was never cooked has no manifest, like a game before its first import.
    class CookedDir
    {
    public:
        CookedDir()
        {
            static std::atomic<int> counter{ 0 };
            mDir = std::filesystem::temp_directory_path() /
                ("mir_mesh_cooked_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            std::filesystem::remove_all(mDir, ec);
            std::filesystem::create_directories(mDir, ec);
        }

        explicit CookedDir(const std::vector<CookedMesh> &meshes) : CookedDir() { Cook(meshes); }

        ~CookedDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mDir, ec);
        }

        CookedDir(const CookedDir &) = delete;
        CookedDir &operator=(const CookedDir &) = delete;

        void Cook(const std::vector<CookedMesh> &meshes)
        {
            std::vector<std::string> files;
            files.reserve(meshes.size());
            std::vector<AssetManifestSourceEntry> entries;
            for (const CookedMesh &mesh : meshes)
            {
                std::string file = mesh.name + ".blob";
                std::ranges::replace(file, '/', '_');
                files.push_back(std::move(file));
                REQUIRE(WriteBytes(mDir / files.back(),
                    BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, std::as_bytes(std::span(mesh.json)))));
                entries.push_back({ { MakeAssetId(mesh.name), kRawAssetTypeTag, kRawAssetContentVersion }, files.back(),
                    mesh.name, {} });
            }
            REQUIRE(WriteBytes(mDir / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }

        const std::filesystem::path &Path() const { return mDir; }

    private:
        std::filesystem::path mDir;
    };
}
