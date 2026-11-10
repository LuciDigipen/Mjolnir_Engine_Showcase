/**
 * @file Process.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/platform/Process and ExecutablePath.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

TEST_CASE("ExecutablePath is a file inside ExecutableDir", "[process]")
{
    const std::filesystem::path &path = mir::ExecutablePath();
    REQUIRE(std::filesystem::is_regular_file(path));
    CHECK(path.parent_path() == mir::ExecutableDir());
}

TEST_CASE("QuoteCommandLineArgument leaves a plain argument alone", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument("--game") == "--game");
    CHECK(mir::QuoteCommandLineArgument("games/HelloWorld") == "games/HelloWorld");
    CHECK(mir::QuoteCommandLineArgument(R"(C:\no\spaces\)") == R"(C:\no\spaces\)");
}

TEST_CASE("QuoteCommandLineArgument quotes an empty argument and whitespace", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument("") == R"("")");
    CHECK(mir::QuoteCommandLineArgument("games/My Game") == R"("games/My Game")");
}

TEST_CASE("QuoteCommandLineArgument escapes quotes and the backslashes around them", "[process]")
{
    CHECK(mir::QuoteCommandLineArgument(R"(say "hi")") == R"("say \"hi\"")");
    CHECK(mir::QuoteCommandLineArgument(R"(a\"b)") == R"("a\\\"b")");
    CHECK(mir::QuoteCommandLineArgument(R"(C:\My Dir\)") == R"("C:\My Dir\\")");
}

TEST_CASE("LaunchProcess returns the exit code of a process it waits for", "[process]")
{
#if defined(_WIN32)
    char *comspec = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
    REQUIRE(comspec != nullptr);
    const std::filesystem::path shell = comspec;
    std::free(comspec);
    const std::vector<std::string> args{"/c", "exit 3"};
#else
    const std::filesystem::path shell = "/bin/sh";
    const std::vector<std::string> args{"-c", "exit 3"};
#endif

    const std::optional<int> exitCode = mir::LaunchProcess(shell, args, true);
    REQUIRE(exitCode.has_value());
    CHECK(*exitCode == 3);
}

TEST_CASE("LaunchProcess reports a program that does not exist", "[process]")
{
    const std::vector<std::string> args;
    CHECK_FALSE(mir::LaunchProcess(mir::ExecutableDir() / "no_such_program.exe", args, true).has_value());
    CHECK_FALSE(mir::LaunchProcess(mir::ExecutableDir() / "no_such_program.exe", args, false).has_value());
}

#if defined(_WIN32)
TEST_CASE("LaunchProcess starts the program in the working directory it is given", "[process]")
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("process-cwd-" + std::to_string(mir::CurrentProcessId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    char *comspec = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
    REQUIRE(comspec != nullptr);
    const std::filesystem::path shell = comspec;
    std::free(comspec);

    const std::vector<std::string> args{"/c", "cd > here.txt"};
    REQUIRE(mir::LaunchProcess(shell, args, true, dir) == 0);
    const std::optional<std::string> here = mir::ReadText(dir / "here.txt");
    REQUIRE(here);
    CHECK(std::filesystem::equivalent(std::filesystem::path(here->substr(0, here->find_last_not_of("\r\n") + 1)), dir));
    std::filesystem::remove_all(dir);
}

TEST_CASE("ProcessesRunningFrom finds a process by the folder its executable is in", "[process]")
{
    const std::filesystem::path dir = mir::ExecutableDir();
    const auto hasThis = [](const std::vector<mir::RunningProcess> &processes) {
        for (const mir::RunningProcess &process : processes)
        {
            if (process.mId == mir::CurrentProcessId())
                return std::filesystem::equivalent(process.mExecutable, mir::ExecutablePath());
        }
        return false;
    };

    CHECK(hasThis(mir::ProcessesRunningFrom(dir)));
    CHECK(hasThis(mir::ProcessesRunningFrom(dir.parent_path())));

    // case does not matter, and a folder whose name only starts the same does not hold it
    std::wstring upper = dir.native();
    CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
    CHECK(hasThis(mir::ProcessesRunningFrom(upper)));
    const std::wstring name = dir.filename().native();
    CHECK_FALSE(hasThis(mir::ProcessesRunningFrom(dir.parent_path() / name.substr(0, name.size() - 1))));
    CHECK(mir::ProcessesRunningFrom(dir / "no-such-folder").empty());
}
#endif

TEST_CASE("ChildProcess starts on demand and exposes its eventual exit code", "[process]")
{
#if defined(_WIN32)
    char *comspec = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
    REQUIRE(comspec != nullptr);
    const std::filesystem::path shell = comspec;
    std::free(comspec);
    const std::vector<std::string> args{"/c", "exit 7"};
#else
    const std::filesystem::path shell = "/bin/sh";
    const std::vector<std::string> args{"-c", "exit 7"};
#endif
    mir::ChildProcess child;
    REQUIRE(child.Start(shell, args));
    const auto exitCode = child.Wait();
    REQUIRE(exitCode);
    CHECK(*exitCode == 7);
    CHECK_FALSE(child.Running());
}

#if defined(_WIN32)
namespace
{
    std::filesystem::path CommandShell()
    {
        char *comspec = nullptr;
        std::size_t length = 0;
        REQUIRE(_dupenv_s(&comspec, &length, "ComSpec") == 0);
        REQUIRE(comspec != nullptr);
        const std::filesystem::path shell = comspec;
        std::free(comspec);
        return shell;
    }

    std::filesystem::path TempFile(const std::string &tag)
    {
        return std::filesystem::temp_directory_path() /
               ("process-" + tag + "-" + std::to_string(mir::CurrentProcessId()) + ".txt");
    }
} // namespace

TEST_CASE("ChildProcess can send the child's stdout and stderr to a file", "[process]")
{
    const std::filesystem::path file = TempFile("output");
    REQUIRE(mir::WriteText(file, "from before"));

    mir::ChildProcess child;
    const std::vector<std::string> args{"/c", "echo to-out& echo to-err 1>&2"};
    REQUIRE(child.Start(CommandShell(), args, {}, file));
    CHECK(child.Wait() == 0);

    const std::optional<std::string> text = mir::ReadText(file);
    std::filesystem::remove(file);
    REQUIRE(text);
    CHECK(text->find("to-out") != std::string::npos);
    CHECK(text->find("to-err") != std::string::npos);
    CHECK(text->find("from before") == std::string::npos);
}

TEST_CASE("ChildProcess with an output file passes on no other handle", "[process]")
{
    // without FILE_SHARE_DELETE, a copy of this handle in the child would block the delete below
    const std::filesystem::path held = TempFile("held");
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE handle = CreateFileW(held.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);

    const std::filesystem::path output = TempFile("quiet");
    mir::ChildProcess child;
    const std::vector<std::string> args{"/c", "ping -n 3 127.0.0.1"};
    const bool started = child.Start(CommandShell(), args, {}, output);
    CloseHandle(handle);
    REQUIRE(started);

    std::error_code ec;
    std::filesystem::remove(held, ec);
    CHECK_FALSE(ec);
    // the delete happened while the child was still alive
    CHECK(child.Running());
    child.Wait();
    std::filesystem::remove(held, ec);
    std::filesystem::remove(output, ec);
}

TEST_CASE("ChildProcess does not start when its output file cannot be made", "[process]")
{
    mir::ChildProcess child;
    const std::vector<std::string> args{"/c", "exit 0"};
    CHECK_FALSE(child.Start(CommandShell(), args, {}, TempFile("dir") / "missing" / "output.txt"));
    CHECK_FALSE(child.Running());
}
#endif

TEST_CASE("CurrentProcessId names this process, the same each time", "[core][process]")
{
    CHECK(mir::CurrentProcessId() != 0);
    CHECK(mir::CurrentProcessId() == mir::CurrentProcessId());
}
