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
#include <map>
#include <stop_token>
#include <utility>

namespace mir
{
    struct SceneLoader::Impl
    {
        explicit Impl(JobExecutor &jobs) : queue(jobs, 1) {}
        struct PendingLoad
        {
            std::stop_source cancellation;
            std::future<SceneLoadCompletion> result;
            SceneInstanceId source = kOwnedBySession;
        };
        BackgroundJobQueue queue;
        std::stop_source cancellation;
        std::future<SceneLoadCompletion> result;
        SceneInstanceId source = kOwnedBySession;
        std::map<SceneInstanceId, PendingLoad> additive;

        std::future<SceneLoadCompletion> Submit(std::string id, std::filesystem::path directory,
            SceneInstanceId source, std::stop_token token, SceneInstanceId instance = kOwnedBySession)
        {
            return queue.Submit([id = std::move(id), directory = std::move(directory), source, token, instance]
                {
                    SceneLoadCompletion completed{ id, directory, source };
                    completed.instance = instance;
                    try { completed.prepared = PrepareScene(directory, token); }
                    catch (const std::exception &error) { completed.error = error.what(); }
                    return completed;
                });
        }
    };

    SceneLoader::SceneLoader(JobExecutor &jobs) : mImpl(std::make_unique<Impl>(jobs)) {}
    SceneLoader::~SceneLoader() { Cancel(); }

    void SceneLoader::Cancel()
    {
        for (auto &[instance, load] : mImpl->additive) load.cancellation.request_stop();
        mImpl->additive.clear();
        mImpl->cancellation.request_stop();
        mImpl->result = {};
        mImpl->source = kOwnedBySession;
    }

    bool SceneLoader::Pending() const { return mImpl->result.valid() || !mImpl->additive.empty(); }

    void SceneLoader::Request(std::string id, std::filesystem::path directory, SceneInstanceId source)
    {
        Cancel();
        mImpl->cancellation = std::stop_source{};
        mImpl->source = source;
        const auto token = mImpl->cancellation.get_token();
        mImpl->result = mImpl->Submit(std::move(id), std::move(directory), source, token);
    }

    void SceneLoader::RequestAdditive(std::string id, std::filesystem::path directory,
        SceneInstanceId source, SceneInstanceId instance)
    {
        if (instance == kOwnedBySession) return;
        Cancel(instance);
        auto &load = mImpl->additive[instance];
        load.source = source;
        load.result = mImpl->Submit(std::move(id), std::move(directory), source,
            load.cancellation.get_token(), instance);
    }

    bool SceneLoader::HasAdditive(SceneInstanceId instance) const { return mImpl->additive.contains(instance); }

    void SceneLoader::Cancel(SceneInstanceId instance)
    {
        const auto found = mImpl->additive.find(instance);
        if (found == mImpl->additive.end()) return;
        found->second.cancellation.request_stop();
        mImpl->additive.erase(found);
    }

    std::vector<SceneLoadCompletion> SceneLoader::TakeReadyAdditive(SceneInstanceId currentSource)
    {
        std::vector<SceneLoadCompletion> ready;
        for (auto it = mImpl->additive.begin(); it != mImpl->additive.end();)
        {
            auto &load = it->second;
            if (load.source != currentSource)
            {
                load.cancellation.request_stop();
                it = mImpl->additive.erase(it);
            }
            else if (load.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                auto completed = load.result.get();
                if (!completed.prepared.cancelled) ready.push_back(std::move(completed));
                it = mImpl->additive.erase(it);
            }
            else ++it;
        }
        return ready;
    }

    std::optional<SceneLoadCompletion> SceneLoader::TakeReady(SceneInstanceId currentSource)
    {
        if (!mImpl->result.valid()) return std::nullopt;
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
