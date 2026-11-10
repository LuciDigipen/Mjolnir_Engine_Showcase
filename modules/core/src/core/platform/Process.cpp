/**
 * @file Process.cpp
 * @author Jackey Cen
 * @brief Starting another process, finding the ones running from a folder, and showing a file in the file browser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/platform/Process.h"

#include "core/log/Log.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <csignal>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <cstddef>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        template <typename CharT> std::basic_string<CharT> Quote(std::basic_string_view<CharT> arg)
        {
            constexpr CharT kSpecial[] = {CharT(' '), CharT('\t'), CharT('\n'), CharT('\v'), CharT('"'), CharT(0)};

            if (!arg.empty() && arg.find_first_of(kSpecial) == std::basic_string_view<CharT>::npos)
                return std::basic_string<CharT>(arg);

            std::basic_string<CharT> quoted(1, CharT('"'));
            std::size_t backslashes = 0;

            for (const CharT c : arg)
            {
                if (c == CharT('\\'))
                {
                    ++backslashes;
                    continue;
                }

                if (c == CharT('"'))
                {
                    // double the backslashes, then escape the quote
                    quoted.append(backslashes * 2 + 1, CharT('\\'));
                }
                else
                {
                    quoted.append(backslashes, CharT('\\'));
                }

                quoted.push_back(c);
                backslashes = 0;
            }

            // so trailing backslashes don't escape the closing quote
            quoted.append(backslashes * 2, CharT('\\'));
            quoted.push_back(CharT('"'));
            return quoted;
        }
    } // namespace

    std::string QuoteCommandLineArgument(std::string_view arg) { return Quote(arg); }

    uint64_t CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<uint64_t>(GetCurrentProcessId());
#elif defined(__linux__)
        return static_cast<uint64_t>(getpid());
#else
        return 0;
#endif
    }

    uint64_t CurrentProcessMemoryUsageBytes()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            return 0;
        return static_cast<uint64_t>(counters.WorkingSetSize);
#elif defined(__linux__)
        // ru_maxrss is kilobytes on Linux
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0)
            return 0;
        return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#else
        return 0;
#endif
    }

    struct ChildProcess::Impl
    {
#if defined(_WIN32)
        HANDLE handle = nullptr;
        // closing it kills the child, so the child dies with this process however it ends
        HANDLE job = nullptr;
#elif defined(__linux__)
        pid_t pid = -1;
#endif
        std::optional<int> exitCode;
    };

    ChildProcess::ChildProcess() : mImpl(std::make_unique<Impl>()) {}

    ChildProcess::~ChildProcess() { Terminate(); }

    ChildProcess::ChildProcess(ChildProcess &&) noexcept = default;
    ChildProcess &ChildProcess::operator=(ChildProcess &&) noexcept = default;

#if defined(_WIN32)
    namespace
    {
        // stdin from NUL, stdout and stderr into one file, and no other handle inherited
        struct RedirectedOutput
        {
            HANDLE input = INVALID_HANDLE_VALUE;
            HANDLE output = INVALID_HANDLE_VALUE;
            HANDLE inherited[2]{};
            std::vector<std::byte> attributes;

            RedirectedOutput() = default;
            RedirectedOutput(const RedirectedOutput &) = delete;
            RedirectedOutput &operator=(const RedirectedOutput &) = delete;

            ~RedirectedOutput()
            {
                if (!attributes.empty())
                    DeleteProcThreadAttributeList(List());
                for (HANDLE handle : {input, output})
                {
                    if (handle != INVALID_HANDLE_VALUE)
                        CloseHandle(handle);
                }
            }

            LPPROC_THREAD_ATTRIBUTE_LIST List() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data()); }

            bool Open(const std::filesystem::path &file, STARTUPINFOEXW &startup)
            {
                SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
                output = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE)
                    return false;

                SIZE_T size = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                attributes.resize(size);
                if (!InitializeProcThreadAttributeList(List(), 1, 0, &size))
                {
                    attributes.clear();
                    return false;
                }
                inherited[0] = input;
                inherited[1] = output;
                if (!UpdateProcThreadAttribute(List(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                               nullptr, nullptr))
                    return false;

                startup.StartupInfo.cb = sizeof(startup);
                startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
                startup.StartupInfo.hStdInput = input;
                startup.StartupInfo.hStdOutput = output;
                startup.StartupInfo.hStdError = output;
                startup.lpAttributeList = List();
                return true;
            }
        };
    } // namespace

    bool ChildProcess::Start(const std::filesystem::path &program, std::span<const std::string> args,
                             const std::filesystem::path &workingDirectory, const std::filesystem::path &outputFile)
    {
        if (mImpl->handle)
            return false;

        std::wstring commandLine = Quote(std::wstring_view(program.native()));
        for (const std::string &arg : args)
        {
            commandLine.push_back(L' ');
            commandLine += std::filesystem::path(QuoteCommandLineArgument(arg)).wstring();
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup.StartupInfo);
        // suspended until it is in the job, so it cannot start a grandchild outside it
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
        RedirectedOutput redirect;
        if (!outputFile.empty())
        {
            if (!redirect.Open(outputFile, startup))
            {
                MIR_LOG_ERROR("ChildProcess: could not send the output of '{}' to '{}' (error {})", program.string(),
                              outputFile.string(), GetLastError());
                return false;
            }
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }

        PROCESS_INFORMATION process{};
        const wchar_t *workingDirectoryValue = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
        if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, outputFile.empty() ? FALSE : TRUE,
                            flags, nullptr, workingDirectoryValue, &startup.StartupInfo, &process))
        {
            MIR_LOG_ERROR("ChildProcess: could not start '{}' (error {})", program.string(), GetLastError());
            return false;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job == nullptr ||
            !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess))
        {
            MIR_LOG_WARN("ChildProcess: '{}' is not tied to this process (error {}); it outlives a crash",
                program.string(), GetLastError());
            if (job != nullptr)
                CloseHandle(job);
            job = nullptr;
        }

        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        mImpl->handle = process.hProcess;
        mImpl->job = job;
        mImpl->exitCode.reset();
        return true;
    }

    std::optional<int> ChildProcess::Poll()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (!mImpl->handle)
            return std::nullopt;
        if (WaitForSingleObject(mImpl->handle, 0) == WAIT_TIMEOUT)
            return std::nullopt;

        DWORD code = 0;
        if (!GetExitCodeProcess(mImpl->handle, &code))
            code = static_cast<DWORD>(-1);
        CloseHandle(mImpl->handle);
        mImpl->handle = nullptr;
        if (mImpl->job != nullptr)
            CloseHandle(mImpl->job);
        mImpl->job = nullptr;
        mImpl->exitCode = static_cast<int>(code);
        return mImpl->exitCode;
    }

    bool ChildProcess::Running()
    {
        if (!mImpl->handle)
            return false;
        return !Poll().has_value();
    }

    std::optional<int> ChildProcess::Wait()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (!mImpl->handle)
            return std::nullopt;
        WaitForSingleObject(mImpl->handle, INFINITE);
        return Poll();
    }

    bool ChildProcess::Terminate()
    {
        if (!mImpl || !mImpl->handle)
            return false;
        const bool stopped = TerminateProcess(mImpl->handle, 1) != FALSE;
        WaitForSingleObject(mImpl->handle, INFINITE);
        Poll();
        return stopped;
    }
#elif defined(__linux__)
    bool ChildProcess::Start(const std::filesystem::path &program, std::span<const std::string> args,
                             const std::filesystem::path &workingDirectory, const std::filesystem::path &outputFile)
    {
        if (mImpl->pid > 0)
            return false;
        // builds are Windows only
        if (!outputFile.empty())
        {
            MIR_LOG_ERROR("ChildProcess: sending the output of '{}' to a file is only supported on Windows",
                          program.string());
            return false;
        }

        const pid_t child = fork();
        if (child < 0)
        {
            MIR_LOG_ERROR("ChildProcess: could not fork for '{}'", program.string());
            return false;
        }
        if (child == 0)
        {
            if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0)
                _exit(127);
            std::vector<std::string> storage;
            storage.reserve(args.size() + 1);
            storage.push_back(program.string());
            storage.insert(storage.end(), args.begin(), args.end());
            std::vector<char *> argv;
            argv.reserve(storage.size() + 1);
            for (std::string &value : storage)
                argv.push_back(value.data());
            argv.push_back(nullptr);
            execv(program.c_str(), argv.data());
            _exit(127);
        }
        mImpl->pid = child;
        mImpl->exitCode.reset();
        return true;
    }

    std::optional<int> ChildProcess::Poll()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (mImpl->pid <= 0)
            return std::nullopt;
        int status = 0;
        const pid_t result = waitpid(mImpl->pid, &status, WNOHANG);
        if (result == 0)
            return std::nullopt;
        mImpl->pid = -1;
        mImpl->exitCode = result < 0 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return mImpl->exitCode;
    }

    bool ChildProcess::Running() { return mImpl->pid > 0 && !Poll().has_value(); }

    std::optional<int> ChildProcess::Wait()
    {
        if (mImpl->exitCode)
            return mImpl->exitCode;
        if (mImpl->pid <= 0)
            return std::nullopt;
        int status = 0;
        const pid_t result = waitpid(mImpl->pid, &status, 0);
        mImpl->pid = -1;
        mImpl->exitCode = result < 0 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return mImpl->exitCode;
    }

    bool ChildProcess::Terminate()
    {
        if (!mImpl || mImpl->pid <= 0)
            return false;
        const bool stopped = kill(mImpl->pid, SIGTERM) == 0;
        Wait();
        return stopped;
    }
#else
    bool ChildProcess::Start(const std::filesystem::path &, std::span<const std::string>, const std::filesystem::path &,
                             const std::filesystem::path &)
    {
        return false;
    }
    bool ChildProcess::Running() { return false; }
    std::optional<int> ChildProcess::Poll() { return std::nullopt; }
    std::optional<int> ChildProcess::Wait() { return std::nullopt; }
    bool ChildProcess::Terminate() { return false; }
#endif

#if defined(_WIN32)
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit, const std::filesystem::path &workingDirectory)
    {
        std::wstring commandLine = Quote(std::wstring_view(program.native()));
        for (const std::string &arg : args)
        {
            // active code page, the same one the child's argv decodes with
            commandLine.push_back(L' ');
            commandLine += std::filesystem::path(QuoteCommandLineArgument(arg)).wstring();
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const wchar_t *directory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();

        // Out of a job that would kill it with this process, such as one a debugger or launcher made, a job that
        // allows no breaking away refuses the flag, so it is started inside the job then
        const auto start = [&](DWORD flags) -> DWORD {
            // CreateProcessW needs a mutable command line
            std::wstring mutableLine = commandLine;
            if (CreateProcessW(program.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, flags, nullptr, directory,
                               &startup, &process))
                return ERROR_SUCCESS;
            return GetLastError();
        };
        DWORD error = start(waitForExit ? 0 : CREATE_BREAKAWAY_FROM_JOB);
        if (error == ERROR_ACCESS_DENIED && !waitForExit)
            error = start(0);
        if (error != ERROR_SUCCESS)
        {
            MIR_LOG_ERROR("LaunchProcess: could not start '{}' (error {})", program.string(), error);
            return std::nullopt;
        }

        CloseHandle(process.hThread);

        int exitCode = 0;
        if (waitForExit)
        {
            WaitForSingleObject(process.hProcess, INFINITE);

            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            exitCode = static_cast<int>(code);
        }

        CloseHandle(process.hProcess);
        return exitCode;
    }

    namespace
    {
        // Lower case with backslashes, for comparing Windows paths
        std::wstring FoldedPath(const std::filesystem::path &path)
        {
            std::wstring text = path.lexically_normal().make_preferred().native();
            if (!text.empty())
                CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
            return text;
        }
    }

    std::vector<RunningProcess> ProcessesRunningFrom(const std::filesystem::path &dir)
    {
        std::vector<RunningProcess> found;
        // the long names Windows reports an executable by, not a short C:\PROGRA~1 one
        std::error_code ec;
        std::filesystem::path base = std::filesystem::weakly_canonical(dir, ec);
        if (ec)
            base = std::filesystem::absolute(dir, ec);
        std::wstring prefix = FoldedPath(base);
        if (ec || prefix.empty())
            return found;
        if (prefix.back() != L'\\')
            prefix.push_back(L'\\');

        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            MIR_LOG_ERROR("ProcessesRunningFrom: could not list processes (error {})", GetLastError());
            return found;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::wstring image(32768, L'\0');
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        {
            // the system's own processes cannot be opened, and run from nowhere a build writes
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr)
                continue;
            DWORD size = static_cast<DWORD>(image.size());
            const bool named = QueryFullProcessImageNameW(process, 0, image.data(), &size) != FALSE;
            CloseHandle(process);

            const std::filesystem::path executable(std::wstring_view(image.data(), named ? size : 0));
            if (named && FoldedPath(executable).starts_with(prefix))
                found.push_back({ static_cast<uint32_t>(entry.th32ProcessID), executable });
        }
        CloseHandle(snapshot);
        return found;
    }

    bool RevealInFileBrowser(const std::filesystem::path &file)
    {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(file, ec).lexically_normal().make_preferred();

        // the shell needs COM on this thread; it may already be there
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        HRESULT shown = E_FAIL;
        if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(absolute.c_str()))
        {
            shown = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        }
        if (SUCCEEDED(com))
            CoUninitialize();

        if (FAILED(shown))
        {
            MIR_LOG_ERROR("RevealInFileBrowser: could not show '{}' (HRESULT {:#010x})", absolute.string(),
                          static_cast<unsigned long>(shown));
            return false;
        }
        return true;
    }
#elif defined(__linux__)
    // Untested: no preset builds Linux. A failed exec shows up as exit status 127.
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit, const std::filesystem::path &workingDirectory)
    {
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(program.string());
        storage.insert(storage.end(), args.begin(), args.end());

        std::vector<char *> argv;
        argv.reserve(storage.size() + 1);
        for (std::string &value : storage)
            argv.push_back(value.data());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (!workingDirectory.empty())
            posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str());
        pid_t pid = 0;
        const int spawned = posix_spawn(&pid, storage.front().c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0)
        {
            MIR_LOG_ERROR("LaunchProcess: could not start '{}'", program.string());
            return std::nullopt;
        }

        if (!waitForExit)
            return 0;

        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
        {
            MIR_LOG_ERROR("LaunchProcess: lost track of '{}'", program.string());
            return std::nullopt;
        }

        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#else
    std::optional<int> LaunchProcess(
        const std::filesystem::path &program, std::span<const std::string>, bool, const std::filesystem::path &)
    {
        MIR_LOG_ERROR("LaunchProcess: not supported on this platform, cannot start '{}'", program.string());
        return std::nullopt;
    }
#endif

#if !defined(_WIN32)
    std::vector<RunningProcess> ProcessesRunningFrom(const std::filesystem::path &) { return {}; }

    bool RevealInFileBrowser(const std::filesystem::path &file)
    {
        MIR_LOG_ERROR("RevealInFileBrowser: not supported on this platform, cannot show '{}'", file.string());
        return false;
    }
#endif
} // namespace mir
