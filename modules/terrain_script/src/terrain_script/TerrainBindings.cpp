/**
 * @file TerrainBindings.cpp
 * @author Jackey Cen
 * @brief Exposes resident terrain height results and metadata to Lua.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain_script/TerrainBindings.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <terrain/TerrainScene.h>

#include <sol/sol.hpp>

#include <cmath>
#include <format>
#include <string_view>

namespace mir
{
    namespace
    {
        std::string_view StatusName(TerrainHeightStatus status)
        {
            switch (status)
            {
            case TerrainHeightStatus::MeetsRequirement:
                return "MeetsRequirement";
            case TerrainHeightStatus::CoarserWhileLoading:
                return "CoarserWhileLoading";
            case TerrainHeightStatus::CoarserAtSourceLimit:
                return "CoarserAtSourceLimit";
            case TerrainHeightStatus::Loading:
                return "Loading";
            case TerrainHeightStatus::OutsideMap:
                return "OutsideMap";
            case TerrainHeightStatus::Failed:
                return "Failed";
            }
            return "Failed";
        }

        std::string_view FailureName(TerrainHeightFailure failure)
        {
            switch (failure)
            {
            case TerrainHeightFailure::None:
                return "None";
            case TerrainHeightFailure::WrongRelease:
                return "WrongRelease";
            case TerrainHeightFailure::InvalidInput:
                return "InvalidInput";
            case TerrainHeightFailure::LoadFailed:
                return "LoadFailed";
            case TerrainHeightFailure::InvalidSamples:
                return "InvalidSamples";
            }
            return "InvalidInput";
        }

        std::string_view WeightsStatusName(TerrainLayerWeightsStatus status)
        {
            switch (status)
            {
            case TerrainLayerWeightsStatus::Ready:
                return "ready";
            case TerrainLayerWeightsStatus::Loading:
                return "loading";
            case TerrainLayerWeightsStatus::OutsideMap:
                return "outside_map";
            case TerrainLayerWeightsStatus::NoWeights:
                return "no_weights";
            case TerrainLayerWeightsStatus::Failed:
                return "failed";
            }
            return "failed";
        }

        std::string_view WeightsFailureName(TerrainLayerWeightsFailure failure)
        {
            switch (failure)
            {
            case TerrainLayerWeightsFailure::None:
                return "None";
            case TerrainLayerWeightsFailure::WrongRelease:
                return "WrongRelease";
            case TerrainLayerWeightsFailure::InvalidInput:
                return "InvalidInput";
            case TerrainLayerWeightsFailure::LoadFailed:
                return "LoadFailed";
            }
            return "InvalidInput";
        }

        sol::table ResultToLua(sol::state_view lua, const TerrainHeightResult &result)
        {
            sol::table out = lua.create_table();
            out["status"] = StatusName(result.status);
            out["failure"] = FailureName(result.failure);
            out["hasHeight"] = result.HasHeight();
            if (result.HasHeight())
                out["localHeight"] = result.localHeight;
            if (result.worldPoint)
            {
                const WorldVec3 meters = result.worldPoint->Meters();
                sol::table point = lua.create_table();
                point["x"] = meters.x;
                point["y"] = meters.y;
                point["z"] = meters.z;
                out["worldPoint"] = point;
                out["worldHeight"] = meters.y;
            }
            if (std::isfinite(result.spacing))
                out["spacing"] = result.spacing;
            if (std::isfinite(result.sourceSpacing))
                out["sourceSpacing"] = result.sourceSpacing;
            if (result.maxError)
                out["maxError"] = *result.maxError;
            out["releaseId"] = std::format("{:016x}", result.release.release.value);
            sol::table release = lua.create_table();
            release["assetId"] = std::format("{:016x}", result.release.asset.value);
            release["sourceFingerprint"] = std::format("{:016x}", result.release.source.value);
            release["formatVersion"] = result.release.format.value;
            release["releaseId"] = std::format("{:016x}", result.release.release.value);
            out["release"] = release;
            sol::table tile = lua.create_table();
            tile["x"] = result.tile.x;
            tile["z"] = result.tile.z;
            out["tile"] = tile;
            out["layer"] = result.layer;
            return out;
        }

        sol::table WeightsResultToLua(sol::state_view lua, const TerrainLayerWeightsResult &result)
        {
            sol::table out = lua.create_table();
            out["status"] = WeightsStatusName(result.status);
            out["failure"] = WeightsFailureName(result.failure);
            sol::table layers = lua.create_table();
            for (size_t i = 0; i < result.layers.size(); ++i)
            {
                sol::table layer = lua.create_table();
                layer["name"] = result.layers[i].name;
                layer["weight"] = result.layers[i].weight;
                layers[i + 1] = layer;
            }
            out["layers"] = layers;
            out["releaseId"] = std::format("{:016x}", result.release.release.value);
            sol::table release = lua.create_table();
            release["assetId"] = std::format("{:016x}", result.release.asset.value);
            release["sourceFingerprint"] = std::format("{:016x}", result.release.source.value);
            release["formatVersion"] = result.release.format.value;
            release["releaseId"] = std::format("{:016x}", result.release.release.value);
            out["release"] = release;
            sol::table tile = lua.create_table();
            tile["x"] = result.tile.x;
            tile["z"] = result.tile.z;
            out["tile"] = tile;
            return out;
        }

        bool Number(const sol::object &object, double &number)
        {
            if (object.get_type() != sol::type::number)
                return false;
            number = object.as<double>();
            return std::isfinite(number);
        }

        sol::table WorldTerrainHeightAt(World &world, sol::variadic_args args, sol::this_state state)
        {
            TerrainHeightResult result;
            double spacing = 0.0;
            glm::dvec3 meters{};
            bool valid = args.size() == 2;
            if (valid)
            {
                const sol::object position = args[0].get<sol::object>();
                valid = position.get_type() == sol::type::table;
                if (valid)
                {
                    const sol::table table = position.as<sol::table>();
                    valid = Number(table.raw_get<sol::object>("x"), meters.x) &&
                            Number(table.raw_get<sol::object>("y"), meters.y) &&
                            Number(table.raw_get<sol::object>("z"), meters.z);
                }
                valid = valid && Number(args[1].get<sol::object>(), spacing) && spacing > 0.0;
            }
            const auto position = valid ? WorldPosition::FromDoubleMeters(meters) : std::nullopt;
            if (!position)
            {
                MIR_LOG_ERROR("script: world:terrain_height_at needs finite x, y, z and positive finite spacing");
                result.failure = TerrainHeightFailure::InvalidInput;
            }
            else if (TerrainWorld *terrain = world.TryResource<TerrainWorld>())
            {
                result = terrain->HeightAt(*position, spacing);
            }
            else
            {
                result.status = TerrainHeightStatus::OutsideMap;
            }
            return ResultToLua(sol::state_view(state), result);
        }

        sol::table WorldTerrainLayerWeightsAt(World &world, sol::variadic_args args, sol::this_state state)
        {
            TerrainLayerWeightsResult result;
            glm::dvec3 meters{};
            bool valid = args.size() == 1;
            if (valid)
            {
                const sol::object position = args[0].get<sol::object>();
                valid = position.get_type() == sol::type::table;
                if (valid)
                {
                    const sol::table table = position.as<sol::table>();
                    valid = Number(table.raw_get<sol::object>("x"), meters.x) &&
                            Number(table.raw_get<sol::object>("y"), meters.y) &&
                            Number(table.raw_get<sol::object>("z"), meters.z);
                }
            }
            const auto position = valid ? WorldPosition::FromDoubleMeters(meters) : std::nullopt;
            if (!position)
            {
                MIR_LOG_ERROR("script: world:terrain_layer_weights_at needs finite x, y, z");
                result.failure = TerrainLayerWeightsFailure::InvalidInput;
            }
            else if (TerrainWorld *terrain = world.TryResource<TerrainWorld>())
            {
                result = terrain->LayerWeightsAt(*position);
            }
            else
            {
                result.status = TerrainLayerWeightsStatus::OutsideMap;
            }
            return WeightsResultToLua(sol::state_view(state), result);
        }
    }

    void RegisterTerrainBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("terrain_height_at", &WorldTerrainHeightAt);
        worldType.set_function("terrain_layer_weights_at", &WorldTerrainLayerWeightsAt);
    }
}
