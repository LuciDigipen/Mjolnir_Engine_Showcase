/**
 * @file TerrainBakeJob.h
 * @author Jackey Cen
 * @brief One TerrainTool sync at a time, run in the background and polled once a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainBake.h>

#include <assets/AssetId.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    class TerrainLayerDocument;

    class TerrainBakeProcess
    {
    public:
        virtual ~TerrainBakeProcess() = default;

        virtual bool Start(const std::filesystem::path &program, const std::vector<std::string> &arguments) = 0;
        virtual std::optional<int> Poll() = 0;
        virtual void Terminate() = 0;
    };

    using TerrainBakeProcessFactory = std::function<std::unique_ptr<TerrainBakeProcess>()>;

    std::unique_ptr<TerrainBakeProcess> MakeChildTerrainBakeProcess();

    // Saves the Edit Layers; false with error set keeps the process from starting.
    using TerrainBakePrepare = std::function<bool(std::string &error)>;

    TerrainBakePrepare SaveTerrainLayersForBake(TerrainLayerDocument &document);

    struct TerrainBakeStart
    {
        TerrainBakeRequest mRequest;
        TerrainBakePrepare mPrepare;
        uint64_t mStartChange = 0;
    };

    struct TerrainBakeResult
    {
        TerrainBakeReport mReport;
        AssetId mAsset;
        uint64_t mStartChange = 0;
    };

    class TerrainBakeJob
    {
    public:
        explicit TerrainBakeJob(TerrainBakeProcessFactory factory = MakeChildTerrainBakeProcess);
        ~TerrainBakeJob();

        TerrainBakeJob(const TerrainBakeJob &) = delete;
        TerrainBakeJob &operator=(const TerrainBakeJob &) = delete;

        // False when a bake is already running, the layers could not be saved, or TerrainTool could not start.
        bool Start(TerrainBakeStart start);
        void Update();
        void Cancel();

        bool Running() const { return mRunning; }
        double Seconds() const;
        // Grows by one each time a bake ends, so the panel notices a finished bake once.
        uint64_t Completed() const { return mCompleted; }
        const std::optional<TerrainBakeResult> &Result() const { return mResult; }
        const std::string &Message() const;

    private:
        void Finish(TerrainBakeReport report);

        TerrainBakeProcessFactory mFactory;
        std::unique_ptr<TerrainBakeProcess> mProcess;
        TerrainBakeRequest mRequest;
        std::filesystem::path mReportFile;
        std::optional<TerrainBakeResult> mResult;
        std::chrono::steady_clock::time_point mStarted;
        uint64_t mStartChange = 0;
        uint64_t mCompleted = 0;
        bool mRunning = false;
    };
} // namespace mir
