/**
 * @file AudioBindings.cpp
 * @author Quy Dong Ta
 * @brief The Lua audio surface, scoped to an entity's AudioEmitter, and the music bus.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio_script/AudioBindings.h>

#include <audio/AudioSystem.h>
#include <audio/components/AudioEmitter.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <sol/sol.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace mir
{
    namespace
    {

        constexpr int kStopFadeMs = 10;


        bool ToName(const sol::object &value, std::string_view binding, std::string &out)
        {
            if (!value.is<std::string>())
            {
                MIR_LOG_ERROR("script: world:{} needs a name string", binding);
                return false;
            }

            out = value.as<std::string>();
            return !out.empty();
        }


        bool ToNumber(const sol::object &value, std::string_view binding, float &out)
        {
            if (value.get_type() != sol::type::number)
            {
                MIR_LOG_ERROR("script: world:{} needs a number", binding);
                return false;
            }

            out = value.as<float>();
            return true;
        }

        AudioEmitter *ResolveEmitter(World &world, Entity entity)
        {
            AudioEmitter *emitter = world.GetComponent<AudioEmitter>(entity);
            if (emitter == nullptr || emitter->mGameObject == 0)
                return nullptr;

            return emitter;
        }

        uint32_t WorldPostEvent(World &world, Entity entity, const sol::object &eventName)
        {
            std::string name;
            if (!ToName(eventName, "post_event", name))
                return 0;

            AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return 0;

            const uint32_t playingId = audio::PostEventOn(name.c_str(), emitter->mGameObject);
            if (playingId == 0)
                return 0;


            if (emitter->mPlayingId != 0)
                audio::StopPlaying(emitter->mPlayingId, kStopFadeMs);

            emitter->mPlayingId = playingId;
            emitter->mStarted = true;
            return playingId;
        }

        bool WorldStopEvent(World &world, Entity entity, sol::optional<sol::object> fadeMs)
        {
            AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr || emitter->mPlayingId == 0)
                return false;

            float fade = static_cast<float>(kStopFadeMs);
            if (fadeMs.has_value() && fadeMs->valid() && !ToNumber(*fadeMs, "stop_event", fade))
                return false;

            audio::StopPlaying(emitter->mPlayingId, static_cast<int>(fade));
            emitter->mPlayingId = 0;

            return true;
        }

        bool WorldIsPlaying(World &world, Entity entity)
        {
            const AudioEmitter *emitter = ResolveEmitter(world, entity);
            return emitter != nullptr && audio::IsVoicePlaying(emitter->mPlayingId);
        }

        bool WorldSetRtpc(World &world, Entity entity, const sol::object &rtpcName,
                          const sol::object &value)
        {
            std::string name;
            float number = 0.0f;
            if (!ToName(rtpcName, "set_rtpc", name) || !ToNumber(value, "set_rtpc", number))
                return false;

            const AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return false;

            return audio::SetRTPCValue(name.c_str(), number, emitter->mGameObject);
        }

        bool WorldSetSwitch(World &world, Entity entity, const sol::object &switchGroup,
                            const sol::object &switchState)
        {
            std::string group;
            std::string state;
            if (!ToName(switchGroup, "set_switch", group) || !ToName(switchState, "set_switch", state))
                return false;

            const AudioEmitter *emitter = ResolveEmitter(world, entity);
            if (emitter == nullptr)
                return false;

            return audio::SetSwitch(group.c_str(), state.c_str(), emitter->mGameObject);
        }


        bool WorldSetState(World &, const sol::object &stateGroup, const sol::object &state)
        {
            std::string group;
            std::string value;
            if (!ToName(stateGroup, "set_state", group) || !ToName(state, "set_state", value))
                return false;

            return audio::SetState(group.c_str(), value.c_str());
        }


        bool ToVolume(const sol::object &value, std::string_view binding, float &out)
        {
            if (!ToNumber(value, binding, out))
                return false;

            // written so NaN fails it too
            if (!(out >= 0.0f && out <= 1.0f))
            {
                MIR_LOG_ERROR("script: world:{} needs a number from 0 to 1, got {}", binding, out);
                return false;
            }
            return true;
        }

        bool ToMuted(const sol::object &value, std::string_view binding, bool &out)
        {
            if (value.get_type() != sol::type::boolean)
            {
                MIR_LOG_ERROR("script: world:{} needs a boolean", binding);
                return false;
            }

            out = value.as<bool>();
            return true;
        }

        bool WorldSetMusicVolume(World &, const sol::object &volume)
        {
            float value = 0.0f;
            if (!ToVolume(volume, "set_music_volume", value))
                return false;

            audio::SetMusicVolume(value);
            return true;
        }

        float WorldMusicVolume(World &)
        {
            return audio::GetMusicVolume();
        }

        bool WorldSetMusicMuted(World &, const sol::object &muted)
        {
            bool value = false;
            if (!ToMuted(muted, "set_music_muted", value))
                return false;

            audio::SetMusicMuted(value);
            return true;
        }

        bool WorldMusicMuted(World &)
        {
            return audio::IsMusicMuted();
        }

        bool WorldSetSfxVolume(World &, const sol::object &volume)
        {
            float value = 0.0f;
            if (!ToVolume(volume, "set_sfx_volume", value))
                return false;

            audio::SetSfxVolume(value);
            return true;
        }

        float WorldSfxVolume(World &)
        {
            return audio::GetSfxVolume();
        }

        bool WorldSetSfxMuted(World &, const sol::object &muted)
        {
            bool value = false;
            if (!ToMuted(muted, "set_sfx_muted", value))
                return false;

            audio::SetSfxMuted(value);
            return true;
        }

        bool WorldSfxMuted(World &)
        {
            return audio::IsSfxMuted();
        }
    }

    void RegisterAudioBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("post_event", &WorldPostEvent);
        worldType.set_function("stop_event", &WorldStopEvent);
        worldType.set_function("is_playing", &WorldIsPlaying);
        worldType.set_function("set_rtpc", &WorldSetRtpc);
        worldType.set_function("set_switch", &WorldSetSwitch);
        worldType.set_function("set_state", &WorldSetState);
        worldType.set_function("set_music_volume", &WorldSetMusicVolume);
        worldType.set_function("music_volume", &WorldMusicVolume);
        worldType.set_function("set_music_muted", &WorldSetMusicMuted);
        worldType.set_function("music_muted", &WorldMusicMuted);
        worldType.set_function("set_sfx_volume", &WorldSetSfxVolume);
        worldType.set_function("sfx_volume", &WorldSfxVolume);
        worldType.set_function("set_sfx_muted", &WorldSetSfxMuted);
        worldType.set_function("sfx_muted", &WorldSfxMuted);
    }
}
