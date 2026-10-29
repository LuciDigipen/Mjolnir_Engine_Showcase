/**
 * @file Log.cpp
 * @author Sumin Park
 * @brief Logging macros
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/log/Log.h"
#include "core/fs/Paths.h"
#include "core/log/Assert.h"
#include "core/log/LogHistory.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mir
{
    namespace
    {
        bool s_initialized = false;

        // a counter, not LogHistory, which is capped and drops old entries
        std::atomic<uint64_t> s_errorCount{ 0 };
        std::filesystem::path s_filePath;

        spdlog::level::level_enum ToSpd(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace:
                return spdlog::level::trace;
            case LogLevel::Debug:
                return spdlog::level::debug;
            case LogLevel::Info:
                return spdlog::level::info;
            case LogLevel::Warn:
                return spdlog::level::warn;
            case LogLevel::Error:
                return spdlog::level::err;
            case LogLevel::Critical:
                return spdlog::level::critical;
            case LogLevel::Off:
                return spdlog::level::off;
            }
            return spdlog::level::info;
        }

        LogLevel FromSpd(spdlog::level::level_enum level)
        {
            switch (level)
            {
            case spdlog::level::trace:
                return LogLevel::Trace;
            case spdlog::level::debug:
                return LogLevel::Debug;
            case spdlog::level::info:
                return LogLevel::Info;
            case spdlog::level::warn:
                return LogLevel::Warn;
            case spdlog::level::err:
                return LogLevel::Error;
            case spdlog::level::critical:
                return LogLevel::Critical;
            default:
                return LogLevel::Off;
            }
        }

        spdlog::source_loc ToSpdLoc(const std::source_location &loc)
        {
            return spdlog::source_loc{
                loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
        }

        // a repeat further apart than this starts over with a full line
        constexpr std::chrono::seconds kRepeatWindow{ 2 };
        constexpr std::chrono::seconds kRepeatReportInterval{ 10 };

        struct RepeatKey
        {
            spdlog::level::level_enum level;
            std::string message;
            std::string file;
            int line;

            bool operator==(const RepeatKey &) const = default;
        };

        struct RepeatKeyHash
        {
            std::size_t operator()(const RepeatKey &key) const
            {
                return std::hash<std::string>{}(key.message) ^
                    (static_cast<std::size_t>(key.line) * 31 + static_cast<std::size_t>(key.level));
            }
        };

        struct RepeatRun
        {
            uint64_t historyId = 0;
            uint64_t unreported = 0;
            spdlog::log_clock::time_point lastSeen;
            spdlog::log_clock::time_point lastReport;
        };


        class RepeatFilterSink final : public spdlog::sinks::dist_sink<std::mutex>
        {
        public:
            using dist_sink::dist_sink;

            void ReportRepeats()
            {
                std::lock_guard lock(mutex_);
                const auto now = spdlog::log_clock::now();
                for (auto &[key, run] : mRuns)
                    Report(key, run, now);
            }

        protected:
            void sink_it_(const spdlog::details::log_msg &msg) override
            {
                EndQuietRuns(msg.time);
                if (msg.level < spdlog::level::warn)
                {
                    Write(msg);
                    return;
                }

                RepeatKey key{ msg.level, std::string(msg.payload.begin(), msg.payload.end()),
                    msg.source.filename != nullptr ? msg.source.filename : "", msg.source.line };
                const auto it = mRuns.find(key);
                if (it == mRuns.end())
                {
                    const uint64_t historyId = Write(msg);
                    mRuns.emplace(std::move(key),
                        RepeatRun{ .historyId = historyId, .lastSeen = msg.time, .lastReport = msg.time });
                    return;
                }

                RepeatRun &run = it->second;
                run.lastSeen = msg.time;
                ++run.unreported;
                // cleared from the Log panel while still repeating, so it shows again
                if (!LogHistory::AddRepeat(run.historyId))
                    run.historyId = PushHistory(msg);
                if (msg.time - run.lastReport >= kRepeatReportInterval)
                    Report(it->first, run, msg.time);
            }

        private:
            static uint64_t PushHistory(const spdlog::details::log_msg &msg)
            {
                return LogHistory::Push(LogEntry{
                    .level = FromSpd(msg.level),
                    .message = std::string(msg.payload.begin(), msg.payload.end()),
                    .file = msg.source.filename != nullptr ? msg.source.filename : "",
                    .line = msg.source.line,
                    .time = msg.time,
                });
            }

            uint64_t Write(const spdlog::details::log_msg &msg)
            {
                dist_sink::sink_it_(msg);
                return PushHistory(msg);
            }

            void Report(const RepeatKey &key, RepeatRun &run, spdlog::log_clock::time_point now)
            {
                if (run.unreported == 0)
                    return;
                const std::string text = std::format("{} (repeated {} more {})", key.message, run.unreported,
                    run.unreported == 1 ? "time" : "times");
                dist_sink::sink_it_(spdlog::details::log_msg(
                    spdlog::source_loc{ key.file.c_str(), key.line, "" }, {}, key.level, text));
                run.unreported = 0;
                run.lastReport = now;
            }

            void EndQuietRuns(spdlog::log_clock::time_point now)
            {
                for (auto it = mRuns.begin(); it != mRuns.end();)
                {
                    if (now - it->second.lastSeen <= kRepeatWindow)
                    {
                        ++it;
                        continue;
                    }
                    Report(it->first, it->second, now);
                    it = mRuns.erase(it);
                }
            }

            std::unordered_map<RepeatKey, RepeatRun, RepeatKeyHash> mRuns;
        };

        std::shared_ptr<RepeatFilterSink> s_sink;
    }

    void InitLog(const LogConfig &config)
    {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_level(ToSpd(config.consoleLevel));

        s_filePath = std::filesystem::absolute(
            config.filePath.empty() ? LogDir() / "engine.log" : std::filesystem::path(config.filePath));
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            s_filePath.string(), config.maxFileBytes, config.maxFiles);
        file->set_level(ToSpd(config.fileLevel));

        s_sink = std::make_shared<RepeatFilterSink>(std::vector<spdlog::sink_ptr>{ console, file });
        s_sink->set_level(spdlog::level::trace);
        LogHistory::SetCapacity(config.uiHistoryCapacity);

        auto logger = std::make_shared<spdlog::logger>("engine", s_sink);

        logger->set_level(std::min(ToSpd(config.consoleLevel), ToSpd(config.fileLevel)));
        logger->set_pattern("%^[%T] [%l] %v%$ (%s:%#)");
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(std::move(logger));
        s_initialized = true;
    }

    uint64_t ErrorCount()
    {
        return s_errorCount;
    }

    const std::filesystem::path &LogFilePath()
    {
        return s_filePath;
    }

    void FlushLog()
    {
        if (s_initialized)
        {
            s_sink->ReportRepeats();
            spdlog::default_logger_raw()->flush();
        }
    }

    void ShutdownLog()
    {
        if (!s_initialized)
            return;

        s_initialized = false;
        s_sink->ReportRepeats();
        s_sink.reset();

        // flushes and releases the log file
        spdlog::shutdown();
    }

    namespace detail
    {
        bool ShouldLog(LogLevel level)
        {
            if (!s_initialized)
            {
                return false;
            }
            return spdlog::default_logger_raw()->should_log(ToSpd(level));
        }

        void Log(LogLevel level, std::string_view msg, std::source_location loc)
        {
            if (level >= LogLevel::Error && level != LogLevel::Off)
                ++s_errorCount;
            if (!s_initialized)
            {
                return;
            }
            spdlog::default_logger_raw()->log(
                ToSpdLoc(loc), ToSpd(level), "{}", msg);
        }

        void LogAssert(const char *expr, std::string_view msg, std::source_location loc)
        {
            ++s_errorCount;
            if (!s_initialized)
            {
                return;
            }

            auto *logger = spdlog::default_logger_raw();
            if (msg.empty())
            {
                logger->log(ToSpdLoc(loc), spdlog::level::critical,
                            "Assertion failed: ({})", expr);
            }
            else
            {
                logger->log(ToSpdLoc(loc), spdlog::level::critical,
                            "Assertion failed: ({}) {}", expr, msg);
            }
            logger->flush();
        }
    }
}
