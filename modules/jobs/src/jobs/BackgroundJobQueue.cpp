#include <jobs/BackgroundJobQueue.h>
#include <jobs/JobExecutor.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mir
{
    struct BackgroundJobQueue::State : std::enable_shared_from_this<State>
    {
        JobExecutor &executor;
        const std::size_t limit;
        const bool inlineExecution;
        std::mutex mutex;
        std::condition_variable complete;
        std::deque<std::function<void()>> pending;
        std::size_t scheduled = 0, running = 0, submitting = 0;
        bool closed = false;

        State(JobExecutor &jobs, std::size_t concurrency)
            : executor(jobs), limit(std::min(concurrency, std::max(std::size_t(1), jobs.WorkerCount()))),
            inlineExecution(jobs.WorkerCount() == 0)
        {}

        void Schedule()
        {
            try { executor.Post([self = shared_from_this()] { self->RunOne(); }); }
            catch (...)
            {
                std::lock_guard lock(mutex);
                --scheduled;
                --submitting;
                complete.notify_all();
                throw;
            }
            std::lock_guard lock(mutex);
            --submitting;
            complete.notify_all();
        }
        void RunOne() noexcept
        {
            for (;;)
            {
                std::function<void()> function;
                {
                    std::lock_guard lock(mutex);
                    if (closed || pending.empty()) { --scheduled; return; }
                    function = std::move(pending.front());
                    pending.pop_front();
                    ++running;
                }
                function();
                // Destroy captures before publishing completion to the owner.
                function = {};
                bool again;
                {
                    std::lock_guard lock(mutex);
                    --running;
                    again = !closed && !pending.empty();
                    if (!again) --scheduled;
                    else if (!inlineExecution) ++submitting;
                    if (running == 0) complete.notify_all();
                }
                // Resubmit each callback so frame batches can run between callbacks.
                if (!again) return;
                if (inlineExecution) continue;
                Schedule();
                return;
            }
        }
    };

    BackgroundJobQueue::BackgroundJobQueue(JobExecutor &executor, std::size_t concurrency)
    {
        if (concurrency == 0) throw std::invalid_argument("background concurrency must be positive");
        mState = std::make_shared<State>(executor, concurrency);
    }
    BackgroundJobQueue::~BackgroundJobQueue() { CloseAndWait(); }
    void BackgroundJobQueue::Post(std::function<void()> function)
    {
        bool schedule = false;
        {
            std::lock_guard lock(mState->mutex);
            if (mState->closed) return;
            mState->pending.push_back(std::move(function));
            if (mState->scheduled < mState->limit)
            {
                ++mState->scheduled;
                ++mState->submitting;
                schedule = true;
            }
        }
        if (schedule) mState->Schedule();
    }
    void BackgroundJobQueue::CloseAndWait()
    {
        std::deque<std::function<void()>> discarded;
        std::unique_lock lock(mState->mutex);
        mState->closed = true;
        discarded.swap(mState->pending);
        mState->complete.wait(lock, [&] { return mState->running == 0 && mState->submitting == 0; });
    }
    std::size_t BackgroundJobQueue::Concurrency() const { return mState->limit; }
}
