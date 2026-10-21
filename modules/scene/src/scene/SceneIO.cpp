/**
 * @file SceneIO.cpp
 * @author Sumin Park
 * @brief Save and load a scene as a manifest plus one file per entity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneIO.h"

#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "EntityCodec.h"

#include <serial/JsonFile.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        constexpr std::string_view kEntitiesDirName = "entities";

        fs::path EntityFilePath(const fs::path &sceneDir, StableId id)
        {
            return sceneDir / kEntitiesDirName / (std::to_string(id) + ".json");
        }

        std::optional<StableId> EntityFileId(const fs::path &file)
        {
            if (file.extension() != ".json")
                return std::nullopt;

            const std::string stem = file.stem().string();
            StableId id = kNullStableId;
            const auto [end, err] = std::from_chars(stem.data(), stem.data() + stem.size(), id);
            if (err != std::errc{} || end != stem.data() + stem.size())
                return std::nullopt;
            return id;
        }

        void RemoveStaleEntityFiles(const fs::path &sceneDir, const LoadedScene &scene)
        {
            std::vector<fs::path> stale;
            std::error_code ec;
            for (fs::directory_iterator it(sceneDir / kEntitiesDirName, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::optional<StableId> id = EntityFileId(it->path());
                if (id && !scene.mEntities.contains(*id))
                    stale.push_back(it->path());
            }

            for (const fs::path &file : stale)
            {
                std::error_code removeEc;
                fs::remove(file, removeEc);
                if (removeEc)
                    MIR_LOG_WARN("SaveScene: could not remove stale '{}': {}", file.string(), removeEc.message());
            }
        }

        struct SceneManifest
        {
            std::string mName;
            StableId mNextId = 1;
            std::vector<StableId> mEntities;
        };

        // ProbeScene and LoadScene both read through this, so whatever the probe passes the load can read
        std::optional<SceneManifest> ReadManifest(const fs::path &sceneDir, std::string &error)
        {
            const std::optional<json> manifest = ReadJsonFile(SceneManifestPath(sceneDir), &error);
            if (!manifest)
                return std::nullopt;
            if (!manifest->is_object())
            {
                error = "the top level is not an object";
                return std::nullopt;
            }

            SceneManifest read;
            if (const auto name = manifest->find("name"); name != manifest->end())
            {
                if (!name->is_string())
                {
                    error = "\"name\" is not a string";
                    return std::nullopt;
                }
                read.mName = name->get<std::string>();
            }
            if (const auto nextId = manifest->find("nextId"); nextId != manifest->end())
            {
                if (!nextId->is_number_unsigned())
                {
                    error = "\"nextId\" is not an entity id";
                    return std::nullopt;
                }
                read.mNextId = nextId->get<StableId>();
            }

            const json entities = manifest->value("entities", json::array());
            if (!entities.is_array())
            {
                error = "\"entities\" is not an array";
                return std::nullopt;
            }
            for (const json &idJson : entities)
            {
                if (!idJson.is_number_unsigned())
                {
                    error = "\"entities\" holds something other than an entity id";
                    return std::nullopt;
                }
                read.mEntities.push_back(idJson.get<StableId>());
            }
            return read;
        }

        std::optional<json> ReadEntityFile(const fs::path &sceneDir, StableId id)
        {
            std::optional<json> entity = ReadJsonFile(EntityFilePath(sceneDir, id));
            if (!entity || !entity->is_object())
                return std::nullopt;
            return entity;
        }
    }

    fs::path SceneManifestPath(const fs::path &sceneDir)
    {
        return sceneDir / kSceneManifestFileName;
    }

    bool SaveScene(World &world, const fs::path &sceneDir, const LoadedScene &scene)
    {
        std::error_code ec;
        fs::create_directories(sceneDir / kEntitiesDirName, ec);
        if (ec)
        {
            MIR_LOG_ERROR("SaveScene: could not create '{}': {}", sceneDir.string(), ec.message());
            return false;
        }

        EntityIdMap ids;
        ids.reserve(scene.mEntities.size());
        for (const auto &[id, entity] : scene.mEntities)
            ids[PackEntity(entity)] = id;

        for (const auto &[id, entity] : scene.mEntities)
        {
            // a file outlives this run, so a reference outside the scene is written null
            if (!WriteJsonFile(EntityFilePath(sceneDir, id), EntityToJson(world, entity, id, ids, false)))
            {
                MIR_LOG_ERROR("SaveScene: could not write entity file for id {}", id);
                return false;
            }
        }

        // The manifest is written last
        json manifest;
        manifest["name"] = scene.mName;
        manifest["nextId"] = scene.mNextId;
        manifest["entities"] = json::array();
        for (const auto &[id, entity] : scene.mEntities)
            manifest["entities"].push_back(id);

        if (!WriteJsonFile(SceneManifestPath(sceneDir), manifest))
        {
            MIR_LOG_ERROR("SaveScene: could not write {} in '{}'", kSceneManifestFileName, sceneDir.string());
            return false;
        }

        RemoveStaleEntityFiles(sceneDir, scene);
        return true;
    }

    SceneProbe ProbeScene(const fs::path &sceneDir)
    {
        SceneProbe probe;

        const std::optional<SceneManifest> manifest = ReadManifest(sceneDir, probe.mManifestError);
        if (!manifest)
            return probe;

        for (const StableId id : manifest->mEntities)
        {
            if (!ReadEntityFile(sceneDir, id))
                probe.mMissingEntities.push_back(id);
        }

        return probe;
    }

    LoadedScene LoadScene(World &world, const fs::path &sceneDir)
    {
        LoadedScene loaded;

        std::string error;
        const std::optional<SceneManifest> manifest = ReadManifest(sceneDir, error);
        if (!manifest)
        {
            MIR_LOG_ERROR("LoadScene: '{}' has no usable {} ({}), loading an empty scene", sceneDir.string(),
                kSceneManifestFileName, error);
            return loaded;
        }
        loaded.mName = manifest->mName;
        loaded.mNextId = manifest->mNextId;

        std::vector<std::pair<StableId, json>> entities;
        for (const StableId id : manifest->mEntities)
        {
            std::optional<json> entityJson = ReadEntityFile(sceneDir, id);
            if (!entityJson)
            {
                MIR_LOG_ERROR("LoadScene: entity file for id {} listed in manifest but missing or unreadable", id);
                continue; // 0030 leaves this failure mode open; skip rather than abort the load
            }
            entities.emplace_back(id, std::move(*entityJson));
        }

        loaded.mEntities = SpawnEntities(world, entities, false);

        // a hand-edited manifest may carry a nextId that doesn't clear every id
        // present
        if (!loaded.mEntities.empty())
            loaded.mNextId = std::max(loaded.mNextId, loaded.mEntities.rbegin()->first + 1);

        return loaded;
    }

    void UnloadScene(World &world, const LoadedScene &loaded)
    {
        for (const auto &[id, entity] : loaded.mEntities)
        {
            // destroying a parent cascades, so a child reached later may
            // already be dead
            if (world.IsAlive(entity))
                world.DestroyEntity(entity);
        }
    }
}
