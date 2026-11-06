/**
 * @file StartupGate.h
 * @author Rahul Nair
 * @brief Decides when startup has waited long enough for what the first view needs to load.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>

namespace mir
{
    class StartupGate
    {
    public:
        // Nothing pending for this many updates: UI layout can add views in its first frames.
        static constexpr int kSettledUpdates = 8;
        // No new low in pending for this long means it will not finish, such as past a budget or after a failure.
        static constexpr double kStallSeconds = 10.0;
        static constexpr double kMaxSeconds = 60.0;

        enum class Result
        {
            Waiting,
            Ready,
            Stalled,
            TimedOut,
        };

        // pending: what the views cannot show yet, nullopt before any view has asked for anything;
        // seconds: any clock that only moves forward
        Result Update(std::optional<size_t> pending, double seconds)
        {
            if (!mStart.has_value())
                mStart = mLowSince = seconds;

            if (pending.has_value())
            {
                mPeak = std::max(mPeak, *pending);
                mPending = *pending;

                // work after none pending is new work, so it gets its own baseline
                if (!mLow.has_value() || *pending < *mLow || *mLow == 0)
                {
                    mLow = *pending;
                    mLowSince = seconds;
                }
            }

            mSettled = pending == 0 ? mSettled + 1 : 0;
            if (mSettled >= kSettledUpdates)
                return Result::Ready;
            if (seconds - *mStart >= kMaxSeconds)
                return Result::TimedOut;
            // zero still needs the updates above
            if (pending != 0 && seconds - *mLowSince >= kStallSeconds)
                return Result::Stalled;
            return Result::Waiting;
        }

        size_t Pending() const { return mPending; }

        // 0 at the most that was pending, 1 at none
        float Progress() const
        {
            return mPeak == 0 ? 1.0f : 1.0f - static_cast<float>(mPending) / static_cast<float>(mPeak);
        }

    private:
        std::optional<double> mStart;
        std::optional<double> mLowSince;
        std::optional<size_t> mLow;
        size_t mPeak = 0;
        size_t mPending = 0;
        int mSettled = 0;
    };
} // namespace mir
