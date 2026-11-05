/**
 * @file SceneLoader.h
 * @author sumin.park
 * @brief Owns cancellable background preparation of the next scene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <scene/SceneIO.h>

#include <memory>
#include <optional>
#include <string>

namespace mir
{
    class JobExecutor;

    struct SceneLoadCompletion
    {
        std::string id;
        std::filesystem::path directory;
        SceneInstanceId source = kOwnedBySession;
        PreparedScene prepared;
        std::string error;
    };

    class SceneLoader
    {
    public:
        explicit SceneLoader(JobExecutor &jobs);
        ~SceneLoader();
        SceneLoader(const SceneLoader &) = delete;
        SceneLoader &operator=(const SceneLoader &) = delete;

        // Main thread only. The latest request replaces and cancels the previous one.
        void Request(std::string id, std::filesystem::path directory, SceneInstanceId source);
        std::optional<SceneLoadCompletion> TakeReady(SceneInstanceId currentSource);
        void Cancel();
        bool Pending() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
