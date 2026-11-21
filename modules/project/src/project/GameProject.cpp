/**
 * @file GameProject.cpp
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <project/GameProject.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>

#include <serial/JsonFile.h>

#include <algorithm>
#include <system_error>

namespace mir
{
    // Normally next to the executable, beside the copies CMake made. A development
    // build also writes an asset_workspace.json there pointing back at the source
    // tree, so the editor's import workflow edits the real game data instead of a copy.
    std::filesystem::path ProjectRoot()
    {
        const std::optional<nlohmann::json> binding = ReadJsonFile(ExecutableDir() / "asset_workspace.json");

        if (binding && binding->is_object() && binding->value("version", 0) == 1)
        {
            const std::filesystem::path source = binding->value("sourceRoot", "");
            std::error_code ec;
            if (std::filesystem::is_directory(source / "games", ec))
                // normalized so paths in messages read one way
                return source.lexically_normal();
        }

        return ExecutableDir();
    }

    std::string ReadDefaultGame()
    {
        const std::filesystem::path path = ExecutableDir() / kEngineManifestFileName;

        std::string error;
        const std::optional<nlohmann::json> json = ReadJsonFile(path, &error);
        if (!json)
        {
            throw SetupError(ErrorCategory::BuildOutput, "No --game given and engine.json could not be read", path,
                error, "Rebuild: the build copies engine.json next to the engine. Or pass --game <dir>.");
        }

        const auto it = json->is_object() ? json->find("defaultGame") : json->end();
        if (it == json->end() || !it->is_string() || it->get<std::string>().empty())
        {
            throw SetupError(ErrorCategory::ProjectData, "No --game given and engine.json names no defaultGame", path,
                "defaultGame is missing or not a non-empty string",
                "Set defaultGame in engine.json, or pass --game <dir>.");
        }

        return it->get<std::string>();
    }

    std::filesystem::path NormalizeGameDir(const std::string &requested, ErrorCategory notFound)
    {
        const std::filesystem::path rootDir = ProjectRoot();

        std::filesystem::path dir = std::filesystem::path(requested).lexically_normal();
        if (dir.is_absolute())
            dir = dir.lexically_relative(rootDir.lexically_normal());

        // lexically_normal keeps a trailing separator as an empty filename
        if (!dir.empty() && dir.filename().empty())
            dir = dir.parent_path();

        if (dir.empty() || dir == "." || dir.has_root_path() || *dir.begin() == "..")
        {
            throw SetupError(notFound, "Game directory '" + requested + "' is not inside the project root", rootDir,
                "games are looked up under the project root only",
                "Name a game under " + (rootDir / "games").string() + ".");
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(rootDir / dir, ec))
        {
            throw SetupError(notFound, "Game directory '" + requested + "' does not exist", rootDir / dir,
                "no such directory", "Name an existing game under " + (rootDir / "games").string() + ".");
        }

        return dir;
    }

    GameProject LoadGameProject(const std::filesystem::path &gameDir, ErrorCategory notFound)
    {
        const std::filesystem::path root = ProjectRoot() / gameDir;
        const std::filesystem::path manifestPath = root / kGameManifestFileName;

        std::error_code manifestEc;
        if (!std::filesystem::is_regular_file(manifestPath, manifestEc))
        {
            throw SetupError(notFound, "'" + gameDir.generic_string() + "' is not a game directory", manifestPath,
                "it has no game.json", "Name a directory that holds a game.json, or add one.");
        }

        std::string parseError;
        const std::optional<nlohmann::json> json = ReadJsonFile(manifestPath, &parseError);
        if (!json || !json->is_object())
        {
            throw SetupError(ErrorCategory::ProjectData, "game.json is not a valid JSON object", manifestPath,
                json ? "the top level is not an object" : parseError,
                "Fix the JSON in " + manifestPath.string() + ", or restore it from git.");
        }

        GameProject project;
        project.mDir = gameDir.generic_string();
        project.mName = gameDir.filename().string();

        std::string sceneDir = "scenes/default";
        std::string soundBanksRoot = "assets/audio/SoundBanks";

        // value() throws on a key of the wrong type; say which file did it
        try
        {
            project.mName = json->value("name", project.mName);
            project.mTitle = json->value("title", project.mTitle);
            // the default must match CMakeLists.txt
            project.mAssetsRoot = json->value("assetsRoot", project.mAssetsRoot);
            sceneDir = json->value("sceneDir", sceneDir);
            soundBanksRoot = json->value("soundBanksRoot", soundBanksRoot);
        }
        catch (const nlohmann::json::exception &e)
        {
            throw SetupError(ErrorCategory::ProjectData, "game.json has a key of the wrong type", manifestPath,
                e.what(), "Make name, title, assetsRoot, sceneDir and soundBanksRoot strings in " +
                manifestPath.string() + ".");
        }

        if (project.mName.empty())
            project.mName = gameDir.filename().string();

        if (const auto shell = json->find("shell"); shell != json->end())
        {
            const auto flag = [&](const char *key, bool &out)
            {
                const auto it = shell->find(key);
                if (it == shell->end())
                    return;
                if (!it->is_boolean())
                {
                    throw SetupError(ErrorCategory::ProjectData, "game.json has a key of the wrong type", manifestPath,
                        std::string("shell.") + key + " is not a boolean",
                        "Make shell.hud and shell.menu true or false in " + manifestPath.string() + ".");
                }
                out = it->get<bool>();
            };
            if (!shell->is_object())
            {
                throw SetupError(ErrorCategory::ProjectData, "game.json has a key of the wrong type", manifestPath,
                    "shell is not an object",
                    "Make shell an object such as { \"menu\": false } in " + manifestPath.string() + ".");
            }
            flag("hud", project.mShellHud);
            flag("menu", project.mShellMenu);
        }

        project.mRootDir = root;
        project.mAssetsDir = (root / project.mAssetsRoot).lexically_normal();
        project.mSceneDir = (root / sceneDir).lexically_normal();
        const auto invalidCatalog = [&](const std::string &cause) {
            throw SetupError(ErrorCategory::ProjectData, "Invalid scene catalog", manifestPath, cause,
                "Register scenes with unique IDs and game-relative paths, then select a startupScene ID.");
        };
        if (const auto scenes = json->find("scenes"); scenes != json->end())
        {
            const auto startup = json->find("startupScene");
            if (!scenes->is_array() || startup == json->end() || !startup->is_string())
                invalidCatalog("scenes must be an array and startupScene must be a string");
            project.mStartupSceneId = startup->get<std::string>();
            for (const auto &entry : *scenes)
            {
                if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                    !entry.contains("path") || !entry["path"].is_string())
                    invalidCatalog("each scene requires string id and path fields");

                const std::string id = entry["id"].get<std::string>();
                const auto relative = std::filesystem::u8path(entry["path"].get<std::string>()).lexically_normal();
                if (id.empty() || std::ranges::any_of(project.mScenes,
                        [&](const ProjectScene &scene) { return scene.mId == id; }))
                    invalidCatalog("empty or duplicate scene ID: " + id);
                if (relative.empty() || relative == "." || relative.has_root_path() || *relative.begin() == "..")
                    invalidCatalog("scene path must be inside the game folder: " + id);
                project.mScenes.push_back({ id, (root / relative).lexically_normal() });
            }
            const auto selected = std::ranges::find(project.mScenes, project.mStartupSceneId, &ProjectScene::mId);
            if (selected == project.mScenes.end())
                invalidCatalog("startupScene is not registered: " + project.mStartupSceneId);
            project.mSceneDir = selected->mDirectory;
        }
        else
        {
            if (json->contains("startupScene"))
                invalidCatalog("startupScene requires a scenes array");
            project.mScenes.push_back({ project.mStartupSceneId, project.mSceneDir });
        }
        project.mProjectSettingsPath = root / kProjectSettingsFileName;
        project.mSoundBankDir = (root / soundBanksRoot).lexically_normal();

        return project;
    }

    GameProject ResolveGameProject(const std::string &requested, ErrorCategory notFound)
    {
        const std::filesystem::path gameDir = NormalizeGameDir(requested, notFound);
        MIR_LOG_INFO("Loading game '{}'", gameDir.generic_string());
        return LoadGameProject(gameDir, notFound);
    }

    void EnsureGameDirectories(const GameProject &project)
    {
        for (const std::filesystem::path &dir :{ project.mAssetsDir, project.mSceneDir, project.mSoundBankDir })
        {
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec))
                continue;

            std::filesystem::create_directories(dir, ec);
            if (ec)
            {
                throw SetupError(ErrorCategory::System, "Could not create a game directory", dir, ec.message(),
                    "Check that " + dir.parent_path().string() + " is writable, or create the directory by hand.");
            }
            MIR_LOG_INFO("Created missing directory '{}'", dir.string());
        }
    }

    std::vector<GameProject> DiscoverGameProjects()
    {
        std::vector<GameProject> projects;

        const std::filesystem::path gamesDir = ProjectRoot() / "games";

        std::error_code ec;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(gamesDir, ec))
        {
            std::error_code entryEc;
            if (!entry.is_directory(entryEc) ||
                !std::filesystem::is_regular_file(entry.path() / kGameManifestFileName, entryEc))
                continue;

            // one broken game must not hide the others from the project menu
            try
            {
                projects.push_back(LoadGameProject(std::filesystem::path("games") / entry.path().filename()));
            }
            catch (const SetupError &error)
            {
                MIR_LOG_ERROR("DiscoverGameProjects: skipping '{}': {} ({})", entry.path().string(), error.What(),
                    error.Cause());
            }
        }

        if (ec)
            MIR_LOG_WARN("DiscoverGameProjects: could not list '{}': {}", gamesDir.string(), ec.message());

        std::sort(projects.begin(), projects.end(),
            [](const GameProject &a, const GameProject &b)
            {
                return a.mDir < b.mDir;
            });
        return projects;
    }
}
