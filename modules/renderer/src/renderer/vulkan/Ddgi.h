/**
 * @file Ddgi.h
 * @author Rahul Nair
 * @brief Renderer-owned DDGI volume, probe pipelines and persistent history.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "RayTracer.h"
#include <renderer/RenderSettings.h>
#include <memory>

namespace mir::vk
{
    // Validated nonzero counts; dispatch Y maps to (first + Y) % total.
    class ProbeUpdateSchedule
    {
    public:
        std::array<uint32_t, 4> Batch(uint32_t total, uint32_t count) const
            { return { mFirst, count, total, mVisited >= total ? 1u : 0u }; }
        void Reset() { mFirst = mVisited = mReadyFrames = mMissedFrames = 0; }
        void SkipUpdate() { if (!Expired()) ++mMissedFrames; }
        bool Expired() const { return mMissedFrames >= kHoldFrames + kFadeFrames; }
        void Advance(uint32_t total, uint32_t count)
        {
            mFirst = (mFirst + count) % total;
            mVisited = mVisited + count < total ? mVisited + count : total;
            if (mVisited >= total && mReadyFrames < 8) ++mReadyFrames;
            if (mMissedFrames > 0) --mMissedFrames;
        }
        // Eight successful updates fade into sampling after the first full sweep.
        float SamplingWeight() const
        {
            const float freshness = mMissedFrames <= kHoldFrames ? 1.0f :
                float(kHoldFrames + kFadeFrames - mMissedFrames) / float(kFadeFrames);
            return float(mReadyFrames) / 8.0f * freshness;
        }
    private:
        static constexpr uint32_t kHoldFrames = 8, kFadeFrames = 24;
        uint32_t mFirst = 0, mVisited = 0, mReadyFrames = 0, mMissedFrames = 0;
    };

    class Ddgi
    {
    public:
        Ddgi();
        ~Ddgi();
        void Initialize(Device &device, VkDescriptorSetLayout sceneLayout, VkDescriptorSetLayout cameraLayout);
        // Before BeginFrame: waits only when replacing/freeing GPU resources.
        void Configure(const DdgiSettings &settings, bool active);
        void ResetHistory();
        bool CanUpdate(WorldPosition camera) const;
        std::optional<RayTracer::GiShading> Record(VkCommandBuffer cmd, uint32_t frame, WorldPosition camera,
            VkDescriptorSet cameraSet, uint32_t cameraOffset, const RayTracer::PreparedScene &scene, bool complete);
        void Shutdown(); // Caller has idled the device.

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
