/**
 * @file Requests.h
 * @author sumin.park
 * @brief Transitions asked for during a frame, applied by App at the top of the next
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace mir
{
    enum class Request : uint8_t
    {
        Play,
        Stop,
        Pause,
        Resume,
        NewScene,
        LoadScene,
        SaveScene,
        ApplyImportedAssets,
        Quit,
    };

    // World resource. Applied where nothing is drawn or running, so a transition never
    // frees what this frame's draw data or systems still hold. Not scene state: Play,
    // Stop and Load keep it
    class RequestQueue
    {
    public:
        // a request already pending is dropped
        void Push(Request request)
        {
            if (std::find(mPending.begin(), mPending.end(), request) == mPending.end())
                mPending.push_back(request);
        }

        // in push order, Quit last so every other request still applies before the close.
        // Empties the queue first: requests pushed while these apply wait for the next frame
        std::vector<Request> Take()
        {
            std::vector<Request> taken = std::exchange(mPending, {});
            std::stable_partition(taken.begin(), taken.end(), [](Request request) { return request != Request::Quit; });
            return taken;
        }

        bool Empty() const { return mPending.empty(); }

    private:
        std::vector<Request> mPending;
    };
}
