/**
 * @file TerrainExecutor.h
 * @author Jackey Cen
 * @brief Where terrain background work runs: a small worker pool, or a manual queue for tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace mir
{
    class JobExecutor;
    class BackgroundJobQueue;
    class TerrainExecutor
    {
    public:
        virtual ~TerrainExecutor() = default;

        // Runs on its executor, possibly inline with zero engine workers. Jobs
        // never throw and must not depend on deferred execution.
        virtual void Post(std::function<void()> job) = 0;
    };

    // Consumer-owned background lane on the engine executor, with no threads
    // of its own. The engine must outlive this adapter. Close cancels pending
    // callbacks and joins running callbacks without stopping the shared engine.
    class TerrainJobExecutor final : public TerrainExecutor
    {
    public:
        TerrainJobExecutor(JobExecutor &executor, uint32_t concurrency);
        ~TerrainJobExecutor() override;
        void Post(std::function<void()> job) override;
        void CloseAndWait();
        size_t Concurrency() const;
    private:
        std::unique_ptr<BackgroundJobQueue> mQueue;
    };

    // Jobs not started when the pool is destroyed are dropped unrun.
    class TerrainWorkerPool final : public TerrainExecutor
    {
    public:
        explicit TerrainWorkerPool(uint32_t threads);
        ~TerrainWorkerPool() override;

        TerrainWorkerPool(const TerrainWorkerPool &) = delete;
        TerrainWorkerPool &operator=(const TerrainWorkerPool &) = delete;

        void Post(std::function<void()> job) override;
        size_t ThreadCount() const
        {
            return mThreads.size();
        }

    private:
        void Work(std::stop_token stop) noexcept;

        std::mutex mMutex;
        std::condition_variable_any mWake;
        std::deque<std::function<void()>> mJobs;
        // Last, so the threads join before the queue goes.
        std::vector<std::jthread> mThreads;
    };

    // Holds jobs until a test runs them on its own thread.
    class TerrainManualExecutor final : public TerrainExecutor
    {
    public:
        void Post(std::function<void()> job) override;

        size_t Pending() const;
        // Oldest first; false when nothing was queued.
        bool RunOne();
        // Includes jobs the run jobs post.
        size_t RunAll();

    private:
        mutable std::mutex mMutex;
        std::deque<std::function<void()>> mJobs;
    };
} // namespace mir
