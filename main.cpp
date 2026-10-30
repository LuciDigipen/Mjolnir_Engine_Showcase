/**
 * @file main.cpp
 * @author Sumin Park
 * @brief Process entry point - parses launch args, resolves the game
 *        project, and drives App through one run (including a
 *        project-switch relaunch).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>
#include <app/ErrorReport.h>
#include <assets/AssetIdRoot.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <core/log/SetupError.h>
#include <core/platform/CrashHandler.h>
#include <core/platform/Process.h>
#include <project/GameProject.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::string_view kUsage = "[--game <dir>] [--frames <N>] [--capture <png>] [--no-editor] [--ui-demo] "
        "[--non-interactive] [--workers <0..32>] [--switch-to <dir>]...";

    struct LaunchOptions
    {
        // empty: use engine.json's defaultGame
        std::string mGameDir;

        // 0: run until the window closes
        uint64_t mMaxFrames = 0;
        std::string mCapturePath;
        uint32_t mWorkerThreads = static_cast<uint32_t>(mir::JobExecutor::DefaultWorkerCount());
        bool mNoEditor = false;
        bool mUIDemo = false;

        // for agents and CI: every prompt takes its safe answer instead of waiting for a click
        bool mNonInteractive = false;

        // test hook: games to restart into, in order
        std::vector<std::string> mProjectSwitches;
    };

    [[noreturn]] void ThrowUsage(std::string what)
    {
        throw mir::SetupError(mir::ErrorCategory::Usage, std::move(what), {}, {},
            "Fix the command line. " + std::string(kUsage));
    }

    // Throws a Usage SetupError on anything it does not recognize
    LaunchOptions ParseLaunchOptions(int argc, char **argv)
    {
        LaunchOptions options;

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--game" && i + 1 < argc)
            {
                options.mGameDir = argv[++i];
            }
            else if (arg == "--frames" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                const auto [parsedEnd, ec] = std::from_chars(value.data(), value.data() + value.size(), options.mMaxFrames);
                if (ec != std::errc{} || parsedEnd != value.data() + value.size() || options.mMaxFrames == 0)
                    ThrowUsage("--frames expects a positive integer, got '" + std::string(value) + "'");
            }
            else if (arg == "--capture" && i + 1 < argc)
            {
                options.mCapturePath = argv[++i];
            }
            else if (arg == "--workers" && i + 1 < argc)
            {
                const std::string_view value = argv[++i];
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.mWorkerThreads);
                if (error != std::errc{} || end != value.data() + value.size() || options.mWorkerThreads > 32)
                    ThrowUsage("--workers expects an integer from 0 to 32");
            }
            else if (arg == "--no-editor")
            {
                options.mNoEditor = true;
            }
            else if (arg == "--ui-demo")
            {
                options.mUIDemo = true;
            }
            else if (arg == "--non-interactive")
            {
                options.mNonInteractive = true;
            }
            else if (arg == "--switch-to" && i + 1 < argc)
            {
                options.mProjectSwitches.emplace_back(argv[++i]);
            }
            else
            {
                ThrowUsage("unrecognized argument '" + std::string(arg) + "'");
            }
        }

        // a switch happens halfway through --frames
        if (!options.mProjectSwitches.empty() && options.mMaxFrames == 0)
            ThrowUsage("--switch-to needs --frames");

        if (!options.mCapturePath.empty() && options.mMaxFrames == 0)
            ThrowUsage("--capture needs --frames");

        return options;
    }

    // Arguments for the next process; a bounded run passes on its remaining frames and switches
    std::vector<std::string> RelaunchArguments(const LaunchOptions &options, const mir::GameProject &next)
    {
        std::vector<std::string> args{ "--game", next.mDir };
        args.insert(args.end(), {"--workers", std::to_string(options.mWorkerThreads)});
        if (options.mNoEditor)
            args.emplace_back("--no-editor");
        if (options.mUIDemo)
            args.emplace_back("--ui-demo");
        if (options.mNonInteractive)
            args.emplace_back("--non-interactive");
        if (!options.mCapturePath.empty())
        {
            args.emplace_back("--capture");
            args.push_back(options.mCapturePath);
        }

        if (options.mMaxFrames != 0)
        {
            args.emplace_back("--frames");
            args.push_back(std::to_string(options.mMaxFrames - options.mMaxFrames / 2));

            for (std::size_t i = 1; i < options.mProjectSwitches.size(); ++i)
            {
                args.emplace_back("--switch-to");
                args.push_back(options.mProjectSwitches[i]);
            }
        }

        return args;
    }

    // Whether LogHistory holds an error
    bool AnyErrorLogged()
    {
        for (const mir::LogEntry &entry : mir::LogHistory::Snapshot())
        {
            if (entry.level == mir::LogLevel::Error || entry.level == mir::LogLevel::Critical)
                return true;
        }
        return false;
    }

    // Anything that is not a SetupError reaching main is an engine bug
    mir::SetupError InternalError(std::string_view what)
    {
        return mir::SetupError(mir::ErrorCategory::Internal, "Unexpected error: " + std::string(what), {},
            std::string(what), "This is an engine bug. Report it with the log attached: " +
            mir::LogFilePath().string());
    }

    int RunEngine(const LaunchOptions &options)
    {
        // a bad --game is the command line's fault, a bad defaultGame is the data's
        const bool fromCommandLine = !options.mGameDir.empty();
        mir::GameProject project = mir::ResolveGameProject(
            fromCommandLine ? options.mGameDir : mir::ReadDefaultGame(),
            fromCommandLine ? mir::ErrorCategory::Usage : mir::ErrorCategory::ProjectData);
        mir::ResolveAssetIdRoot(project);

        mir::AppDesc desc{};
        desc.mProject = project;
        desc.mMaxFrames = options.mMaxFrames;
        desc.mCapturePath = options.mCapturePath;
        desc.mWorkerThreads = options.mWorkerThreads;
        // a build without the editor is always the game; --no-editor is then a no-op
        desc.mEnableEditorLayout = mir::App::kHasEditor && !options.mNoEditor;
        desc.mUIDemo = options.mUIDemo;
        desc.mNonInteractive = options.mNonInteractive;
        if (!options.mProjectSwitches.empty())
            desc.mSwitchProjectTo = options.mProjectSwitches.front();

        // Outside the try, so a failure in Run reports with App alive. A failure while
        // building it reports after what was built has been torn down
        std::optional<mir::App> app;
        try
        {
            // also loads the game's scene
            app.emplace(desc);

            // Only now is startup actually done - close explicitly rather than
            // leaning on WS_EX_TOPMOST z-order to hide the swap.
            app->Splash().SetProgress("Ready", 1.0f);
            app->Splash().Close();

            app->Run();
        }
        catch (const mir::SetupError &error)
        {
            return mir::ReportFatal(error);
        }
        catch (const std::exception &error)
        {
            return mir::ReportFatal(InternalError(error.what()));
        }
        catch (...)
        {
            return mir::ReportFatal(InternalError("unknown exception"));
        }

        // a bounded (test) run fails if anything logged an error
        const bool bounded = options.mMaxFrames != 0;
        int exitCode = (bounded && AnyErrorLogged()) ? 1 : 0;
        const std::optional<mir::GameProject> next = app->ProjectToOpen();

        // module destructors log what fails and carry on, so the counter is how teardown reports
        const uint64_t errorsBeforeTeardown = mir::ErrorCount();
        app.reset();
        if (mir::ErrorCount() > errorsBeforeTeardown)
            exitCode = mir::kShutdownExitCode;

        // switching games restarts the process; a bounded run waits for the child
        if (next.has_value())
        {
            const std::vector<std::string> args = RelaunchArguments(options, *next);
            MIR_LOG_INFO("Restarting into '{}'", next->mDir);

            // the next process writes the same log file
            mir::ShutdownLog();

            const std::optional<int> childExit = mir::LaunchProcess(mir::ExecutablePath(), args, bounded);
            if (!childExit.has_value())
            {
                mir::InitLog();
                MIR_LOG_ERROR("could not restart into '{}'", next->mDir);
                mir::FlushLog();
                return 1;
            }

            return (bounded && exitCode == 0) ? *childExit : exitCode;
        }

        mir::FlushLog();
        return exitCode;
    }
}

int main(int argc, char **argv)
{
    // Logging lives outside App so early construction failures are still visible.
    mir::InitLog();
    MIR_LOG_INFO("Log file: {}", mir::LogFilePath().string());
    mir::ClearErrorReport();

    try
    {
        mir::InstallCrashHandler({ .mReportPath = mir::ErrorReportPath(),
                                   .mDumpPath = mir::CrashDumpPath(),
                                   .mReporterPath = mir::ReporterPath(),
                                   .mLogPath = mir::LogFilePath() });
    }
    catch (const mir::SetupError &error)
    {
        return mir::ReportFatal(error);
    }

    // bad arguments or game directory exit before the splash, with no App to tear down
    try
    {
        return RunEngine(ParseLaunchOptions(argc, argv));
    }
    catch (const mir::SetupError &error)
    {
        return mir::ReportFatal(error);
    }
    catch (const std::exception &error)
    {
        return mir::ReportFatal(InternalError(error.what()));
    }
    catch (...)
    {
        return mir::ReportFatal(InternalError("unknown exception"));
    }
}
