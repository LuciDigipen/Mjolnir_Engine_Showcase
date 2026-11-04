/**
 * @file BuildProbe.cpp
 * @author Quy Dong Ta
 * @brief Child process for BuildJob.tests.cpp: swaps a build into --out as MjolnirBuild does, stops where the
 *        --game folder's name says, and waits to be canceled without writing a report.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

int main(int argc, char **argv)
{
    namespace fs = std::filesystem;

    fs::path game;
    fs::path out;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string_view flag = argv[i];
        if (flag == "--game")
            game = argv[i + 1];
        else if (flag == "--out")
            out = argv[i + 1];
    }
    if (game.empty() || out.empty())
        return 2;

    fs::path staging = out;
    staging += ".tmp";
    fs::path previous = out;
    previous += ".old";

    fs::create_directories(staging);
    std::ofstream(staging / "mjolnir_build.json") << "{ \"version\": 1 }\n";
    std::ofstream(staging / "Demo.exe") << "new";

    // CommitStaging's order: the previous build moves aside, then the new one moves in
    if (fs::exists(out))
        fs::rename(out, previous);
    if (game.filename() == "moved-in")
        fs::rename(staging, out);

    std::this_thread::sleep_for(std::chrono::minutes(5));
    return 1;
}
