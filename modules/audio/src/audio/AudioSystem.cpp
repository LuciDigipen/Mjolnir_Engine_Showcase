/**
 * @file AudioSystem.cpp
 * @author Dong Ta
 * @brief Wwise sound engine lifecycle - init, per-frame update, event
 *        posting, and teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "audio/AudioSystem.h"

#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>

#include "AkDefaultIOHookDeferred.h"

#ifndef AK_OPTIMIZED
#include <AK/Comm/AkCommunication.h>
#endif

#include "core/log/Log.h"
#include "core/log/SetupError.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <glm/geometric.hpp>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace mir::audio {
    namespace {
        CAkDefaultIOHookDeferred g_lowLevelIO;
        bool g_ioInitialized = false;
        bool g_Initialized = false;
        

        constexpr AkGameObjectID kGlobalObject = 1;
        constexpr AkGameObjectID kListener = 2;

        const std::filesystem::path kInitBank = "Init.bnk";


        AkGameObjectID g_nextEmitterId = 1000;


        bool g_warnedSetPositionFailed = false;
        bool Check(AKRESULT result, const char* stage)
        {
            if (result != AK_Success)
            {
                MIR_LOG_ERROR("Wwise {} failed (AKRESULT {})", stage, static_cast<int>(result));
                return false;
            }
            return true;
        }
        

        constexpr float kMinUpLengthSq = 1e-8f;

        AkSoundPosition MakeSoundPosition(WorldPosition position, const glm::vec3 &front, const glm::vec3 &up)
        {
            const bool d = glm::dot(front, front) <= 0.0f || glm::dot(up, up) <= 0.0f;
            const glm::vec3 f = d ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::normalize(front);
            const glm::vec3 rawUp = d ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(up);

            glm::vec3 ortho = rawUp - f * glm::dot(f, rawUp);
            if (glm::dot(ortho, ortho) <= kMinUpLengthSq)
            {

                const bool alignedWithY = f.y > 0.99f || f.y < -0.99f;
                const glm::vec3 reference = alignedWithY ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                         : glm::vec3(0.0f, 1.0f, 0.0f);
                ortho = reference - f * glm::dot(f, reference);
            }

            const glm::vec3 u = glm::normalize(ortho);

            const glm::dvec3 pos(position.Meters());
            AkSoundPosition out;
            out.Set(pos.x, pos.y, -pos.z, f.x, f.y, -f.z, u.x, u.y, -u.z);
            return out;
        }

        void ApplyPosition(AkGameObjectID gameObject, const AkSoundPosition &position, const char* what)
        {
            if (AK::SoundEngine::SetPosition(gameObject, position) == AK_Success)
                return;

            if (!g_warnedSetPositionFailed)
            {
                MIR_LOG_ERROR("wwise setposition rejected for {} (object {}); position updates are being dropped",
                    what, gameObject);
                g_warnedSetPositionFailed = true;
            }
        }


        std::unordered_set<AkPlayingID> g_livePlayingIds;

        std::mutex g_endedMutex;
        std::vector<AkPlayingID> g_endedPlayingIds;
        std::vector<AkPlayingID> g_endedScratch;

        // Called on Wwise's audio thread: nothing may escape into it
        void OnEventEnded(AkCallbackType type, AkEventCallbackInfo *eventInfo, void *, void *) noexcept
        {
            if (type != AK_EndOfEvent || eventInfo == nullptr)
                return;

            try
            {
                std::lock_guard<std::mutex> lock(g_endedMutex);
                g_endedPlayingIds.push_back(eventInfo->playingID);
            }
            catch (const std::bad_alloc &)
            {
                // out of memory is the process's failure: terminate reaches the crash handler
                std::terminate();
            }
            catch (...)
            {
                // a lost end only keeps IsVoicePlaying true; Terminate clears it
            }
        }

        std::string ResultText(AKRESULT result)
        {
            return "AKRESULT " + std::to_string(static_cast<int>(result));
        }

        // Tears down whatever came up, so a caller that never reaches Terminate leaks nothing
        [[noreturn]] void Fail(ErrorCategory category, std::string what, const std::filesystem::path &path,
                               std::string cause, std::string fix)
        {
            Terminate();
            throw SetupError(category, std::move(what), path, std::move(cause), std::move(fix));
        }

        // Every .bnk directly in @p bankDir, Init.bnk first: Wwise loads nothing before it
        std::vector<std::filesystem::path> FindBanks(const std::filesystem::path &bankDir)
        {
            std::vector<std::filesystem::path> banks;
            std::error_code ec;
            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(bankDir, ec))
            {
                std::error_code entryEc;
                if (entry.is_regular_file(entryEc) && entry.path().extension() == ".bnk")
                    banks.push_back(entry.path().filename());
            }
            std::sort(banks.begin(), banks.end(), [](const std::filesystem::path &a, const std::filesystem::path &b)
                {
                    if ((a == kInitBank) != (b == kInitBank))
                        return a == kInitBank;
                    return a < b;
                });
            return banks;
        }
    }

    void Init(const std::filesystem::path &bankDir)
    {
        std::error_code errorC;
        if (!std::filesystem::is_directory(bankDir, errorC))
        {
            throw SetupError(ErrorCategory::System, "The SoundBank directory is missing", bankDir,
                "it should have been created at startup", "Create the directory, or check its parent is writable.");
        }

        // Checked before Wwise starts, so a missing Init.bnk names itself instead of failing LoadBank
        const std::vector<std::filesystem::path> banks = FindBanks(bankDir);
        if (!banks.empty() && banks.front() != kInitBank)
        {
            throw SetupError(ErrorCategory::ProjectData, "SoundBanks are present but Init.bnk is not", bankDir,
                "Wwise cannot load any bank before Init.bnk",
                "Regenerate the SoundBanks in Wwise and copy Init.bnk into " + bankDir.string() + ".");
        }

        AkMemSettings memSettings;
        AK::MemoryMgr::GetDefaultSettings(memSettings);
        if (const AKRESULT result = AK::MemoryMgr::Init(&memSettings); result != AK_Success)
        {
            Fail(ErrorCategory::System, "Wwise memory manager failed to start", {}, ResultText(result),
                "Check the log above for Wwise's reason.");
        }

        AkStreamMgrSettings stmSettings;
        AK::StreamMgr::GetDefaultSettings(stmSettings);
        if (AK::StreamMgr::Create(stmSettings) == nullptr)
        {
            Fail(ErrorCategory::System, "Wwise stream manager failed to start", {}, "StreamMgr::Create returned null",
                "Check the log above for Wwise's reason.");
        }

        AkDeviceSettings deviceSettings;
        AK::StreamMgr::GetDefaultDeviceSettings(deviceSettings);
        if (const AKRESULT result = g_lowLevelIO.Init(deviceSettings); result != AK_Success)
        {
            Fail(ErrorCategory::System, "Wwise file I/O failed to start", {}, ResultText(result),
                "Check the log above for Wwise's reason.");
        }
        g_ioInitialized = true;

        AkInitSettings initSettings;
        AkPlatformInitSettings AkPlatformInitSettings;
        AK::SoundEngine::GetDefaultInitSettings(initSettings);

        AK::SoundEngine::GetDefaultPlatformInitSettings(AkPlatformInitSettings);
        if (const AKRESULT result = AK::SoundEngine::Init(&initSettings, &AkPlatformInitSettings); result != AK_Success)
        {
            Fail(ErrorCategory::System, "Wwise sound engine failed to start", {}, ResultText(result),
                "Check that an audio output device is available.");
        }
#ifndef AK_OPTIMIZED
        AkCommSettings commSettings;
        AK::Comm::GetDefaultInitSettings(commSettings);
        if (const AKRESULT result = AK::Comm::Init(commSettings); result != AK_Success)
        {
            Fail(ErrorCategory::System, "Wwise profiler connection failed to start", {}, ResultText(result),
                "Close any other program holding the Wwise profiler port, such as another engine instance "
                "or a tool connected to it.");
        }
#endif

        g_lowLevelIO.SetBasePath(bankDir.c_str());
        for (const std::filesystem::path &bank : banks)
        {
            AkBankID bankID;
            if (const AKRESULT result = AK::SoundEngine::LoadBank(bank.string().c_str(), bankID); result != AK_Success)
            {
                Fail(ErrorCategory::ProjectData, "SoundBank " + bank.string() + " failed to load", bankDir / bank,
                    ResultText(result),
                    "Regenerate the SoundBanks with the Wwise version the engine links, then copy them into " +
                        bankDir.string() + ".");
            }
        }

        AK::SoundEngine::RegisterGameObj(kListener, "Listener");
        AK::SoundEngine::SetDefaultListeners(&kListener, 1);
        AK::SoundEngine::RegisterGameObj(kGlobalObject, "Global");
        g_Initialized = true;
        MIR_LOG_INFO("wwise initialized, {} SoundBank(s) from '{}'", banks.size(), bankDir.string());
    }


    void Update()
    {
        if (!g_Initialized)
            return;

        AK::SoundEngine::RenderAudio();

        {
            std::lock_guard<std::mutex> lock(g_endedMutex);
            g_endedScratch.swap(g_endedPlayingIds);
        }

        for (const AkPlayingID playingId : g_endedScratch)
            g_livePlayingIds.erase(playingId);

        g_endedScratch.clear();
    }

    bool IsInitialized()
    {
        return g_Initialized;
    }

    uint64_t RegisterEmitter(const char* debugName)
    {
        if (!g_Initialized)
            return 0;
        
        const AkGameObjectID id = g_nextEmitterId++;
        if (!Check(AK::SoundEngine::RegisterGameObj(id, debugName), "RegisterGameObj"))
            return 0;
        return id;
    }

    void UnregisterEmitter(uint64_t gameObj)
    {
        if (!g_Initialized || gameObj == 0 || gameObj == AK_INVALID_GAME_OBJECT)
            return;

        AK::SoundEngine::UnregisterGameObj(static_cast<AkGameObjectID>(gameObj));
    }

    void SetEmitterPosition(uint64_t gameObject, WorldPosition position, const glm::vec3 &front,
                            const glm::vec3 &up)
    {
        if (!g_Initialized || gameObject == 0)
            return;

        ApplyPosition(static_cast<AkGameObjectID>(gameObject), MakeSoundPosition(position, front, up), "emitter");
    }

    void SetListenerPosition(WorldPosition position, const glm::vec3 &front, const glm::vec3 &up)
    {
        if (!g_Initialized)
            return;

        ApplyPosition(kListener, MakeSoundPosition(position, front, up), "listener");
    }

    bool PostEvent(const char* eventName)
    {
        return PostEventOn(eventName, kGlobalObject) != 0;
    }

    uint32_t PostEventOn(const char* eventName, uint64_t gameObject)
    {
        if (!g_Initialized)
            return 0;

        static_assert(std::is_nothrow_invocable_v<decltype(&OnEventEnded), AkCallbackType, AkEventCallbackInfo *,
                          void *, void *>, "C callbacks and thread entry points must be noexcept (conventions.md)");
        const AkPlayingID playingId = AK::SoundEngine::PostEvent(eventName,
            static_cast<AkGameObjectID>(gameObject), AK_EndOfEvent, &OnEventEnded);

        if (playingId == AK_INVALID_PLAYING_ID)
        {
            MIR_LOG_ERROR("wwise postevent ({}) on object {} failed", eventName, gameObject);
            return 0;
        }

        g_livePlayingIds.insert(playingId);
        return playingId;
    }


    void StopPlaying(uint32_t playingId, int fadeMs)
    {
        if (!g_Initialized || playingId == AK_INVALID_PLAYING_ID)
            return;

        AK::SoundEngine::ExecuteActionOnPlayingID(AkActionOnEventType_Stop,
            static_cast<AkPlayingID>(playingId), static_cast<AkTimeMs>(fadeMs));

        g_livePlayingIds.erase(playingId);
    }

    bool IsVoicePlaying(uint32_t playingId)
    {
        return playingId != AK_INVALID_PLAYING_ID && g_livePlayingIds.count(playingId) != 0;
    }



    bool SetRTPCValue(const char *rtpcName, float value, uint64_t gameObject)
    {
        if (!g_Initialized || rtpcName == nullptr)
            return false;

        const AkGameObjectID target = gameObject == 0 ? AK_INVALID_GAME_OBJECT : static_cast<AkGameObjectID>(gameObject);

        return Check(AK::SoundEngine::SetRTPCValue(rtpcName, static_cast<AkRtpcValue>(value), target),"SetRTPCValue");
    }

    bool SetSwitch(const char *switchGroup, const char *switchState, uint64_t gameObject)
    {
        if (!g_Initialized || switchGroup == nullptr || switchState == nullptr || gameObject == 0)
            return false;

        return Check(AK::SoundEngine::SetSwitch(switchGroup, switchState, static_cast<AkGameObjectID>(gameObject)), "SetSwitch");
    }

    bool SetState(const char *stateGroup, const char *state)
    {
        if (!g_Initialized || stateGroup == nullptr || state == nullptr)
            return false;

        return Check(AK::SoundEngine::SetState(stateGroup, state), "SetState");
    }

    void Terminate()
    {
        if (AK::SoundEngine::IsInitialized()) 
        {
            AK::SoundEngine::UnregisterAllGameObj();
            AK::SoundEngine::ClearBanks();
#ifndef AK_OPTIMIZED
            AK::Comm::Term();
#endif
            AK::SoundEngine::Term();
        }

        if (g_ioInitialized) 
        {
            g_lowLevelIO.Term();
            g_ioInitialized = false;
        }

        if (AK::IAkStreamMgr::Get())
            AK::IAkStreamMgr::Get()->Destroy();

        if (AK::MemoryMgr::IsInitialized())
            AK::MemoryMgr::Term();

        g_livePlayingIds.clear();
        {
            std::lock_guard<std::mutex> lock(g_endedMutex);
            g_endedPlayingIds.clear();
        }

        g_Initialized = false;
    }

}
