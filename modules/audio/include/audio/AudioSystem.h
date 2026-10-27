/**
 * @file AudioSystem.h
 * @author Dong Ta
 * @brief Wwise sound engine lifecycle - init, per-frame update, event
 *        posting, and teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <glm/vec3.hpp>
#include <core/math/WorldCoordinates.h>
namespace mir::audio
{
    enum class BankState : uint8_t
    {
        Unknown,
        Loading,
        Loaded,
        Failed,
    };

    // Loads Init.bnk and queues every other bank in bankDir, which then load on
    // Wwise's bank thread. Throws SetupError on any failure before that
    void Init(const std::filesystem::path &bankDir);

    void Update();

    // Safe without Init and safe to call twice
    void Terminate();

    uint64_t RegisterEmitter(const char *debugName);

    void UnregisterEmitter(uint64_t gameObject);

    // Wwise positions are double, so world positions go in unnarrowed.
    void SetEmitterPosition(uint64_t gameObject,
        WorldPosition position,
        const glm::vec3 &front,
        const glm::vec3 &up);

    void SetListenerPosition(WorldPosition position,
        const glm::vec3 &front,
        const glm::vec3 &up);

    bool PostEvent(const char *eventName);

    uint32_t PostEventOn(const char *eventName, uint64_t gameObject);

    // A fadeMs of 10-20 keeps a stopped voice from clicking.
    void StopPlaying(uint32_t playingId, int fadeMs = 0);

    // True until Wwise reports audio has ended. Ends are collected on
    // the audio thread and applied in Update(), so this answers for last frame.
    bool IsVoicePlaying(uint32_t playingId);


    bool SetRTPCValue(const char *rtpcName, float value, uint64_t gameObject = 0);

    //switches are per game object, wwise has no global switch.
    bool SetSwitch(const char *switchGroup, const char *switchState, uint64_t gameObject);

    //states are global by design, no game object takes part.
    bool SetState(const char *stateGroup, const char *state);

    // Queues the bank the first time it is named, with or without ".bnk". The
    // state changes in Update(), so a load finishing now is seen next frame
    BankState RequestBank(const char *bankName);

    // An event posted before its bank has loaded does not play
    bool AreBanksLoading();

    // False without Init
    bool PlayMusic(const char *eventName, const char *bankName);

    void StopMusic(int fadeMs);


    bool IsMusicPlaying();

    // Linear, 0 to 1, written to the Music_Volume game parameter
    void SetMusicVolume(float volume);

    float GetMusicVolume();

    // Silences the Music bus and keeps the volume to come back to
    void SetMusicMuted(bool muted);

    bool IsMusicMuted();

    bool IsInitialized();

}
