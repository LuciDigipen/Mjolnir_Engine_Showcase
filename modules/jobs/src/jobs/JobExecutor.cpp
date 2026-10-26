#include <jobs/JobExecutor.h>
#include <core/platform/CrashHandler.h>
#include <core/log/SetupError.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mir
{
    struct JobExecutor::Impl
    {
        using Function = std::function<void(std::size_t, std::size_t)>;
        inline static thread_local Impl *sCurrent = nullptr;
        std::mutex mMutex, mDispatchMutex;
        std::condition_variable_any mWake;
        std::condition_variable mComplete;
        std::size_t mNext = 0;
        std::size_t mCount = 0, mGrain = 1, mBatches = 0, mRemaining = 0;
        const Function *mFunction = nullptr;
        std::exception_ptr mError;
        std::deque<std::function<void()>> mBackground;
        struct CurrentScope
        {
            Impl *previous;
            explicit CurrentScope(Impl *self) : previous(sCurrent) { sCurrent = self; }
            ~CurrentScope() { sCurrent = previous; }
        };
        struct Range { const Function *function; std::size_t begin, end; };
        // Last: even partial-construction unwinding joins before state dies.
        std::vector<std::jthread> mWorkers;

        explicit Impl(std::size_t workers)
        {
            mWorkers.reserve(workers);
            for (std::size_t i = 0; i < workers; ++i)
                mWorkers.emplace_back([this](std::stop_token stop) noexcept { Work(stop); });
        }
        ~Impl()
        {
            for (auto &worker : mWorkers) worker.request_stop();
            mWake.notify_all();
            mWorkers.clear();
        }
        static void Inline(std::size_t count, std::size_t grain, const Function &function)
        {
            std::exception_ptr error;
            for (std::size_t begin = 0; begin < count;)
            {
                const std::size_t end = begin + std::min(grain, count - begin);
                try { function(begin, end); }
                catch (...) { if (!error) error = std::current_exception(); }
                begin = end;
            }
            if (error) std::rethrow_exception(error);
        }
        // Requires mMutex. Completion tracks ranges, never unrelated background
        // workers, so a frame join does not wait for all pending streaming work.
        bool TakeRange(Range &range)
        {
            if (mNext >= mBatches) return false;
            const std::size_t begin = mNext++ * mGrain;
            range = { mFunction, begin, begin + std::min(mGrain, mCount - begin) };
            return true;
        }
        void Execute(Range range) noexcept
        {
            std::exception_ptr error;
            {
                CurrentScope current(this);
                try { (*range.function)(range.begin, range.end); }
                catch (...) { error = std::current_exception(); }
            }
            std::lock_guard lock(mMutex);
            if (error && !mError) mError = error;
            if (--mRemaining == 0) mComplete.notify_one();
        }
        void Work(std::stop_token stop) noexcept
        {
            ArmCrashHandler();
            for (;;)
            {
                Range range{};
                std::function<void()> background;
                std::unique_lock lock(mMutex);
                if (!mWake.wait(lock, stop, [&] { return mNext < mBatches || !mBackground.empty(); }) || stop.stop_requested()) return;
                const bool haveRange = TakeRange(range);
                if (!haveRange)
                {
                    background = std::move(mBackground.front());
                    mBackground.pop_front();
                }
                lock.unlock();
                if (haveRange) Execute(range);
                else
                {
                    CurrentScope current(this);
                    background(); // noexcept worker entry preserves fatal callback policy
                }
            }
        }
        void Post(std::function<void()> function)
        {
            if (mWorkers.empty())
            {
                CurrentScope current(this);
                try { function(); }
                catch (...) { std::terminate(); }
                return;
            }
            {
                std::lock_guard lock(mMutex);
                mBackground.push_back(std::move(function));
            }
            mWake.notify_one();
        }
        void Run(std::size_t count, std::size_t grain, const Function &function)
        {
            if (grain == 0) throw std::invalid_argument("JobExecutor grain must be positive");
            if (count == 0) return;
            if (mWorkers.empty() || count <= grain || sCurrent == this)
            {
                Inline(count, grain, function);
                return;
            }
            std::lock_guard dispatch(mDispatchMutex);
            {
                std::lock_guard lock(mMutex);
                mFunction = &function;
                mCount = count;
                mGrain = grain;
                mBatches = count / grain + (count % grain != 0);
                mNext = 0;
                mRemaining = mBatches;
                mError = nullptr;
            }
            mWake.notify_all();
            for (;;)
            {
                Range range{};
                {
                    std::lock_guard lock(mMutex);
                    if (!TakeRange(range)) break;
                }
                Execute(range);
            }
            std::exception_ptr error;
            {
                std::unique_lock lock(mMutex);
                mComplete.wait(lock, [&] { return mRemaining == 0; });
                mFunction = nullptr;
                error = std::exchange(mError, nullptr);
            }
            if (error) std::rethrow_exception(error);
        }
    };
    JobExecutor::JobExecutor(std::size_t workers) try : mImpl(std::make_unique<Impl>(workers)) {}
    catch (const std::system_error &error)
    {
        throw SetupError(ErrorCategory::System, "The engine worker threads could not start", {}, error.what(),
            "Use --workers 0 or reduce the worker count and restart.");
    }
    JobExecutor::~JobExecutor() = default;
    std::size_t JobExecutor::DefaultWorkerCount()
    {
        const auto threads = std::thread::hardware_concurrency();
        return threads > 1 ? std::min<std::size_t>(threads - 1, 4) : 0;
    }
    std::size_t JobExecutor::WorkerCount() const { return mImpl->mWorkers.size(); }
    void JobExecutor::Post(std::function<void()> function) { mImpl->Post(std::move(function)); }
    void JobExecutor::ParallelFor(std::size_t count, std::size_t grain,
        const std::function<void(std::size_t, std::size_t)> &function)
    {
        mImpl->Run(count, grain, function);
    }
}
