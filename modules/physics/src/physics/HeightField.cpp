/**
 * @file HeightField.cpp
 * @author sumin.park
 * @brief Builds Jolt height field shapes from sample grids and measures what quantization cost.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/HeightField.h>

#include "HeightFieldData.h"

#include <core/log/Log.h>

#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mir
{
    namespace
    {
        static_assert(kHeightFieldHole == JPH::HeightFieldShapeConstants::cNoCollisionValue);

        // Sub shape ids hold 15 bits each of x and z.
        constexpr uint32_t kMaxHeightFieldSide = 32768;
    }

    const HeightFieldInfo *SharedHeightField::Info() const
    {
        return mData && mData->mInfo ? &*mData->mInfo : nullptr;
    }

    SharedHeightField BuildHeightField(const HeightFieldDesc &desc)
    {
        const uint32_t stride = desc.mSampleStride;
        if (desc.mCountX < 2 || desc.mCountZ < 2 ||
            desc.mSamples.size() != static_cast<std::size_t>(desc.mCountX) * desc.mCountZ)
        {
            MIR_LOG_ERROR("BuildHeightField: needs countX * countZ samples, at least 2 x 2");
            return {};
        }
        if (stride == 0 || (desc.mCountX - 1) % stride != 0 || (desc.mCountZ - 1) % stride != 0)
        {
            MIR_LOG_ERROR("BuildHeightField: count - 1 must be a multiple of the sample stride");
            return {};
        }
        const float spacing = desc.mSpacing * static_cast<float>(stride);
        if (!(desc.mSpacing > 0.0f) || !std::isfinite(spacing))
        {
            MIR_LOG_ERROR("BuildHeightField: spacing must be positive and finite");
            return {};
        }
        // Jolt 5.5's triangle walk breaks from block size 3 (terrain ADR 0017).
        if ((desc.mBlockSize != 2 && desc.mBlockSize != 4) || desc.mBitsPerSample < 1 || desc.mBitsPerSample > 8)
        {
            MIR_LOG_ERROR("BuildHeightField: block size must be 2 or 4, bits per sample in [1, 8]");
            return {};
        }
        const bool sharedRange = !std::isnan(desc.mQuantizationMin) || !std::isnan(desc.mQuantizationMax);
        if (sharedRange && !(std::isfinite(desc.mQuantizationMin) && std::isfinite(desc.mQuantizationMax) &&
                             desc.mQuantizationMin <= desc.mQuantizationMax))
        {
            MIR_LOG_ERROR("BuildHeightField: a quantization range needs finite min <= max");
            return {};
        }

        const uint32_t columns = (desc.mCountX - 1) / stride + 1;
        const uint32_t rows = (desc.mCountZ - 1) / stride + 1;
        // Jolt's square is at least two blocks; every added sample is a hole.
        const uint32_t side = std::max({columns, rows, 2 * desc.mBlockSize});
        if (side > kMaxHeightFieldSide - kMaxHeightFieldSide % desc.mBlockSize)
        {
            MIR_LOG_ERROR("BuildHeightField: {} x {} samples exceed Jolt's {} per side", columns, rows,
                          kMaxHeightFieldSide);
            return {};
        }

        // Holds Jolt registered while the shape is built, with or without a PhysicsWorld.
        auto data = std::make_shared<SharedHeightField::Data>();
        JPH::HeightFieldShapeSettings settings;
        settings.mScale = JPH::Vec3(spacing, 1.0f, spacing);
        settings.mSampleCount = side;
        settings.mBlockSize = desc.mBlockSize;
        settings.mBitsPerSample = desc.mBitsPerSample;
        if (sharedRange)
        {
            settings.mMinHeightValue = desc.mQuantizationMin;
            settings.mMaxHeightValue = desc.mQuantizationMax;
        }
        settings.mHeightSamples.resize(static_cast<std::size_t>(side) * side, kHeightFieldHole);
        for (uint32_t z = 0; z < rows; ++z)
        {
            for (uint32_t x = 0; x < columns; ++x)
            {
                const float height = desc.mSamples[static_cast<std::size_t>(z) * stride * desc.mCountX +
                                                   static_cast<std::size_t>(x) * stride];
                if (!std::isfinite(height))
                {
                    MIR_LOG_ERROR("BuildHeightField: sample ({}, {}) is not finite", x * stride, z * stride);
                    return {};
                }
                if (sharedRange && height != kHeightFieldHole &&
                    (height < desc.mQuantizationMin || height > desc.mQuantizationMax))
                {
                    MIR_LOG_ERROR("BuildHeightField: sample ({}, {}) is outside the quantization range", x * stride,
                                  z * stride);
                    return {};
                }
                settings.mHeightSamples[static_cast<std::size_t>(z) * side + x] = height;
            }
        }

        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.HasError())
        {
            MIR_LOG_ERROR("BuildHeightField: {}", result.GetError().c_str());
            return {};
        }

        data->mShape = result.Get();
        SharedHeightField built;
        if (result.Get()->GetSubType() != JPH::EShapeSubType::HeightField)
        {
            built.mData = std::move(data);
            return built;
        }
        const auto *field = static_cast<const JPH::HeightFieldShape *>(result.Get().GetPtr());
        HeightFieldInfo info{field->GetSampleCount(), field->GetStats().mSizeBytes};
        for (uint32_t z = 0; z < rows; ++z)
            for (uint32_t x = 0; x < columns; ++x)
            {
                const float input = desc.mSamples[size_t(z) * stride * desc.mCountX + size_t(x) * stride];
                if (input == kHeightFieldHole)
                    continue;
                const JPH::Vec3 decoded = field->GetPosition(x, z);
                if (field->IsNoCollision(x, z) || !std::isfinite(decoded.GetY()))
                {
                    MIR_LOG_ERROR("BuildHeightField: a valid sample decoded to a hole");
                    return {};
                }
                info.mMaxQuantizationError =
                    std::max(info.mMaxQuantizationError, std::abs(double(decoded.GetY()) - input));
                info.mMaxHorizontalError = std::max(
                    {info.mMaxHorizontalError, std::abs(double(decoded.GetX()) - double(x) * desc.mSpacing * stride),
                     std::abs(double(decoded.GetZ()) - double(z) * desc.mSpacing * stride)});
                ++info.mValidSamples;
            }
        info.mExtentX = field->GetPosition(columns - 1, 0).GetX();
        info.mExtentZ = field->GetPosition(0, rows - 1).GetZ();
        data->mInfo = info;
        built.mData = std::move(data);
        return built;
    }
}
