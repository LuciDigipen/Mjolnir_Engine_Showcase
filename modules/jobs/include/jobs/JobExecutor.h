#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace mir
{
    // Shared worker service. Range dispatch participates on the caller and returns
    // only after every range has finished. Callbacks must own disjoint outputs.
    // Zero workers is the serial backend. Nested dispatch to this executor runs
    // inline; concurrent external dispatches serialize. Destroy only when idle.
    class JobExecutor
    {
    public:
        explicit JobExecutor(std::size_t workers = 0);
        // Leave a core for the caller; bound the default pool on large machines.
        static std::size_t DefaultWorkerCount();
        ~JobExecutor();
        JobExecutor(const JobExecutor &) = delete;
        JobExecutor &operator=(const JobExecutor &) = delete;
        std::size_t WorkerCount() const;
        // Background callbacks must not throw or borrow objects that die before
        // they finish. Prefer BackgroundJobQueue for consumer lifetime/cancellation.
        // Zero workers executes inline. Ready range batches take queue priority.
        void Post(std::function<void()> function);
        // Exceptions are rethrown on the caller after all other ranges drain.
        void ParallelFor(std::size_t count, std::size_t grain,
            const std::function<void(std::size_t begin, std::size_t end)> &function);
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
