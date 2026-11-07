/**
 * @file SceneInstance.h
 * @author sumin.park
 * @brief Runtime identity and ownership of a loaded scene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace mir
{
    using SceneInstanceId = uint64_t;
    inline constexpr SceneInstanceId kOwnedBySession = 0; // object with this id is owned by session not scene

    inline SceneInstanceId AllocateSceneInstanceId()
    {
        static std::atomic<SceneInstanceId> next{ 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }
}
