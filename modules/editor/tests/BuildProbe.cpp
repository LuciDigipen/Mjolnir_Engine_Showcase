/**
 * @file BuildProbe.cpp
 * @author Quy Dong Ta
 * @brief Child process for BuildJob.tests.cpp: swaps a build into --out, or a template into --game-build, as
 *        MjolnirBuild does, stops where the --game folder's name says, writes --progress as a compile would, and
 *        waits to be canceled without writing a report. With no arguments it is a game that keeps running.
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

    // copied in as a template's exe, it is the game Build and Run starts with no arguments
    if (argc == 1)
    {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }

    fs::path game;
    fs::path gameBuild;
    fs::path out;
    fs::path progress;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string_view flag = argv[i];
        if (flag == "--game")
            game = argv[i + 1];
        else if (flag == "--game-build")
            gameBuild = argv[i + 1];
        else if (flag == "--out")
            out = argv[i + 1];
        else if (flag == "--progress")
            progress = argv[i + 1];
    }
    if (game.empty() || out.empty())
        return 2;

    if (!progress.empty())
    {
        fs::path temp = progress;
        temp += ".tmp";
        std::ofstream(temp) << R"({"version":1,"step":"game build","phase":"compile","done":2,"total":5})" << "\n";
        fs::rename(temp, progress);
    }

    // A compile's order: the previous template moves aside and is marked, then the new one moves in
    if (game.filename() == "template-mid-swap")
    {
        fs::path previousTemplate = gameBuild;
        previousTemplate += ".old";
        fs::rename(gameBuild, previousTemplate);
        std::ofstream(previousTemplate / "mjolnir_game_build_previous");
        std::this_thread::sleep_for(std::chrono::minutes(5));
        return 1;
    }

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
