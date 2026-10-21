/**
 * @file GameProject.cpp
 * @author Jackey Cen
 * @brief A game directory resolved into what the engine loads from it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameProject.h>

#include <assets/AssetPath.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <renderer/RenderSettings.h>
#include <scene/SceneIO.h>

#include <serial/JsonFile.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        // Where games/ lives. Normally that is next to the executable, beside
        // the copies CMake made. A development build also writes an
        // asset_workspace.json there pointing back at the source tree, so the
        // editor's import workflow edits the real game data instead of a copy.
        std::filesystem::path ProjectRoot()
        {
            const std::optional<nlohmann::json> binding =
                ReadJsonFile(ExecutableDir() / "asset_workspace.json");

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
    } // namespace

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
                "Set defaultGame in engine.json (e.g. games/HelloWorld), or pass --game <dir>.");
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
                "Name a game under " + (rootDir / "games").string() + ", e.g. games/HelloWorld.");
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

        // assetsRoot default must match CMakeLists.txt
        std::string assetsRoot = "assets";
        std::string sceneDir = "scenes/default";
        std::string soundBanksRoot = "assets/audio/SoundBanks";

        // value() throws on a key of the wrong type; say which file did it
        try
        {
            project.mName = json->value("name", project.mName);
            project.mTitle = json->value("title", project.mTitle);
            assetsRoot = json->value("assetsRoot", assetsRoot);
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

        // The same normalizer AssetCooker applies to its --source roots, so ids match
        AssetPathResult idRoot = MakeLogicalAssetPath(gameDir.generic_string(), assetsRoot);
        if (!idRoot.Ok())
        {
            throw SetupError(ErrorCategory::ProjectData, "game.json has an unusable assetsRoot '" + assetsRoot + "'",
                manifestPath, std::string(Describe(idRoot.error)),
                "Make assetsRoot a relative path inside the game directory, e.g. assets.");
        }

        project.mAssetIdRoot = std::move(idRoot.path);
        project.mRootDir = root;
        project.mAssetsDir = (ProjectRoot() / project.mAssetIdRoot).lexically_normal();
        project.mSceneDir = (root / sceneDir).lexically_normal();
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

    bool CreateGameProject(const std::string &name, bool useRayTracing)
    {
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "." || name == "..")
        {
            MIR_LOG_ERROR("CreateGameProject: '{}' is not a valid project name", name);
            return false;
        }

        const std::filesystem::path root = ProjectRoot() / "games" / name;

        std::error_code ec;
        if (std::filesystem::exists(root, ec))
        {
            MIR_LOG_ERROR("CreateGameProject: 'games/{}' already exists", name);
            return false;
        }

        const std::filesystem::path sceneDir = root / "scenes" / "default";
        if (!std::filesystem::create_directories(sceneDir, ec) ||
            !std::filesystem::create_directories(root / "assets", ec))
        {
            MIR_LOG_ERROR("CreateGameProject: could not create 'games/{}': {}", name, ec.message());
            return false;
        }

        const nlohmann::json manifest{ { "name", name },{ "title", "MjolnirEngine - " + name } };

        // same shape as games/EmptyScene/scenes/default/scene.json
        const nlohmann::json scene{ { "entities", nlohmann::json::array() },{ "name", "default" },{ "nextId", 1 } };

        if (!WriteJsonFile(root / kGameManifestFileName, manifest) ||
            !WriteJsonFile(sceneDir / kSceneManifestFileName, scene))
        {
            MIR_LOG_ERROR("CreateGameProject: could not write 'games/{}' manifest/scene", name);
            return false;
        }

        return RenderSettings{ .rayTracing = useRayTracing }.SaveFile(root / kProjectSettingsFileName);
    }
}
