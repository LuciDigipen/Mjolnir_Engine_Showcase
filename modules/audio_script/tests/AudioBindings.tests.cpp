/**
 * @file AudioBindings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the Lua world:post_event/stop_event/is_playing/set_rtpc/set_switch/set_state bindings
 *        and the music volume and mute.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio_script/AudioBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <audio/AudioSystem.h>
#include <audio/components/AudioEmitter.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    using mir::AudioEmitter;
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::RuntimeFieldDecl;
    using mir::ScriptHost;
    using mir::World;


    constexpr uint64_t kFakeGameObject = 4242;

    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            {"playingId", FieldKind::Int},
            {"stopped", FieldKind::Bool},
            {"rtpc", FieldKind::Bool},
            {"switched", FieldKind::Bool},
            {"state", FieldKind::Bool},
            {"reached", FieldKind::Bool},
            {"playing", FieldKind::Bool},
            {"volume", FieldKind::Float},
            {"muted", FieldKind::Bool},
        };
        return ComponentRegistry::Instance().RegisterRuntime("AudioBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(World &world, Entity entity, std::string_view field)
    {
        const ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    struct Fixture
    {
        World world;
        CommandBuffer commands;
        ScriptHost host;
        const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterAudioBindings);
        Entity entity{};
        int32_t instance = -1;

        Fixture(std::string_view source, std::string_view name)
        {
            entity = world.CreateEntity();
            ProbeOps().AddDefault(world, entity);

            REQUIRE(host.LoadScriptSource(std::string(name), source));
            instance = host.CreateInstance(std::string(name));
            REQUIRE(instance != -1);
        }

        ~Fixture() { host.DestroyInstance(instance); }

        AudioEmitter &AddEmitter(uint64_t gameObject, uint32_t playingId = 0)
        {
            world.AddComponent<AudioEmitter>(entity, AudioEmitter{});
            AudioEmitter &emitter = *world.GetComponent<AudioEmitter>(entity);
            emitter.mGameObject = gameObject;
            emitter.mPlayingId = playingId;
            return emitter;
        }

        void Start()
        {
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
        }
    };

    constexpr std::string_view kProbeEverything = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set(entity, "AudioBindingsProbe", "playingId", world:post_event(entity, "Play_chargeDown"))
    world:set(entity, "AudioBindingsProbe", "stopped", world:stop_event(entity))
    world:set(entity, "AudioBindingsProbe", "rtpc", world:set_rtpc(entity, "Speed", 0.5))
    world:set(entity, "AudioBindingsProbe", "switched", world:set_switch(entity, "Surface", "Grass"))
    world:set(entity, "AudioBindingsProbe", "state", world:set_state("Music", "Combat"))
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kPostOnly = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set(entity, "AudioBindingsProbe", "playingId", world:post_event(entity, "Play_chargeDown"))
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kIsPlaying = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set(entity, "AudioBindingsProbe", "playing", world:is_playing(entity))
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kMalformed = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local anySucceeded = (world:post_event(entity, 7) ~= 0)
        or world:set_rtpc(entity, "Speed", "loud")
        or world:set_rtpc(entity, 7, 0.5)
        or world:set_switch(entity, "Surface", nil)
        or world:set_state("Music", 3)

    world:set(entity, "AudioBindingsProbe", "rtpc", not anySucceeded)
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kMusic = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local volumeSet = world:set_music_volume(0.25)
    local mutedSet = world:set_music_muted(true)
    world:set(entity, "AudioBindingsProbe", "rtpc", volumeSet and mutedSet)
    world:set(entity, "AudioBindingsProbe", "volume", world:music_volume())
    world:set(entity, "AudioBindingsProbe", "muted", world:music_muted())
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kMusicMalformed = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local anySucceeded = world:set_music_volume(1.5)
        or world:set_music_volume(-0.1)
        or world:set_music_volume(0 / 0)
        or world:set_music_volume("loud")
        or world:set_music_muted(1)
        or world:set_music_muted("yes")

    world:set(entity, "AudioBindingsProbe", "rtpc", not anySucceeded)
    world:set(entity, "AudioBindingsProbe", "volume", world:music_volume())
    world:set(entity, "AudioBindingsProbe", "muted", world:music_muted())
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kSfx = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local volumeSet = world:set_sfx_volume(0.6)
    local mutedSet = world:set_sfx_muted(true)
    world:set(entity, "AudioBindingsProbe", "rtpc", volumeSet and mutedSet)
    world:set(entity, "AudioBindingsProbe", "volume", world:sfx_volume())
    world:set(entity, "AudioBindingsProbe", "muted", world:sfx_muted())
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kSfxMalformed = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local anySucceeded = world:set_sfx_volume(2)
        or world:set_sfx_volume(-1)
        or world:set_sfx_volume(0 / 0)
        or world:set_sfx_volume(math.huge)
        or world:set_sfx_volume({})
        or world:set_sfx_muted(0)
        or world:set_sfx_muted(nil)

    world:set(entity, "AudioBindingsProbe", "rtpc", not anySucceeded)
    world:set(entity, "AudioBindingsProbe", "volume", world:sfx_volume())
    world:set(entity, "AudioBindingsProbe", "muted", world:sfx_muted())
    world:set(entity, "AudioBindingsProbe", "reached", true)
end

return T
)LUA";

    // the volumes are process state, so each test puts them back
    struct MusicReset
    {
        ~MusicReset()
        {
            mir::audio::SetMusicVolume(1.0f);
            mir::audio::SetMusicMuted(false);
            mir::audio::SetSfxVolume(1.0f);
            mir::audio::SetSfxMuted(false);
        }
    };
}

TEST_CASE("the audio bindings are inert on an entity with no AudioEmitter", "[script][audio]")
{
    // The whole file rests on this: no Init, so no engine, so every call refuses.
    REQUIRE_FALSE(mir::audio::IsInitialized());

    Fixture fixture(kProbeEverything, "audio_probe_no_emitter");

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "playingId") == 0);
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "stopped"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "switched"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "state"));
}

TEST_CASE("an AudioEmitter with no game object yet is refused", "[script][audio]")
{
    Fixture fixture(kProbeEverything, "audio_probe_unregistered");


    fixture.AddEmitter(/*gameObject=*/0);
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "playingId") == 0);
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "switched"));
}

