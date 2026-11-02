/**
 * @file SceneLoadGroup.h
 * @author sumin.park
 * @brief Tracks whether a scene has the resources needed to start.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mir
{
    class SceneLoadGroup
    {
    public:
        void Begin()
        {
            mReady = false;
            mDiscovered = false;
            mProgress = 0.0f;
            mReports.clear();
        }

        // A source's totals for this frame, replacing its earlier report of the same frame.
        void Report(const void *source, uint32_t required, uint32_t settled)
        {
            for (SourceReport &report : mReports)
                if (report.source == source)
                {
                    report.required = required;
                    report.settled = settled;
                    return;
                }
            mReports.push_back({ source, required, settled });
        }

        void EndFrame()
        {
            if (mReady)
                return;
            // Allow one full frame to discover required resources.
            const bool wasDiscovered = mDiscovered;
            mDiscovered = true;
            uint64_t required = 0, settled = 0;
            for (const SourceReport &report : mReports)
            {
                required += report.required;
                settled += std::min(report.settled, report.required);
            }
            mReports.clear();
            if (!wasDiscovered)
                return;
            // Newly discovered resources must not move the progress bar backwards.
            const float fraction = required == 0 ? 1.0f : static_cast<float>(settled) / static_cast<float>(required);
            mProgress = std::max(mProgress, fraction);
            mReady = settled == required;
            if (mReady)
                mProgress = 1.0f;
        }

        void ForceReady()
        {
            mReady = true;
            mProgress = 1.0f;
            mReports.clear();
        }

        bool Ready() const { return mReady; }
        // False until the work set is known, then the finished share of it. Never decreases within a load.
        bool Discovered() const { return mDiscovered; }
        float Progress() const { return mProgress; }

    private:
        struct SourceReport
        {
            const void *source = nullptr;
            uint32_t required = 0;
            uint32_t settled = 0;
        };

        std::vector<SourceReport> mReports;
        bool mReady = true;
        bool mDiscovered = true;
        float mProgress = 1.0f;
    };
}
