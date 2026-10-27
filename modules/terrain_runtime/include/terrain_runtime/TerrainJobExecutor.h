/**
 * @file TerrainJobExecutor.h
 * @author Rahul Nair
 * @brief Adapts the engine worker service to shared terrain background work.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainExecutor.h>

#include <memory>

namespace mir
{
    class JobExecutor;
    class BackgroundJobQueue;

    // Shared by the streamer and runtime consumers; the engine must outlive them.
    // Only the last owner closes the lane. Runtime teardown waits without closing
    // it, so a retained streamer can finish reads and submit new work.
    class TerrainJobExecutor final : public TerrainExecutor
    {
    public:
        TerrainJobExecutor(JobExecutor &executor, uint32_t concurrency);
        ~TerrainJobExecutor() override;
        void Post(std::function<void()> job) override;
        void WaitIdle();
        size_t Concurrency() const;
    private:
        std::unique_ptr<BackgroundJobQueue> mQueue;
    };

}