TEST_CASE("a registered emitter still needs a live sound engine", "[script][audio]")
{
    Fixture fixture(kProbeEverything, "audio_probe_registered");


    fixture.AddEmitter(kFakeGameObject);
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "playingId") == 0);
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "switched"));
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "state"));
}

TEST_CASE("a refused post leaves the emitter's voice bookkeeping alone", "[script][audio]")
{
    Fixture fixture(kPostOnly, "audio_probe_post_refused");


    const uint32_t existingVoice = 99;
    fixture.AddEmitter(kFakeGameObject, existingVoice);
    fixture.Start();

    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "playingId") == 0);

    const AudioEmitter &emitter = *fixture.world.GetComponent<AudioEmitter>(fixture.entity);
    CHECK(emitter.mPlayingId == existingVoice);
    CHECK_FALSE(emitter.mStarted);
}

TEST_CASE("the audio bindings refuse malformed arguments instead of erroring out",
          "[script][audio]")
{
    Fixture fixture(kMalformed, "audio_probe_malformed");

    fixture.AddEmitter(kFakeGameObject);
    fixture.Start();

    // Both together: the script survived every bad call, and no bad call passed.
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
}

TEST_CASE("world:is_playing is false without an emitter, and for a voice the engine does not have", "[script][audio]")
{
    Fixture noEmitter(kIsPlaying, "audio_probe_is_playing_none");
    noEmitter.Start();
    CHECK(ReadProbeField<bool>(noEmitter.world, noEmitter.entity, "reached"));
    CHECK_FALSE(ReadProbeField<bool>(noEmitter.world, noEmitter.entity, "playing"));

    // a stale id is not a playing voice: no engine is running, so nothing is live
    Fixture staleVoice(kIsPlaying, "audio_probe_is_playing_stale");
    staleVoice.AddEmitter(kFakeGameObject, /*playingId=*/99);
    staleVoice.Start();
    CHECK(ReadProbeField<bool>(staleVoice.world, staleVoice.entity, "reached"));
    CHECK_FALSE(ReadProbeField<bool>(staleVoice.world, staleVoice.entity, "playing"));
}

TEST_CASE("world:stop_event only reports a voice it actually stopped", "[script][audio]")
{
    Fixture fixture(kProbeEverything, "audio_probe_stop");

    // Nothing playing: the stop has nothing to report even though the emitter
    // itself resolves.
    fixture.AddEmitter(kFakeGameObject, /*playingId=*/0);
    fixture.Start();

    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "stopped"));
}

TEST_CASE("the music volume and mute are kept without a sound engine", "[script][audio]")
{
    const MusicReset reset;
    Fixture fixture(kMusic, "audio_probe_music");

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "volume") == 0.25f);
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "muted"));
    CHECK(mir::audio::GetMusicVolume() == 0.25f);
    CHECK(mir::audio::IsMusicMuted());
}

TEST_CASE("a music volume outside 0 to 1, or a mute that is not a boolean, is refused", "[script][audio]")
{
    const MusicReset reset;
    mir::audio::SetMusicVolume(0.5f);
    Fixture fixture(kMusicMalformed, "audio_probe_music_malformed");

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "volume") == 0.5f);
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "muted"));
}

TEST_CASE("music cannot start without a sound engine", "[audio]")
{
    CHECK_FALSE(mir::audio::PlayMusic("Play_Soundtrack", "Music"));
    CHECK_FALSE(mir::audio::IsMusicPlaying());
    CHECK(mir::audio::RequestBank("Music") == mir::audio::BankState::Unknown);
    CHECK_FALSE(mir::audio::AreBanksLoading());

    // safe to call with nothing to stop
    mir::audio::StopMusic(0);
}

TEST_CASE("the SFX volume and mute are kept apart from the music's", "[script][audio]")
{
    const MusicReset reset;
    Fixture fixture(kSfx, "audio_probe_sfx");

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "volume") == 0.6f);
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "muted"));
    CHECK(mir::audio::GetMusicVolume() == 1.0f);
    CHECK_FALSE(mir::audio::IsMusicMuted());
}

TEST_CASE("an SFX volume outside 0 to 1, or a mute that is not a boolean, is refused", "[script][audio]")
{
    const MusicReset reset;
    mir::audio::SetSfxVolume(0.5f);
    Fixture fixture(kSfxMalformed, "audio_probe_sfx_malformed");

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "rtpc"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "volume") == 0.5f);
    CHECK_FALSE(ReadProbeField<bool>(fixture.world, fixture.entity, "muted"));
}
