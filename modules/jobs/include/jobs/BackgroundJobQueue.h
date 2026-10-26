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
        std::size_t Concurrency() const;
    private:
        struct State;
        std::shared_ptr<State> mState;
    };
}
