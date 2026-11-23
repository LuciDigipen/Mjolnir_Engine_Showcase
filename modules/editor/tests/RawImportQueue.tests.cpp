/**
 * @file RawImportQueue.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/RawImportQueue.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/RawImportQueue.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace
{
    const std::filesystem::path kPath = "assets/aircraft/plane.aircraft.json";
    const std::filesystem::path kOther = "assets/scripts/pilot.lua";
}

TEST_CASE("A raw file moves from queued to cooking to applied", "[editor][import]")
{
    mir::RawImportQueue queue;
    CHECK_FALSE(queue.Status(kPath).has_value());

    queue.Queue(kPath);
    REQUIRE(queue.Status(kPath).has_value());
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Queued);

    queue.StartCooking(kPath);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Cooking);

    queue.Finish(kPath, false, true);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);
}

TEST_CASE("An unchanged cook waits for an apply the batch still owes", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.Queue(kOther);
    queue.StartCooking(kPath);
    queue.StartCooking(kOther);

    queue.Finish(kPath, true, true);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::WaitingForApply);
    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);

    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, true, false);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::WaitingForStop);
    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);
}

TEST_CASE("A file queued again while cooking stays queued", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Queue(kPath);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Queued);

    queue.Finish(kPath, false, true);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Queued);
}

TEST_CASE("A failed cook reports its reason and keeps it through an apply", "[editor][import]")
{
    mir::RawImportQueue queue;
    for (const char *reason : { "could not start AssetCooker", "AssetCooker failed", "unreadable result", "canceled" })
    {
        INFO(reason);
        queue.Queue(kPath);
        queue.StartCooking(kPath);
        queue.Fail(kPath, reason);
        REQUIRE(queue.Status(kPath).has_value());
        CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
        CHECK(queue.Status(kPath)->mReason == reason);
    }

    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "canceled");
}

TEST_CASE("A vetoed apply reports its reason", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, true, true);
    queue.MarkApplyFailed("a prefab vetoed the reload");
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "a prefab vetoed the reload");
}

TEST_CASE("A later apply also applies the files a veto left cooked", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.Queue(kOther);
    queue.StartCooking(kPath);
    queue.StartCooking(kOther);
    queue.Finish(kPath, true, true);
    queue.Finish(kOther, true, true);
    queue.MarkApplyFailed("a prefab vetoed the reload");

    queue.Queue(kOther);
    queue.StartCooking(kOther);
    queue.Finish(kOther, queue.NeedsApply(true, true), true);
    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);
    CHECK(queue.Status(kPath)->mReason.empty());
    CHECK(queue.Status(kOther)->mState == mir::RawImportState::Applied);
}

TEST_CASE("A vetoed file that failed again keeps its newer failure through a later apply", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, true, true);
    queue.MarkApplyFailed("a prefab vetoed the reload");

    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Fail(kPath, "AssetCooker failed");
    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "AssetCooker failed");
}

TEST_CASE("A file the cook left out is skipped, not applied", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Skip(kPath, "excluded from the asset cook");
    queue.Finish(kPath, true, true);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Skipped);
    CHECK(queue.Status(kPath)->mReason == "excluded from the asset cook");
    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Skipped);

    queue.Skip(kOther, "excluded from the asset cook");
    CHECK_FALSE(queue.Status(kOther).has_value());
}

TEST_CASE("A vetoed apply keeps the changed manifest unapplied until the next changed cook", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, true, true);
    queue.MarkApplyFailed("a prefab vetoed the reload");
    CHECK_FALSE(queue.NeedsApply(false, false));

    queue.Queue(kPath);
    queue.StartCooking(kPath);
    CHECK(queue.NeedsApply(false, true));
    queue.Finish(kPath, queue.NeedsApply(false, true), true);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::WaitingForApply);

    queue.MarkApplied();
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);
    CHECK_FALSE(queue.NeedsApply(false, true));
}

TEST_CASE("A second veto keeps the manifest unapplied for another changed cook", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, true, true);
    queue.MarkApplyFailed("first veto");

    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, queue.NeedsApply(false, true), true);
    queue.MarkApplyFailed("second veto");

    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "second veto");
    CHECK(queue.NeedsApply(false, true));
    CHECK_FALSE(queue.NeedsApply(false, false));
}

TEST_CASE("A failure after an applied cook is reported", "[editor][import]")
{
    mir::RawImportQueue queue;
    queue.Queue(kPath);
    queue.StartCooking(kPath);
    queue.Finish(kPath, false, true);
    REQUIRE(queue.Status(kPath).has_value());
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Applied);

    queue.Fail(kPath, "AssetCooker failed");
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "AssetCooker failed");

    queue.Queue(kPath);
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Queued);
    queue.StartCooking(kPath);
    queue.Fail(kPath, "canceled");
    CHECK(queue.Status(kPath)->mState == mir::RawImportState::Failed);
    CHECK(queue.Status(kPath)->mReason == "canceled");
}
