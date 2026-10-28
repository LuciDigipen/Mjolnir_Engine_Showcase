/**
 * @file PostProcessingBindings.cpp
 * @author Rahul Nair
 * @brief The Lua post-processing surface: world:set_fog/get_fog, set_hdr/get_hdr,
 *        set_bloom/get_bloom, set_depth_of_field/get_depth_of_field,
 *        set_film/get_film and set_motion_blur/get_motion_blur - one global
 *        stack, see PostProcessingBindings.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/PostProcessingBindings.h>

#include <script/BindingHelpers.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <renderer/BloomEffect.h>
#include <renderer/DepthOfFieldEffect.h>
#include <renderer/FilmEffect.h>
#include <renderer/FogEffect.h>
#include <renderer/HdrEffect.h>
#include <renderer/MotionBlurEffect.h>
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

        bool GetOptionalInt(const sol::table &t, const char *key, std::string_view binding, int &value)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            if (obj.get_type() != sol::type::number)
            {
                MIR_LOG_ERROR("script: world:{} - '{}' must be a number", binding, key);
                return false;
            }
            value = obj.as<int>();
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

        // A key that moved to the Camera refuses the call rather than being ignored.
        bool RejectCameraField(const sol::table &t, const char *key, std::string_view binding)
        {
            const sol::object obj = t[key];
            if (!obj.valid())
                return true;
            MIR_LOG_ERROR("script: world:{} - '{}' is a Camera field now; "
                "use world:set(camera, \"Camera\", \"{}\", value)", binding, key, key);
            return false;
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

            if (!RejectCameraField(args, "exposure", "set_hdr") ||
                !GetOptionalBool(args, "enabled", "set_hdr", enabled) ||
                !GetOptionalTonemapOperator(args, "tonemapOperator", "set_hdr", hdr.op))
                return false;

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

        bool WorldSetDepthOfField(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_depth_of_field");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kDepthOfFieldEffectName)];
            DepthOfFieldParams dof = ReadDepthOfFieldParams(state.params);
            bool enabled = state.enabled;

            if (!RejectCameraField(args, "focusDistance", "set_depth_of_field") ||
                !RejectCameraField(args, "aperture", "set_depth_of_field") ||
                !GetOptionalBool(args, "enabled", "set_depth_of_field", enabled) ||
                !GetOptionalFloat(args, "maxBlur", "set_depth_of_field", dof.maxBlur))
                return false;

            dof.maxBlur = std::max(dof.maxBlur, 0.0f);

            state.enabled = enabled;
            WriteDepthOfFieldParams(state.params, dof);
            return true;
        }

        sol::object WorldGetDepthOfField(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_depth_of_field");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kDepthOfFieldEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const DepthOfFieldParams dof =
                it != settings->effects.end() ? ReadDepthOfFieldParams(it->second.params) : DepthOfFieldParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["maxBlur"] = dof.maxBlur;
            return out;
        }

        bool WorldSetFilm(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_film");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kFilmEffectName)];
            FilmParams film = ReadFilmParams(state.params);
            bool enabled = state.enabled;

            if (!GetOptionalBool(args, "enabled", "set_film", enabled) ||
                !GetOptionalFloat(args, "grain", "set_film", film.grain) ||
                !GetOptionalFloat(args, "contrast", "set_film", film.contrast) ||
                !GetOptionalFloat(args, "saturation", "set_film", film.saturation) ||
                !GetOptionalFloat(args, "vignetteStrength", "set_film", film.vignetteStrength) ||
                !GetOptionalFloat(args, "vignetteRadius", "set_film", film.vignetteRadius))
                return false;

            film.grain = std::max(film.grain, 0.0f);
            film.contrast = std::max(film.contrast, 0.0f);
            film.saturation = std::max(film.saturation, 0.0f);
            film.vignetteStrength = std::max(film.vignetteStrength, 0.0f);
            film.vignetteRadius = std::max(film.vignetteRadius, 0.0f);

            state.enabled = enabled;
            WriteFilmParams(state.params, film);
            return true;
        }

        sol::object WorldGetFilm(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_film");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kFilmEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const FilmParams film = it != settings->effects.end() ? ReadFilmParams(it->second.params) : FilmParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["grain"] = film.grain;
            out["contrast"] = film.contrast;
            out["saturation"] = film.saturation;
            out["vignetteStrength"] = film.vignetteStrength;
            out["vignetteRadius"] = film.vignetteRadius;
            return out;
        }

        bool WorldSetMotionBlur(World &world, const sol::table &args)
        {
            PostProcessingSettings *settings = ResolveSettings(world, "set_motion_blur");
            if (settings == nullptr)
                return false;

            PostEffectState &state = settings->effects[std::string(kMotionBlurEffectName)];
            MotionBlurParams motionBlur = ReadMotionBlurParams(state.params);
            bool enabled = state.enabled;

            if (!RejectCameraField(args, "shutterStrength", "set_motion_blur") ||
                !GetOptionalBool(args, "enabled", "set_motion_blur", enabled) ||
                !GetOptionalInt(args, "sampleCount", "set_motion_blur", motionBlur.sampleCount))
                return false;

            motionBlur.sampleCount = std::max(motionBlur.sampleCount, 1);

            state.enabled = enabled;
            WriteMotionBlurParams(state.params, motionBlur);
            return true;
        }

        sol::object WorldGetMotionBlur(World &world, sol::this_state luaState)
        {
            sol::state_view lua(luaState);
            PostProcessingSettings *settings = ResolveSettings(world, "get_motion_blur");
            if (settings == nullptr)
                return sol::nil;

            const auto it = settings->effects.find(std::string(kMotionBlurEffectName));
            const bool enabled = it != settings->effects.end() && it->second.enabled;
            const MotionBlurParams motionBlur =
                it != settings->effects.end() ? ReadMotionBlurParams(it->second.params) : MotionBlurParams{};

            sol::table out = lua.create_table();
            out["enabled"] = enabled;
            out["sampleCount"] = motionBlur.sampleCount;
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
        worldType.set_function("set_depth_of_field", &WorldSetDepthOfField);
        worldType.set_function("get_depth_of_field", &WorldGetDepthOfField);
        worldType.set_function("set_film", &WorldSetFilm);
        worldType.set_function("get_film", &WorldGetFilm);
        worldType.set_function("set_motion_blur", &WorldSetMotionBlur);
        worldType.set_function("get_motion_blur", &WorldGetMotionBlur);
    }
}
