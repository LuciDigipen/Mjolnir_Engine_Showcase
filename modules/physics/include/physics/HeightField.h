/**
 * @file HeightField.h
 * @author sumin.park
 * @brief Static height field shapes: build one from a grid of samples, share it between bodies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace mir
{
    class PhysicsWorld;

    // Drops every triangle touching the sample (Jolt's cNoCollisionValue).
    inline constexpr float kHeightFieldHole = std::numeric_limits<float>::max();

    // Cells split along (x, z)-(x+1, z+1), the terrain surface's diagonal.
    struct HeightFieldDesc
    {
        // x fastest.
        std::span<const float> mSamples;
        uint32_t mCountX = 0;
        uint32_t mCountZ = 0;
        float mSpacing = 1.0f;
        // Sample (0, 0) at height 0.
        WorldPosition mOrigin;

        // Uses every stride-th sample; count - 1 must be a multiple of it.
        uint32_t mSampleStride = 1;

        // Jolt's block size; 4 takes ~55% of the memory for ~1.6x the error.
        uint32_t mBlockSize = 2;
        uint32_t mBitsPerSample = 8;

        // Jolt's 16-bit height range; shapes given one range decode a sample they share alike away from
        // their max sides. NaN takes the samples' own range.
        float mQuantizationMin = std::numeric_limits<float>::quiet_NaN();
        float mQuantizationMax = std::numeric_limits<float>::quiet_NaN();
    };

    struct HeightFieldInfo
    {
        // Jolt's square side, padded with holes.
        uint32_t mSampleCount = 0;
        std::size_t mMemoryBytes = 0;
        double mMaxQuantizationError = 0.0;
        double mMaxHorizontalError = 0.0;
        uint64_t mValidSamples = 0;
        double mExtentX = 0.0;
        double mExtentZ = 0.0;
    };

    // A built height field shape that static bodies at any number of places share. Immutable.
    class SharedHeightField
    {
    public:
        bool IsValid() const
        {
            return mData != nullptr;
        }
        // Null for a shape Jolt did not make a height field of.
        const HeightFieldInfo *Info() const;

    private:
        friend class PhysicsWorld;
        friend SharedHeightField BuildHeightField(const HeightFieldDesc &desc);

        struct Data;
        std::shared_ptr<const Data> mData;
    };

    // Any thread; mOrigin is not used. Invalid (logged) on bad input.
    SharedHeightField BuildHeightField(const HeightFieldDesc &desc);
}
