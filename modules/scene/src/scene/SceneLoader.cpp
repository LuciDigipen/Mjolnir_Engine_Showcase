/**
 * @file SceneLoader.cpp
 * @author sumin.park
 * @brief Prepares scene files on the shared worker pool without touching a World.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/SceneLoader.h"

#include <jobs/BackgroundJobQueue.h>

#include <chrono>
#include <exception>
#include <future>
#include <stop_token>
#include <utility>

namespace mir
{
    struct SceneLoader::Impl
    {
        explicit Impl(JobExecutor &jobs) : queue(jobs, 1) {}
        BackgroundJobQueue queue;
        std::stop_source cancellation;
        std::future<SceneLoadCompletion> result;
        SceneInstanceId source = kOwnedBySession;
    };

    SceneLoader::SceneLoader(JobExecutor &jobs) : mImpl(std::make_unique<Impl>(jobs)) {}
    SceneLoader::~SceneLoader() { Cancel(); }

    void SceneLoader::Cancel()
    {
        mImpl->cancellation.request_stop();
        mImpl->result = {};
        mImpl->source = kOwnedBySession;
    }

    bool SceneLoader::Pending() const { return mImpl->result.valid(); }

    void SceneLoader::Request(std::string id, std::filesystem::path directory, SceneInstanceId source)
    {
        Cancel();
        mImpl->cancellation = std::stop_source{};
        mImpl->source = source;
        const auto token = mImpl->cancellation.get_token();
        mImpl->result = mImpl->queue.Submit(
            [id = std::move(id), directory = std::move(directory), source, token]
            {
                SceneLoadCompletion completed{ id, directory, source };
                try { completed.prepared = PrepareScene(directory, token); }
                catch (const std::exception &error) { completed.error = error.what(); }
                return completed;
            });
    }

    std::optional<SceneLoadCompletion> SceneLoader::TakeReady(SceneInstanceId currentSource)
    {
        if (!Pending()) return std::nullopt;
        if (currentSource != mImpl->source)
        {
            Cancel();
            return std::nullopt;
        }
        if (mImpl->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return std::nullopt;
        SceneLoadCompletion completed = mImpl->result.get();
        if (completed.prepared.cancelled) return std::nullopt;
        return completed;
    }

}
