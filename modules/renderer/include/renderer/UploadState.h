/**
 * @file UploadState.h
 * @author sumin.park
 * @brief Common readiness states for asynchronous GPU resources.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    enum class UploadState : uint8_t { Stale, Pending, Uploading, Ready, Failed };
}
