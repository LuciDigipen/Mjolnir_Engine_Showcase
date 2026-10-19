/**
 * @file PostProcessingBindings.cpp
 * @author Rahul Nair
 * @brief The Lua post-processing surface: world:set_fog/get_fog, set_hdr/get_hdr,
 *        set_bloom/get_bloom - one global stack, see PostProcessingBindings.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "PostProcessingBindings.h"

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <renderer/BloomEffect.h>
#include <renderer/FogEffect.h>
#include <renderer/HdrEffect.h>
#include <renderer/PostProcessingSettings.h>

#include <glm/vec3.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <string>

namespace mir
{
    namespace
    {
        PostProcessingSettings *ResolveSettings(World &world, std::string_view binding)
        {
            PostProcessingSettings *settings = world.TryResource<PostProcessingSettings>();
            if (settings == nullptr)
                MIR_LOG_ERROR("script: world:{} - this world has no PostProcessingSettings", binding);
            return settings;
        }

        bool GetOptionalBool(const sol::table &t, const char *key, std::string_view binding, bool &value)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            if (!obj.is<bool>())
            {
                MIR_LOG_ERROR("script: world:{} - '{}' must be a boolean", binding, key);
                return false;
            }
            value = obj.as<bool>();
            return true;
        }

        bool GetOptionalFloat(const sol::table &t, const char *key, std::string_view binding, float &value)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            if (obj.get_type() != sol::type::number)
            {
                MIR_LOG_ERROR("script: world:{} - '{}' must be a number", binding, key);
                return false;
            }
            value = obj.as<float>();
            return true;
        }

        bool GetOptionalVec3(const sol::table &t, const char *key, std::string_view binding, glm::vec3 &value)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            if (!obj.is<sol::table>())
            {
                MIR_LOG_ERROR("script: world:{} - '{}' must be a table {{x, y, z}}", binding, key);
                return false;
            }
            const sol::table c = obj.as<sol::table>();
            value = glm::vec3(c.get_or("x", value.x), c.get_or("y", value.y), c.get_or("z", value.z));
            return true;
        }

        bool GetOptionalTonemapOperator(const sol::table &t, const char *key, std::string_view binding,
            TonemapOperator &value)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            if (!obj.is<std::string>())
            {
                MIR_LOG_ERROR("script: world:{} - '{}' must be a string (\"none\", \"reinhard\" or \"aces\")",
                    binding, key);
                return false;
            }

            const std::string name = obj.as<std::string>();
            const TonemapOperator op = ParseOperator(name);
            if (ToString(op) != name)
            {
                MIR_LOG_ERROR("script: world:{} - '{}' unknown tonemap operator '{}'", binding, key, name);
                return false;
            }

            value = op;
            return true;
        }

        sol::table Vec3ToLua(sol::state_view lua, const glm::vec3 &v)
        {
            sol::table out = lua.create_table();
            out["x"] = v.x;
            out["y"] = v.y;
            out["z"] = v.z;
            return out;
        }

        bool WorldSetFog(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_fog");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kFogEffectName)];
            FogParams fog = ReadFogParams(state.params);
            bool enabled = state.enabled;

            if (!GetOptionalBool(args, "enabled", "set_fog", enabled) ||
                !GetOptionalVec3(args, "color", "set_fog", fog.color) ||
                !GetOptionalFloat(args, "density", "set_fog", fog.density) ||
                !GetOptionalFloat(args, "startDistance", "set_fog", fog.start) ||
                !GetOptionalFloat(args, "endDistance", "set_fog", fog.end))
                return false;

            fog.density = std::max(fog.density, 0.0f);
            fog.start = std::max(fog.start, 0.0f);
            fog.end = std::max(fog.end, fog.start); // same invariant the panel enforces

            state.enabled = enabled;
            WriteFogParams(state.params, fog);
            return true;
        }

        sol::object WorldGetFog(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_fog");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kFogEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const FogParams fog = it != settings->effects.end() ? ReadFogParams(it->second.params) : FogParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["color"] = Vec3ToLua(lua, fog.color);
            out["density"] = fog.density;
            out["startDistance"] = fog.start;
            out["endDistance"] = fog.end;
            return out;
        }

        bool WorldSetHdr(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_hdr");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kHdrEffectName)];
            HdrParams hdr = ReadHdrParams(state.params);
            bool enabled = state.enabled;

            if (!GetOptionalBool(args, "enabled", "set_hdr", enabled) ||
                !GetOptionalFloat(args, "exposure", "set_hdr", hdr.exposure) ||
                !GetOptionalTonemapOperator(args, "tonemapOperator", "set_hdr", hdr.op))
                return false;

            hdr.exposure = std::max(hdr.exposure, 0.0f);

            state.enabled = enabled;
            WriteHdrParams(state.params, hdr);
            return true;
        }

        sol::object WorldGetHdr(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_hdr");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kHdrEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const HdrParams hdr = it != settings->effects.end() ? ReadHdrParams(it->second.params) : HdrParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["exposure"] = hdr.exposure;
            out["tonemapOperator"] = std::string(ToString(hdr.op));
            return out;
        }

        bool WorldSetBloom(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_bloom");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kBloomEffectName)];
            BloomParams bloom = ReadBloomParams(state.params);
            bool enabled = state.enabled;

            if (!GetOptionalBool(args, "enabled", "set_bloom", enabled) ||
                !GetOptionalFloat(args, "threshold", "set_bloom", bloom.threshold) ||
                !GetOptionalFloat(args, "intensity", "set_bloom", bloom.intensity) ||
                !GetOptionalFloat(args, "radius", "set_bloom", bloom.radius))
                return false;

            bloom.threshold = std::max(bloom.threshold, 0.0f);
            bloom.intensity = std::max(bloom.intensity, 0.0f);
            bloom.radius = std::max(bloom.radius, 0.0f);

            state.enabled = enabled;
            WriteBloomParams(state.params, bloom);
            return true;
        }

        sol::object WorldGetBloom(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_bloom");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kBloomEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const BloomParams bloom =
                it != settings->effects.end() ? ReadBloomParams(it->second.params) : BloomParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["threshold"] = bloom.threshold;
            out["intensity"] = bloom.intensity;
            out["radius"] = bloom.radius;
            return out;
        }
    }

    void RegisterPostProcessingBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("set_fog", &WorldSetFog);
        worldType.set_function("get_fog", &WorldGetFog);
        worldType.set_function("set_hdr", &WorldSetHdr);
        worldType.set_function("get_hdr", &WorldGetHdr);
        worldType.set_function("set_bloom", &WorldSetBloom);
        worldType.set_function("get_bloom", &WorldGetBloom);
    }
}
