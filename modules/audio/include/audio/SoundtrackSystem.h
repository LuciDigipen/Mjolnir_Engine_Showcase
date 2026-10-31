/**
 * @file SoundtrackSystem.h
 * @author Quy Dong Ta
 * @brief Starts the scene's Soundtrack on Play and cuts it on Stop.
 *
 * Asks for the soundtrack's bank in Edit too, so Play seldom has to wait for it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/components/Soundtrack.h>

#include <core/ecs/Entity.h>
#include <core/ecs/Query.h>
#include <core/ecs/System.h>

namespace mir
{
    class SoundtrackSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

        
        static void OnPlayEnd(SystemContext &context);

        // Soundtrack's OnRemove during Play: fades out the music it started
        static void OnSoundtrackRemoved(World &world, Entity entity);

    private:
        Query<Soundtrack> *mQuery = nullptr;
        Entity mOwner = kNullEntity;
        bool mWarnedMultipleSoundtracks = false;
    };
}
