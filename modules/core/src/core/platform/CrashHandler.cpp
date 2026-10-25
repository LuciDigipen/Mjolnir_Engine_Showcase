/**
 * @file CrashHandler.cpp
 * @author sumin.park
 * @brief Last-resort report for a crash or std::terminate on any thread
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/platform/CrashHandler.h"

#include "core/log/Log.h"
#include "core/log/SetupError.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#endif

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace mir
{
#if defined(_WIN32)
    namespace
    {
        using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
            PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

        // ours, in the customer range, so they never collide with a real NTSTATUS
        constexpr DWORD kTerminateCode = 0xE0000001;
        constexpr DWORD kPureCallCode = 0xE0000002;
        constexpr DWORD kInvalidParameterCode = 0xC0000417; // STATUS_INVALID_CRUNTIME_PARAMETER

        constexpr ULONG kStackGuarantee = 32 * 1024;

        // Text built on the crash thread, which must not allocate: the faulting thread
        // may hold the heap lock
        template <std::size_t N>
        struct FixedText
        {
            char data[N];
            std::size_t size = 0;

            void Append(std::string_view text)
            {
                for (const char c : text)
                {
                    if (size + 1 < N)
                        data[size++] = c;
                }
            }

            void AppendDecimal(uint32_t value)
            {
                char digits[10];
                std::size_t count = 0;
                do
                {
                    digits[count++] = static_cast<char>('0' + value % 10);
                    value /= 10;
                } while (value != 0);
                while (count > 0)
                    Append(std::string_view(&digits[--count], 1));
            }

            void AppendHex(uint32_t value)
            {
                Append("0x");
                for (int shift = 28; shift >= 0; shift -= 4)
                    Append(std::string_view(&"0123456789ABCDEF"[(value >> shift) & 0xF], 1));
            }

            // a JSON string body. Control and non-ASCII bytes become '?', since a
            // message from a throwing conversion is often not valid UTF-8
            void AppendEscaped(std::string_view text)
            {
                for (const char c : text)
                {
                    const auto byte = static_cast<unsigned char>(c);
                    if (c == '"' || c == '\\')
                    {
                        Append("\\");
                        Append(std::string_view(&c, 1));
                    }
                    else if (byte < 0x20 || byte >= 0x80)
                        Append("?");
                    else
                        Append(std::string_view(&c, 1));
                }
            }
        };

        struct CrashState
        {
            // set once at install, only read after
            std::wstring reportPath;
            // written first, then renamed over reportPath, so a reader sees a whole report or none
            std::wstring reportTempPath;
            std::wstring dumpPath;
            std::string reportHead;
            std::string reportTail;
            // CreateProcessW writes into it; empty for no box
            std::vector<wchar_t> reporterCommand;
            MiniDumpWriteDumpFn writeDump = nullptr;
            HANDLE wake = nullptr;

            // written by the faulting thread before it sets wake. Static, because a
            // stack array in the filter could fault again on an overflowed stack
            std::atomic<bool> claimed{ false };
            DWORD code = 0;
            DWORD threadId = 0;
            bool terminated = false;
            EXCEPTION_POINTERS *pointers = nullptr;
            EXCEPTION_RECORD record{};
            CONTEXT context{};
            EXCEPTION_POINTERS captured{};
            std::exception_ptr exception;
        };

        CrashState gCrash;

        std::string JsonEscape(std::string_view text)
        {
            std::string out;
            for (const char c : text)
            {
                if (c == '"' || c == '\\')
                    out += '\\';
                if (static_cast<unsigned char>(c) < 0x20)
                    out += '?';
                else
                    out += c;
            }
            return out;
        }

        // UTF-8 whatever the code page, so a non-ASCII path cannot throw here
        std::string Utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        std::string_view Describe(DWORD code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
                return "access violation";
            case EXCEPTION_STACK_OVERFLOW:
                return "stack overflow";
            case kPureCallCode:
                return "pure virtual call";
            case kInvalidParameterCode:
                return "invalid CRT parameter";
            default:
                return "exception";
            }
        }

        // Raw OS calls only; message may be null
        void WriteReport(const char *message)
        {
            FixedText<1024> cause;
            if (gCrash.terminated)
                cause.Append("std::terminate");
            else
            {
                cause.Append(Describe(gCrash.code));
                cause.Append(" ");
                cause.AppendHex(gCrash.code);
            }
            cause.Append(" on thread ");
            cause.AppendDecimal(gCrash.threadId);
            if (message != nullptr)
            {
                cause.Append(": ");
                cause.Append(message);
            }

            const HANDLE file =
                CreateFileW(gCrash.reportTempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;

            DWORD written = 0;
            WriteFile(file, gCrash.reportHead.data(), static_cast<DWORD>(gCrash.reportHead.size()), &written, nullptr);
            WriteFile(file, cause.data, static_cast<DWORD>(cause.size), &written, nullptr);
            WriteFile(file, gCrash.reportTail.data(), static_cast<DWORD>(gCrash.reportTail.size()), &written, nullptr);
            CloseHandle(file);
            MoveFileExW(gCrash.reportTempPath.c_str(), gCrash.reportPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        void WriteDump()
        {
            if (gCrash.writeDump == nullptr)
                return;

            const HANDLE file =
                CreateFileW(gCrash.dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;

            MINIDUMP_EXCEPTION_INFORMATION info{ gCrash.threadId, gCrash.pointers, FALSE };
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
            gCrash.writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
            CloseHandle(file);
        }

        // best effort, after the report and dump are on disk: rethrowing may allocate
        bool TakeMessage(FixedText<512> &out) noexcept
        {
            if (!gCrash.terminated)
                return false;

            // e.g. a joinable std::thread destroyed, or an explicit std::terminate()
            if (!gCrash.exception)
            {
                out.Append("no active exception");
                return true;
            }

            try
            {
                std::rethrow_exception(gCrash.exception);
            }
            catch (const std::exception &e)
            {
                out.AppendEscaped(e.what());
            }
            catch (...)
            {
                out.Append("unknown exception");
            }
            return true;
        }

        void StartReporter()
        {
            if (gCrash.reporterCommand.empty())
                return;

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, gCrash.reporterCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                    &startup, &process))
            {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }

        DWORD WINAPI CrashThreadMain(void *) noexcept
        {
            WaitForSingleObject(gCrash.wake, INFINITE);

            WriteReport(nullptr);
            WriteDump();

            FixedText<512> message;
            if (TakeMessage(message))
            {
                message.data[message.size] = '\0';
                WriteReport(message.data);
            }

            StartReporter();
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kCrashExitCode));
            return 0;
        }

        // Faulting thread: record, wake the crash thread, wait for it to end the process.
        // Kept tiny, since it may run on an overflowed stack
        [[noreturn]] void Crash(DWORD code, EXCEPTION_POINTERS *pointers, bool terminated) noexcept
        {
            // a second thread crashing meanwhile waits too; the first report wins
            if (gCrash.claimed.exchange(true))
            {
                for (;;)
                    Sleep(INFINITE);
            }

            gCrash.code = code;
            gCrash.threadId = GetCurrentThreadId();
            gCrash.terminated = terminated;
            if (pointers != nullptr)
                gCrash.pointers = pointers;
            else
            {
                RtlCaptureContext(&gCrash.context);
                gCrash.record.ExceptionCode = code;
            #if defined(_M_X64)
                gCrash.record.ExceptionAddress = reinterpret_cast<void *>(gCrash.context.Rip);
            #endif
                gCrash.captured = EXCEPTION_POINTERS{ &gCrash.record, &gCrash.context };
                gCrash.pointers = &gCrash.captured;
            }

            // per thread, so it is taken here and rethrown on the crash thread
            if (terminated)
                gCrash.exception = std::current_exception();

            SetEvent(gCrash.wake);
            for (;;)
                Sleep(INFINITE);
        }

        [[noreturn]] void OnTerminate() noexcept { Crash(kTerminateCode, nullptr, true); }

        // std::terminate on a thread that never armed ends in abort, which raises this
        // process-wide. The CRT resets it to the default before calling it, so re-arm first
        void OnAbort(int) noexcept
        {
            std::signal(SIGABRT, &OnAbort);
            Crash(kTerminateCode, nullptr, true);
        }

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS *pointers) noexcept
        {
            Crash(pointers->ExceptionRecord->ExceptionCode, pointers, false);
        }

        void OnInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t) noexcept
        {
            Crash(kInvalidParameterCode, nullptr, false);
        }

        void OnPureCall() noexcept { Crash(kPureCallCode, nullptr, false); }
    }

    void InstallCrashHandler(const CrashConfig &config)
    {
        if (gCrash.wake != nullptr)
            return;

        std::error_code ec;
        std::filesystem::create_directories(config.mReportPath.parent_path(), ec);
        std::filesystem::create_directories(config.mDumpPath.parent_path(), ec);

        gCrash.reportPath = config.mReportPath.wstring();
        gCrash.reportTempPath = gCrash.reportPath + L".tmp";
        gCrash.dumpPath = config.mDumpPath.wstring();

        const std::string dump = JsonEscape(Utf8(config.mDumpPath));
        const std::string log = JsonEscape(Utf8(config.mLogPath));
        gCrash.reportHead = "{\"kind\":\"crash\",\"category\":\"crash\",\"exitCode\":" + std::to_string(kCrashExitCode) +
            ",\"what\":\"The engine crashed\",\"path\":\"" + dump + "\",\"cause\":\"";
        gCrash.reportTail = "\",\"fix\":\"This is an engine bug. Report it with the log and the dump attached: " + log +
            ", " + dump + "\",\"log\":\"" + log + "\"}\n";

        if (!config.mReporterPath.empty())
        {
            const std::wstring command =
                L"\"" + config.mReporterPath.wstring() + L"\" \"" + config.mReportPath.wstring() + L"\"";
            gCrash.reporterCommand.assign(command.begin(), command.end());
            gCrash.reporterCommand.push_back(L'\0');
        }

        // loaded now: a crash may hold the loader lock
        if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
        {
            gCrash.writeDump =
                reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
        if (gCrash.writeDump == nullptr)
            MIR_LOG_WARN("InstallCrashHandler: dbghelp.dll has no MiniDumpWriteDump; a crash writes no dump");

        // started now: a crash cannot rely on creating a thread
        static_assert(std::is_nothrow_invocable_v<decltype(&CrashThreadMain), void *>,
            "C callbacks and thread entry points must be noexcept (conventions.md)");
        gCrash.wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HANDLE thread = gCrash.wake != nullptr ? CreateThread(nullptr, 0, &CrashThreadMain, nullptr, 0, nullptr) : nullptr;
        if (thread == nullptr)
        {
            throw SetupError(ErrorCategory::System, "The crash handler could not start", {},
                "Win32 error " + std::to_string(GetLastError()), "Check that the system is not out of memory or handles.");
        }
        CloseHandle(thread);

        // no Windows Error Reporting box: ours replaces it
        SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
        std::signal(SIGABRT, &OnAbort);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetUnhandledExceptionFilter(&OnUnhandledException);
        _set_invalid_parameter_handler(&OnInvalidParameter);
        _set_purecall_handler(&OnPureCall);

        ArmCrashHandler();
    }

    void ReclaimCrashHandler()
    {
        if (gCrash.wake == nullptr)
            return;

        if (SetUnhandledExceptionFilter(&OnUnhandledException) != &OnUnhandledException)
            MIR_LOG_WARN("ReclaimCrashHandler: a library had replaced the unhandled-exception filter; took it back");
        if (std::set_terminate(&OnTerminate) != &OnTerminate)
            MIR_LOG_WARN("ReclaimCrashHandler: a library had replaced the terminate handler; took it back");
    }

    void ArmCrashHandler()
    {
        if (gCrash.wake == nullptr)
            return;

        std::set_terminate(&OnTerminate);
        ULONG size = kStackGuarantee;
        SetThreadStackGuarantee(&size);
    }
#else
    // a signal-based handler goes here; until then a crash ends with the platform default
    void InstallCrashHandler(const CrashConfig &) {}
    void ReclaimCrashHandler() {}
    void ArmCrashHandler() {}
#endif
} // namespace mir
