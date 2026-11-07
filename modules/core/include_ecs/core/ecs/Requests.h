/**
 * @file Requests.h
 * @author sumin.park
 * @brief Transitions asked for during a frame, applied by App at the top of the next
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "SceneInstance.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
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

    struct OpenSceneRequest
    {
        std::string id;
        bool saveCurrent = false;
        std::string newScenePath;
    };

    struct AdditiveSceneRequest
    {
        std::string id;
        SceneInstanceId instance = kOwnedBySession;
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

        // In push order, and Quit runs last in its frame, so a Save pushed with it still saves.
        // Empties the queue first: requests pushed while these apply go to the next frame,
        // which after a Quit never runs, so nothing runs after a Quit
        std::vector<Request> Take()
        {
            std::vector<Request> taken = std::exchange(mPending, {});
            std::stable_partition(taken.begin(), taken.end(), [](Request request) { return request != Request::Quit; });
            return taken;
        }

        bool Empty() const
        {
            return mPending.empty() && !mOpenProject.has_value() && !mOpenScene.has_value() &&
                !mNextScene.has_value() && mAdditiveScenes.empty() && mUnloadScenes.empty() && !mPrimaryScene;
        }

// What was pending when a span of time began, kept apart so what is pushed during it can be told from it.
        struct Held
        {
            std::vector<Request> requests;
            std::optional<std::string> openProject;
            std::optional<OpenSceneRequest> openScene;
            std::optional<std::string> nextScene;
            std::vector<AdditiveSceneRequest> additiveScenes;
            std::vector<SceneInstanceId> unloadScenes;
            std::optional<SceneInstanceId> primaryScene;
        };
        Held Hold()
        {
            return { std::exchange(mPending,{}), std::exchange(mOpenProject, std::nullopt),
                    std::exchange(mOpenScene, std::nullopt), std::exchange(mNextScene, std::nullopt),
                    std::exchange(mAdditiveScenes, {}), std::exchange(mUnloadScenes, {}),
                    std::exchange(mPrimaryScene, std::nullopt) };
        }
// Drops everything pushed since Hold and brings back what was held. Returns how many were dropped.
        size_t Restore(Held held)
        {
            const size_t dropped = mPending.size() + mOpenProject.has_value() + mOpenScene.has_value() +
                mNextScene.has_value() + mAdditiveScenes.size() + mUnloadScenes.size() + mPrimaryScene.has_value();
            mPending = std::move(held.requests);
            mOpenProject = std::move(held.openProject);
            mOpenScene = std::move(held.openScene);
            mNextScene = std::move(held.nextScene);
            mAdditiveScenes = std::move(held.additiveScenes);
            mUnloadScenes = std::move(held.unloadScenes);
            mPrimaryScene = held.primaryScene;
            return dropped;
        }

        // a game directory to switch to; the last one pushed in a frame wins
        void PushOpenProject(std::string dir) { mOpenProject = std::move(dir); }
        std::optional<std::string> TakeOpenProject() { return std::exchange(mOpenProject, std::nullopt); }

        void PushOpenScene(OpenSceneRequest request) { mOpenScene = std::move(request); }
        std::optional<OpenSceneRequest> TakeOpenScene() { return std::exchange(mOpenScene, std::nullopt); }

        void PushNextScene(std::string id) { mNextScene = std::move(id); }
        std::optional<std::string> TakeNextScene() { return std::exchange(mNextScene, std::nullopt); }

        void PushAdditiveScene(AdditiveSceneRequest request) { mAdditiveScenes.push_back(std::move(request)); }
        std::vector<AdditiveSceneRequest> TakeAdditiveScenes() { return std::exchange(mAdditiveScenes, {}); }
        void PushUnloadScene(SceneInstanceId instance)
        {
            if (std::find(mUnloadScenes.begin(), mUnloadScenes.end(), instance) == mUnloadScenes.end())
                mUnloadScenes.push_back(instance);
        }
        std::vector<SceneInstanceId> TakeUnloadScenes() { return std::exchange(mUnloadScenes, {}); }
        void PushPrimaryScene(SceneInstanceId instance) { mPrimaryScene = instance; }
        std::optional<SceneInstanceId> TakePrimaryScene() { return std::exchange(mPrimaryScene, std::nullopt); }

    private:
        std::vector<Request> mPending;
        std::optional<std::string> mOpenProject;
        std::optional<OpenSceneRequest> mOpenScene;
        std::optional<std::string> mNextScene;
        std::vector<AdditiveSceneRequest> mAdditiveScenes;
        std::vector<SceneInstanceId> mUnloadScenes;
        std::optional<SceneInstanceId> mPrimaryScene;
    };
}
