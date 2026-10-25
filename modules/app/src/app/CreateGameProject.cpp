/**
 * @file CreateGameProject.cpp
 * @author sumin.park
 * @brief Writes a new game directory from the editor's New Project dialog.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "CreateGameProject.h"

#include <core/log/Log.h>
#include <project/GameProject.h>
#include <renderer/RenderSettings.h>
#include <scene/SceneIO.h>
#include <serial/JsonFile.h>

#include <filesystem>
#include <system_error>

namespace mir
{
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
