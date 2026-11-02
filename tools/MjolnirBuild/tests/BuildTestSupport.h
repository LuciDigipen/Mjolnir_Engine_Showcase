/**
 * @file BuildTestSupport.h
 * @author Quy Dong Ta
 * @brief Temporary folders, a fake checkout and a fake player template for the MjolnirBuild tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <BuildOutput.h>

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace mir::build::test
{
    namespace fs = std::filesystem;

    // A fresh folder per test, removed when it ends
    struct Sandbox
    {
        fs::path mRoot;

        Sandbox()
        {
            static int sequence = 0;
            mRoot = fs::temp_directory_path() /
                ("mjolnir-build-tests-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(sequence++));
            fs::remove_all(mRoot);
            fs::create_directories(mRoot);
            mRoot = NormalizedPath(mRoot);
        }

        ~Sandbox()
        {
            std::error_code ec;
            fs::remove_all(mRoot, ec);
        }

        Sandbox(const Sandbox &) = delete;
        Sandbox &operator=(const Sandbox &) = delete;
    };

    inline void Touch(const fs::path &file, std::string_view text = "x")
    {
        fs::create_directories(file.parent_path());
        REQUIRE(WriteText(file, text));
    }

    inline void MakePlayerTemplate(const fs::path &dir, bool editor = false)
    {
        Touch(dir / "MjolnirEngine.exe");
        Touch(dir / "MjolnirErrorReporter.exe");
        Touch(dir / "player_info.json", editor ? "{ \"version\": 1, \"editor\": true }" : "{ \"version\": 1, \"editor\": false }");
        Touch(dir / "fonts" / "Inter.ttf");
    }

    // A checkout with one engine asset and two games: Demo has assets, sound banks, a Prefab
    // folder, a second scene and an IDE setting; Quiet has a scene and nothing else
    inline void MakeCheckout(const fs::path &root)
    {
        Touch(root / "assets" / "engine.txt");

        const fs::path demo = root / "games" / "Demo";
        Touch(demo / "game.json", R"({ "name": "Demo", "assetsRoot": "assets", "sceneDir": "scenes/default" })");
        Touch(demo / "project_settings.json", R"({ "ide": { "path": "C:/Code.exe" }, "rendering": { "rayTracing": true } })");
        Touch(demo / "terrain_streaming.json", "{}");
        Touch(demo / "terrain_streaming.local.json", "{}");
        Touch(demo / "scenes" / "default" / "scene.json", R"({ "entities": [] })");
        Touch(demo / "scenes" / "other" / "scene.json", R"({ "entities": [] })");
        Touch(demo / "assets" / "notes.txt");
        Touch(demo / "assets" / "prefabs" / "thing.txt");
        Touch(demo / "assets" / "audio" / "SoundBanks" / "Init.bnk");

        const fs::path quiet = root / "games" / "Quiet";
        Touch(quiet / "game.json", R"({ "name": "Quiet" })");
        Touch(quiet / "scenes" / "default" / "scene.json", R"({ "entities": [] })");
    }
}
