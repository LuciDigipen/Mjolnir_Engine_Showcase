/**
 * @file SceneLoadGroup.h
 * @author sumin.park
 * @brief The resources a scene needs before it can run, reported by the systems that prepare them.
 *
 * A scene load starts a group. Each frame every system that prepares resources reports how many bindings
 * the scene requires and how many have settled, as a usable resource or a terminal failure. The group is
 * ready once a frame has been reported in full and nothing is left unsettled. It never looks at global
 * asset activity, so thumbnails and other Worlds do not delay it, and it stays ready until the next load.
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
        // A new scene load, nothing is known about its work yet.
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

        // After a frame's systems ran. Decides readiness from what was reported this frame.
        void EndFrame()
        {
            if (mReady)
                return;
            // The first frame after Begin enumerates the work, later frames can only add what scripts create.
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
            // Progress only moves forward, work found late must not walk the bar back.
            const float fraction = required == 0 ? 1.0f : static_cast<float>(settled) / static_cast<float>(required);
            mProgress = std::max(mProgress, fraction);
            mReady = settled == required;
            if (mReady)
                mProgress = 1.0f;
        }

        // Ends the wait without the work, for a caller that gave up on it.
        void ForceReady()
        {
            mReady = true;
            mProgress = 1.0f;
            mReports.clear();
        }

        bool Ready() const { return mReady; }
        // False until the work set is known, then the settled share of it. Never decreases within a load.
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
