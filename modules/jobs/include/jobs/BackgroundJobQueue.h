/**
 * @file BackgroundJobQueue.h
 * @author Rahul Nair
 * @brief A consumer-owned background lane on the shared engine executor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace mir
{
    class JobExecutor;
    // A consumer-owned background lane on an existing executor. No extra threads.
    // The executor must outlive the queue. Callbacks must not throw or wait for
    // jobs in this lane. Close/destruction must be called outside its callbacks.
    class BackgroundJobQueue
    {
    public:
        BackgroundJobQueue(JobExecutor &executor, std::size_t concurrency);
        ~BackgroundJobQueue();
        BackgroundJobQueue(const BackgroundJobQueue &) = delete;
        BackgroundJobQueue &operator=(const BackgroundJobQueue &) = delete;
        // After CloseAndWait, posts are dropped. Zero executor workers runs inline.
        void Post(std::function<void()> function);
        // Cancel callbacks not yet started and wait only for this lane's running
        // callbacks. The shared executor and other consumers remain available.
        void CloseAndWait();
        // Finish submitted callbacks without cancelling them or closing the lane.
        // Call outside callbacks, with external producers stopped while waiting.
        void WaitIdle();
        std::size_t Concurrency() const;
    private:
        struct State;
        std::shared_ptr<State> mState;
    };
}
