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
    // Calls use validated, nonzero counts. The batch maps dispatch Y to
    // (first + Y) % total, so every probe is visited before history is ready.
    class ProbeUpdateSchedule
    {
    public:
        std::array<uint32_t, 4> Batch(uint32_t total, uint32_t count) const
            { return { mFirst, count, total, mVisited >= total ? 1u : 0u }; }
        void Reset() { mFirst = mVisited = 0; }
        void Advance(uint32_t total, uint32_t count)
        {
            mFirst = (mFirst + count) % total;
            mVisited = mVisited + count < total ? mVisited + count : total;
        }
    private:
        uint32_t mFirst = 0, mVisited = 0;
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
        bool Record(VkCommandBuffer cmd, uint32_t frame, WorldPosition camera,
            VkDescriptorSet cameraSet, uint32_t cameraOffset, const RayTracer::PreparedScene &scene, bool complete);
        void Shutdown(); // Caller has idled the device.

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
