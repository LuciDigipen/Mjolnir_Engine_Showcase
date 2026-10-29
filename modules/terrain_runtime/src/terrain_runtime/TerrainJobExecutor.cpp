/**
 * @file TerrainJobExecutor.cpp
 * @author Rahul Nair
 * @brief Adapts the engine worker service to shared terrain background work.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain_runtime/TerrainJobExecutor.h"

#include <jobs/BackgroundJobQueue.h>

#include <utility>

namespace mir
{
    TerrainJobExecutor::TerrainJobExecutor(JobExecutor &executor, uint32_t concurrency)
        : mQueue(std::make_unique<BackgroundJobQueue>(executor, concurrency)) {}
    TerrainJobExecutor::~TerrainJobExecutor() = default;
    void TerrainJobExecutor::Post(std::function<void()> job) { mQueue->Post(std::move(job)); }
    void TerrainJobExecutor::WaitIdle() { mQueue->WaitIdle(); }
    size_t TerrainJobExecutor::Concurrency() const { return mQueue->Concurrency(); }

}
