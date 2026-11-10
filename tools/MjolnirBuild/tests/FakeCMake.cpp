/**
 * @file FakeCMake.cpp
 * @author Quy Dong Ta
 * @brief A test-only cmake.exe for the game build step: configures, compiles and installs without a compiler,
 *        and appends each call to fake_cmake_calls.txt in the checkout. fake_cmake_fail.txt there names
 *        a phase to fail ("configure", "compile", "install") or "editor" to install an editor build, and
 *        fake_cmake_slow.txt makes the compile take a couple of seconds. With --sleep <seconds> it only waits, as a
 *        game left running in a build's folder.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::string Read(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    void Write(const fs::path &file, const std::string &text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

    // "true" or "false" for NAME:BOOL=ON in the cache
    std::optional<std::string> CacheFlag(const std::string &cache, const std::string &name)
    {
        const std::string key = name + ":BOOL=";
        const std::size_t at = cache.find(key);
        if (at == std::string::npos)
            return std::nullopt;
        return cache.compare(at + key.size(), 2, "ON") == 0 ? "true" : "false";
    }
}

int main(int argc, char **argv)
{
    // stands in for a game left running in a folder a build replaces
    if (argc == 3 && std::string_view(argv[1]) == "--sleep")
    {
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        return 0;
    }

    const std::vector<std::string> args(argv + 1, argv + argc);
    std::string call;
    for (const std::string &arg : args)
        call += (call.empty() ? "" : " ") + arg;
    std::ofstream("fake_cmake_calls.txt", std::ios::app) << call << "\n";
    std::printf("fake cmake %s\n", call.c_str());

    const std::string fail = Read("fake_cmake_fail.txt");
    const auto failing = [&](const char *phase) { return fail.find(phase) != std::string::npos; };

    if (args.size() >= 2 && args[0] == "--preset")
    {
        if (failing("configure"))
        {
            std::puts("CMake Error at CMakeLists.txt:1 (project):");
            std::puts("  No CMAKE_CXX_COMPILER could be found.");
            return 1;
        }
        // fake_cmake_binary_dir.txt stands in for a binaryDir other than builds/<preset>
        const std::string binaryDir = Read("fake_cmake_binary_dir.txt");
        const fs::path dir = binaryDir.empty() ? fs::path("builds") / args[1] : fs::path(binaryDir);

        // As CMake's cache does: an entry stays until -D sets it or -U removes it
        std::map<std::string, std::string> entries;
        std::istringstream previous(Read(dir / "CMakeCache.txt"));
        for (std::string line; std::getline(previous, line);)
        {
            if (const std::size_t at = line.find(":BOOL="); at != std::string::npos)
                entries[line.substr(0, at)] = line.substr(at + 6);
        }
        for (std::size_t i = 2; i < args.size(); ++i)
        {
            const std::size_t equals = args[i].find('=');
            if (args[i].starts_with("-D") && equals != std::string::npos)
                entries[args[i].substr(2, equals - 2)] = args[i].substr(equals + 1);
            else if (args[i].starts_with("-U"))
                entries.erase(args[i].substr(2));
        }
        std::string cache = "# fake\n";
        for (const auto &[name, value] : entries)
            cache += name + ":BOOL=" + value + "\n";
        Write(dir / "CMakeCache.txt", cache);
        Write(dir / "build.ninja", "# fake\n");
        return 0;
    }

    if (args.size() >= 3 && args[0] == "--build" && args[1] == "--preset")
    {
        // As Ninja prints to a file; fake_cmake_slow.txt spaces the lines out for the progress reader
        const bool slow = fs::exists("fake_cmake_slow.txt");
        for (int done = 1; done <= 3; ++done)
        {
            std::printf("[%d/3] Building CXX object engine/CMakeFiles/Engine.dir/Part%d.cpp.obj\n", done, done);
            std::fflush(stdout);
            if (slow)
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
        if (failing("compile"))
        {
            std::puts("FAILED: engine/CMakeFiles/Engine.dir/Engine.cpp.obj");
            std::puts("Engine.cpp(1): error C2065: 'oops': undeclared identifier");
            return 1;
        }
        return 0;
    }

    if (args.size() >= 4 && args[0] == "--install" && args[2] == "--prefix")
    {
        if (failing("install"))
            return 1;
        const fs::path prefix = args[3];
        const std::string cache = Read(fs::path(args[1]) / "CMakeCache.txt");
        std::string info = std::string("{ \"version\": 1, \"engineVersion\": \"0.1.0\", \"commit\": \"fake\", \"editor\": ") +
            (failing("editor") ? "true" : "false");
        for (const auto &[option, key] : { std::pair{ "MIR_LARGE_WORLD", "largeWorld" }, std::pair{ "MIR_WITH_AUDIO", "audio" },
                 std::pair{ "MIR_WITH_DLSS", "dlss" }, std::pair{ "MIR_WITH_RTXGI", "rtxgi" } })
        {
            if (const std::optional<std::string> flag = CacheFlag(cache, option))
                info += std::string(", \"") + key + "\": " + *flag;
        }
        Write(prefix / "MjolnirEngine.exe", "engine");
        Write(prefix / "MjolnirErrorReporter.exe", "reporter");
        Write(prefix / "game_build_info.json", info + " }\n");
        Write(prefix / "fonts" / "Inter.ttf", "font");
        return 0;
    }

    std::fprintf(stderr, "fake cmake: unexpected arguments: %s\n", call.c_str());
    return 2;
}
