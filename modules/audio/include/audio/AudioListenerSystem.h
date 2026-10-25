/**
 * @file AudioListenerSystem.h
 * @author Quy Dong Ta
 * @brief Pushes the active AudioListener's WorldTransform to Wwise each frame.
 *
 * Registered in SystemPhase::LateUpdate, after Propagate makes WorldTransform
 * current for the frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/components/AudioListener.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/WorldTransform.h>

namespace mir
{
    class AudioListenerSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<const WorldTransform, AudioListener> *mQuery = nullptr;
        bool mWarnedMultipleListeners = false;
    };
}
