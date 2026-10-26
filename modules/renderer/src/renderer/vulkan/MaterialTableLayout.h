/**
 * @file MaterialTableLayout.h
 * @author Sumin Park
 * @brief Row counts and set-4 bindings shared by the material table and its consumers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir::vk
{
    // Set 4 is the shared table set: material records, sampled images and samplers.
    constexpr uint32_t kMaterialTableSetIndex = 4;
    constexpr uint32_t kMaterialRecordBinding = 0;
    constexpr uint32_t kMaterialImageBinding = 1;
    constexpr uint32_t kMaterialSamplerBinding = 2;

    constexpr uint32_t kMaxSharedImages = 1024;
    constexpr uint32_t kMaxSharedSamplers = 256;

    constexpr uint32_t kMaterialSetCombinedImages = 2;
    constexpr uint32_t kMaterialSetRecordUniformBuffers = 1;
    constexpr uint32_t kCameraSetUniformBuffers = 1;
    constexpr uint32_t kCameraSetStorageBuffers = 1;
    constexpr uint32_t kModelSetStorageBuffers = 1;
    constexpr uint32_t kShadowSetCombinedImages = 3;
    constexpr uint32_t kShadowSetStorageBuffers = 1;

    constexpr uint32_t kRasterPipelineSetCount = kMaterialTableSetIndex + 1;
    constexpr uint32_t kRtPipelineSetCount = 2;
    constexpr uint32_t kRtSetStorageBuffers = 2;
    constexpr uint32_t kRtSetStorageImages = 7;
    constexpr uint32_t kRtSetAccelerationStructures = 1;

    constexpr uint32_t kMaterialRecordCapacityBytes = 1u << 20;

    // RT set 0 keeps its own row order. The table rows reuse the old combined-texture row.
    constexpr uint32_t kRtMaterialImageBinding = 9;
    constexpr uint32_t kRtMaterialSamplerBinding = 10;
    constexpr uint32_t kRtMaterialRecordBinding = 11;
}
