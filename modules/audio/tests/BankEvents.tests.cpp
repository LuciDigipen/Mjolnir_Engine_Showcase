/**
 * @file BankEvents.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for listing each SoundBank's events from the Wwise metadata beside it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/BankEvents.h>

#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    using mir::BankEvents;

    struct TempDir
    {
        std::filesystem::path path;

        explicit TempDir(std::string_view name)
            : path(std::filesystem::temp_directory_path() /
                   ("mir_bank_events_" + std::string(name) + "_" + std::to_string(mir::CurrentProcessId())))
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void WriteFile(const std::filesystem::path &path, std::string_view text)
    {
        std::ofstream(path, std::ios::binary) << text;
    }
}

TEST_CASE("each bank lists the events in the metadata beside it", "[audio]")
{
    const TempDir dir("listed");

    WriteFile(dir.path / "Init.bnk", "");
    WriteFile(dir.path / "Music.bnk", "");
    WriteFile(dir.path / "Music.json", R"({ "SoundBanksInfo": { "SoundBanks": [ { "ShortName": "Music",
        "Events": [ { "Name": "Play_B" }, { "Name": "Play_A" }, { "Name": "Play_B" } ] } ] } })");
    WriteFile(dir.path / "Sfx.bnk", "");
    WriteFile(dir.path / "Broken.bnk", "");
    WriteFile(dir.path / "Broken.json", "{ \"SoundBanksInfo\": ");
    WriteFile(dir.path / "Orphan.json", R"({ "SoundBanksInfo": { "SoundBanks": [] } })");

    const std::vector<BankEvents> banks = mir::ListBankEvents(dir.path);
    REQUIRE(banks.size() == 3);
    CHECK(banks[0] == BankEvents{"Broken", {}, false});
    CHECK(banks[1] == BankEvents{"Music", {"Play_A", "Play_B"}, true});
    CHECK(banks[2] == BankEvents{"Sfx", {}, false});
}

TEST_CASE("a missing bank folder lists no banks", "[audio]")
{
    const TempDir dir("missing");

    CHECK(mir::ListBankEvents(dir.path / "SoundBanks").empty());
}

TEST_CASE("SoundtrackCheck's banks list the events its scene and scripts post", "[audio]")
{
    const std::vector<BankEvents> banks =
        mir::ListBankEvents(std::filesystem::path(MIR_GAMES_DIR) / "SoundtrackCheck/assets/audio/SoundBanks");

    REQUIRE(banks.size() == 2);
    CHECK(banks[0] == BankEvents{"Music", {"Play_Soundtrack"}, true});
    CHECK(banks[1] == BankEvents{"Sfx", {"Play_Sfx_Loop"}, true});
}
