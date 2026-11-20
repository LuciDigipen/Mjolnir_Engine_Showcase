/**
 * @file TerrainBakeJob.cpp
 * @author Jackey Cen
 * @brief One TerrainTool sync at a time, run in the background and polled once a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainBakeJob.h>

#include <terrain_editor/TerrainLayerDocument.h>

#include <core/platform/Process.h>

#include <atomic>
#include <format>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        class ChildTerrainBakeProcess final : public TerrainBakeProcess
        {
        public:
            bool Start(const std::filesystem::path &program, const std::vector<std::string> &arguments) override
            {
                return mProcess.Start(program, arguments);
            }

            std::optional<int> Poll() override { return mProcess.Poll(); }
            void Terminate() override { mProcess.Terminate(); }

        private:
            ChildProcess mProcess;
        };

        std::string PathText(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return { reinterpret_cast<const char *>(text.data()), text.size() };
        }

        std::filesystem::path NextReportFile(const std::filesystem::path &directory, std::error_code &ec)
        {
            static std::atomic<uint64_t> sequence{0};
            std::filesystem::create_directories(directory, ec);
            while (!ec)
            {
                const auto run = directory / std::format("sync-{}-{}", CurrentProcessId(), sequence++);
                if (std::filesystem::create_directory(run, ec))
                    return run / "report.json";
            }
            return {};
        }
    } // namespace

    std::unique_ptr<TerrainBakeProcess> MakeChildTerrainBakeProcess()
    {
        return std::make_unique<ChildTerrainBakeProcess>();
    }

    TerrainBakePrepare SaveTerrainLayersForBake(TerrainLayerDocument &document)
    {
        return [&document](std::string &error)
        {
            if (!document.Unsaved() || document.Save())
                return true;
            error = document.SaveError();
            return false;
        };
    }

    TerrainBakeJob::TerrainBakeJob(TerrainBakeProcessFactory factory) : mFactory(std::move(factory))
    {
    }

    TerrainBakeJob::~TerrainBakeJob()
    {
        if (mRunning && mProcess)
            Cancel();
    }

    bool TerrainBakeJob::Start(TerrainBakeStart start)
    {
        if (mRunning)
            return false;

        mRequest = std::move(start.mRequest);
        mStartChange = start.mStartChange;
        mResult.reset();
        mStarted = std::chrono::steady_clock::now();

        if (start.mPrepare)
        {
            std::string error;
            if (!start.mPrepare(error))
            {
                TerrainBakeReport report;
                report.mOutcome = TerrainBakeOutcome::Failed;
                report.mMessage = error.empty() ? "The Edit Layers could not be saved." : error;
                Finish(std::move(report));
                return false;
            }
        }

        if (mRequest.mTool.empty())
        {
            TerrainBakeReport report;
            report.mOutcome = TerrainBakeOutcome::Failed;
            report.mMessage = "the editor does not know where TerrainTool is; rebuild the editor";
            Finish(std::move(report));
            return false;
        }

        std::error_code ec;
        const auto reportFile = NextReportFile(mRequest.mReportDirectory, ec);
        if (ec)
        {
            TerrainBakeReport report;
            report.mMessage = "Cannot select a TerrainTool report file: " + ec.message();
            Finish(std::move(report));
            return false;
        }
        mReportFile = reportFile;
        mProcess = mFactory ? mFactory() : nullptr;
        if (!mProcess || !mProcess->Start(mRequest.mTool, TerrainBakeArguments(mRequest, mReportFile)))
        {
            mProcess.reset();
            TerrainBakeReport report;
            report.mOutcome = TerrainBakeOutcome::Failed;
            report.mMessage = "could not start TerrainTool at " + PathText(mRequest.mTool);
            Finish(std::move(report));
            return false;
        }

        mRunning = true;
        return true;
    }

    void TerrainBakeJob::Update()
    {
        if (!mRunning || !mProcess)
            return;

        if (const std::optional<int> exitCode = mProcess->Poll())
            Finish(ReadTerrainBakeReport(mReportFile, *exitCode));
    }

    void TerrainBakeJob::Cancel()
    {
        if (!mRunning)
            return;

        if (mProcess)
            mProcess->Terminate();

        TerrainBakeReport report;
        report.mOutcome = TerrainBakeOutcome::Canceled;
        report.mMessage = "Bake canceled.";
        Finish(std::move(report));
    }

    double TerrainBakeJob::Seconds() const
    {
        if (!mRunning)
            return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStarted).count();
    }

    const std::string &TerrainBakeJob::Message() const
    {
        static const std::string kEmpty;
        return mResult ? mResult->mReport.mMessage : kEmpty;
    }

    void TerrainBakeJob::Finish(TerrainBakeReport report)
    {
        if (!mReportFile.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(mReportFile, ignored);
            std::filesystem::remove(mReportFile.parent_path(), ignored);
            mReportFile.clear();
        }
        TerrainBakeResult result;
        result.mReport = std::move(report);
        result.mAsset = mRequest.mAsset;
        result.mStartChange = mStartChange;

        mRunning = false;
        ++mCompleted;
        mResult = std::move(result);
    }
} // namespace mir
