/**
 * @file AsyncAssetService.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/AsyncAssetService: requests, failures, cancellation and reload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AsyncAssetService.h>

#include <assets/AssetBlob.h>
#include <assets/AssetDecoder.h>
#include <assets/AssetManifest.h>
#include <assets/AssetsModule.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>
#include <jobs/JobExecutor.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mir;
using namespace std::chrono_literals;

namespace
{
    constexpr uint32_t kTextTag = 0x54455854u;
    constexpr uint32_t kPartTag = 0x54524150u;
    constexpr uint32_t kOtherTag = 0x4F544852u;

    struct Text
    {
        std::string value;
    };
    struct Unrelated
    {
    };

    std::atomic<int> gDecodes{ 0 };
    std::atomic<bool> gStarted{ false };
    std::atomic<bool> gRelease{ false };

    bool DecodeText(const AssetBlobView &root, const AssetDependencies &, Text &out, std::string &)
    {
        ++gDecodes;
        out.value = std::string(AsStringView(root));
        return true;
    }

    bool DecodeUpper(const AssetBlobView &root, const AssetDependencies &, Text &out, std::string &)
    {
        ++gDecodes;
        out.value = std::string(AsStringView(root));
        for (char &c : out.value)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return true;
    }

    bool DecodeFails(const AssetBlobView &, const AssetDependencies &, Text &, std::string &error)
    {
        ++gDecodes;
        error = "the payload is wrong";
        return false;
    }

    bool DecodeThrows(const AssetBlobView &, const AssetDependencies &, Text &, std::string &)
    {
        ++gDecodes;
        throw std::runtime_error("boom");
    }

    bool DecodeGated(const AssetBlobView &root, const AssetDependencies &, Text &out, std::string &)
    {
        gStarted = true;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!gRelease && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        out.value = std::string(AsStringView(root));
        return true;
    }

    // the root text, then the text of each part it can reach
    bool DecodeBundle(const AssetBlobView &root, const AssetDependencies &dependencies, Text &out, std::string &)
    {
        ++gDecodes;
        out.value = std::string(AsStringView(root));
        for (const char *name : { "part-a", "part-b" })
            if (const AssetBlobView *part = dependencies.Find(MakeAssetId(name)))
                out.value += "+" + std::string(AsStringView(*part));
        return true;
    }

    constexpr uint32_t kPartTags[]{ kPartTag };

    constexpr AssetDecoder<Text> kTextDecoder{ DecodeText, {} };
    constexpr AssetDecoder<Text> kUpperDecoder{ DecodeUpper, {} };
    constexpr AssetDecoder<Text> kFailsDecoder{ DecodeFails, {} };
    constexpr AssetDecoder<Text> kThrowsDecoder{ DecodeThrows, {} };
    constexpr AssetDecoder<Text> kGatedDecoder{ DecodeGated, {} };
    constexpr AssetDecoder<Text> kBundleDecoder{ DecodeBundle, kPartTags };

    std::vector<std::byte> MakeBlob(uint32_t tag, uint32_t version, std::string_view text)
    {
        return BuildAssetBlob(tag, version, std::as_bytes(std::span(text)));
    }

    // A cooked directory with a manifest, written from the items added so far
    struct Cooked
    {
        struct Item
        {
            std::string name;
            uint32_t entryTag = kTextTag;
            uint32_t entryVersion = 1;
            std::vector<std::byte> bytes;
            bool write = true;
            std::vector<AssetId> dependencies;
        };

        std::filesystem::path dir;
        std::vector<Item> items;

        Cooked()
        {
            static std::atomic<int> counter{ 0 };
            dir = std::filesystem::temp_directory_path() /
                  ("mir_async_assets_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
        }

        ~Cooked()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Add(std::string name, std::string_view text, std::vector<AssetId> dependencies = {},
            uint32_t tag = kTextTag)
        {
            items.push_back({ std::move(name), tag, 1, MakeBlob(tag, 1, text), true, std::move(dependencies) });
        }

        void AddRaw(Item item) { items.push_back(std::move(item)); }

        Item &Find(const std::string &name)
        {
            for (Item &item : items)
                if (item.name == name)
                    return item;
            throw std::runtime_error("no item " + name);
        }

        void Write() const
        {
            std::vector<std::string> files;
            files.reserve(items.size());
            std::vector<AssetManifestSourceEntry> entries;
            for (const Item &item : items)
            {
                files.push_back(item.name + ".blob");
                if (item.write)
                    REQUIRE(WriteBytes(dir / files.back(), item.bytes));
                else
                {
                    std::error_code ec;
                    std::filesystem::remove(dir / files.back(), ec);
                }
                entries.push_back({ { MakeAssetId(item.name), item.entryTag, item.entryVersion }, files.back(),
                    item.name, item.dependencies });
            }
            REQUIRE(WriteBytes(dir / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    AssetId Id(const char *name) { return MakeAssetId(name); }

    // Drains until nothing is waiting, within a deadline so a broken run fails instead of hanging
    bool DrainUntilIdle(AsyncAssetService &service)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            service.DrainCompletions();
            if (service.Idle())
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void ResetCounters()
    {
        gDecodes = 0;
        gStarted = false;
        gRelease = false;
    }
}

TEST_CASE("Two requests for one asset and decoder share one load", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(2);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest first = service.Request(Id("greeting"), kTextDecoder);
    const AssetRequest second = service.Request(Id("greeting"), kTextDecoder);
    CHECK(first.index == second.index);

    REQUIRE(DrainUntilIdle(service));
    CHECK(gDecodes == 1);
    const auto a = service.Acquire<Text>(first);
    const auto b = service.Acquire<Text>(second);
    REQUIRE(a != nullptr);
    CHECK(a == b);
    CHECK(a->value == "hello");
}

TEST_CASE("The same asset under two decoders is two loads", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(2);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest plain = service.Request(Id("greeting"), kTextDecoder);
    const AssetRequest upper = service.Request(Id("greeting"), kUpperDecoder);
    CHECK(plain.index != upper.index);

    REQUIRE(DrainUntilIdle(service));
    CHECK(gDecodes == 2);
    CHECK(service.Acquire<Text>(plain)->value == "hello");
    CHECK(service.Acquire<Text>(upper)->value == "HELLO");
}

TEST_CASE("A finished load is not visible before the drain", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(2);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest request = service.Request(Id("greeting"), kTextDecoder);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (gDecodes == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(gDecodes == 1);

    // the decode is done, and the state has not moved
    CHECK(service.State(request) == AssetLoadState::Queued);
    CHECK(service.Acquire<Text>(request) == nullptr);
    CHECK_FALSE(service.Idle());

    service.DrainCompletions();
    CHECK(service.State(request) == AssetLoadState::Ready);
    CHECK(service.Idle());
}

TEST_CASE("With no workers a load runs inline and completes at the next drain", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest request = service.Request(Id("greeting"), kTextDecoder);
    CHECK(gDecodes == 1);
    CHECK(service.State(request) == AssetLoadState::Queued);
    CHECK_FALSE(service.Idle());

    service.DrainCompletions();
    CHECK(service.State(request) == AssetLoadState::Ready);
    CHECK(service.Acquire<Text>(request)->value == "hello");
    CHECK(service.Idle());
}

TEST_CASE("Acquire is null unless the request is ready and was made for that type", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    CHECK(service.State(AssetRequest{}) == AssetLoadState::Stale);
    CHECK(service.Acquire<Text>(AssetRequest{}) == nullptr);

    const AssetRequest request = service.Request(Id("greeting"), kTextDecoder);
    CHECK(service.Acquire<Text>(request) == nullptr);

    service.DrainCompletions();
    CHECK(service.Acquire<Text>(request) != nullptr);
    CHECK(service.Acquire<Unrelated>(request) == nullptr);
}

TEST_CASE("A failed load is reported, remembered and not decoded again", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("other", "unrelated");
    const AssetDecoder<Text> *decoder = &kTextDecoder;
    const std::vector<std::byte> good = MakeBlob(kTextTag, 1, "x");

    SECTION("no manifest entry")
    {
    }
    SECTION("unreadable file")
    {
        cooked.AddRaw({ "subject", kTextTag, 1, good, false, {} });
    }
    SECTION("bad header")
    {
        cooked.AddRaw({ "subject", kTextTag, 1, std::vector<std::byte>(64, std::byte{ 0x7F }), true, {} });
    }
    SECTION("blob type differs from its entry")
    {
        cooked.AddRaw({ "subject", kPartTag, 1, good, true, {} });
    }
    SECTION("blob version differs from its entry")
    {
        cooked.AddRaw({ "subject", kTextTag, 2, good, true, {} });
    }
    SECTION("decoder rejects the payload")
    {
        cooked.Add("subject", "x");
        decoder = &kFailsDecoder;
    }
    SECTION("decoder throws")
    {
        cooked.Add("subject", "x");
        decoder = &kThrowsDecoder;
    }
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest request = service.Request(Id("subject"), *decoder);
    service.DrainCompletions();
    CHECK(service.State(request) == AssetLoadState::Failed);
    CHECK(service.Acquire<Text>(request) == nullptr);
    CHECK(service.Idle());
    const int decodes = gDecodes;

    // asking again returns the same failed handle and does no work
    const AssetRequest again = service.Request(Id("subject"), *decoder);
    CHECK(again.index == request.index);
    service.DrainCompletions();
    CHECK(service.State(again) == AssetLoadState::Failed);
    CHECK(gDecodes == decodes);
}

TEST_CASE("CancelAll lets a failed load be tried again", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.AddRaw({ "subject", kTextTag, 1, MakeBlob(kTextTag, 1, "late"), false, {} });
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest request = service.Request(Id("subject"), kTextDecoder);
    service.DrainCompletions();
    REQUIRE(service.State(request) == AssetLoadState::Failed);

    // the file appears, the failure stands until the service is cleared
    REQUIRE(WriteBytes(cooked.dir / "subject.blob", MakeBlob(kTextTag, 1, "late")));
    const AssetRequest remembered = service.Request(Id("subject"), kTextDecoder);
    service.DrainCompletions();
    CHECK(service.State(remembered) == AssetLoadState::Failed);

    service.CancelAll();
    const AssetRequest retried = service.Request(Id("subject"), kTextDecoder);
    service.DrainCompletions();
    CHECK(service.State(retried) == AssetLoadState::Ready);
    CHECK(service.Acquire<Text>(retried)->value == "late");
}

TEST_CASE("A missing dependency fails the load before the decoder runs", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.AddRaw({ "part-a", kPartTag, 1, MakeBlob(kPartTag, 1, "A"), false, {} });
    cooked.Add("root", "R", { Id("part-a") });
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest request = service.Request(Id("root"), kBundleDecoder);
    service.DrainCompletions();
    CHECK(service.State(request) == AssetLoadState::Failed);
    CHECK(gDecodes == 0);
}

TEST_CASE("A request reads only the dependency types its decoder lists", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("part-a", "A", {}, kPartTag);
    cooked.Add("part-b", "B", {}, kPartTag);
    // a part of a type nobody asked for leads to one that is wanted, and neither file exists,
    // so reading either would fail the load
    cooked.AddRaw({ "other", kOtherTag, 1, MakeBlob(kOtherTag, 1, "o"), false, { Id("hidden") } });
    cooked.AddRaw({ "hidden", kPartTag, 1, MakeBlob(kPartTag, 1, "h"), false, {} });
    cooked.Add("root", "R", { Id("part-a"), Id("other"), Id("part-b") });
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(2);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest request = service.Request(Id("root"), kBundleDecoder);
    REQUIRE(DrainUntilIdle(service));
    REQUIRE(service.State(request) == AssetLoadState::Ready);
    CHECK(service.Acquire<Text>(request)->value == "R+A+B");
}

TEST_CASE("A decoder that lists no dependency types loads the root alone", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.AddRaw({ "part-a", kPartTag, 1, MakeBlob(kPartTag, 1, "A"), false, {} });
    cooked.Add("root", "R", { Id("part-a") });
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest request = service.Request(Id("root"), kTextDecoder);
    service.DrainCompletions();
    CHECK(service.State(request) == AssetLoadState::Ready);
    CHECK(service.Acquire<Text>(request)->value == "R");
}

TEST_CASE("CancelAll makes handles stale, keeps held results alive and reopens the lane", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(2);
    AsyncAssetService service(assets, jobs, 2);

    const AssetRequest before = service.Request(Id("greeting"), kTextDecoder);
    REQUIRE(DrainUntilIdle(service));
    const std::shared_ptr<const Text> held = service.Acquire<Text>(before);
    REQUIRE(held != nullptr);

    service.CancelAll();
    CHECK(service.State(before) == AssetLoadState::Stale);
    CHECK(service.Acquire<Text>(before) == nullptr);
    CHECK(service.Idle());
    CHECK(held->value == "hello");

    const AssetRequest after = service.Request(Id("greeting"), kTextDecoder);
    REQUIRE(DrainUntilIdle(service));
    CHECK(service.State(after) == AssetLoadState::Ready);
    CHECK(service.State(before) == AssetLoadState::Stale);
}

TEST_CASE("CancelAll cancels loads that have not started and drops the ones that finish", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("first", "1");
    cooked.Add("second", "2");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(1);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest first = service.Request(Id("first"), kGatedDecoder);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!gStarted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(gStarted);

    // the one lane is busy with the first load, so the second waits behind it
    const AssetRequest second = service.Request(Id("second"), kTextDecoder);
    auto cancel = std::async(std::launch::async, [&service] { service.CancelAll(); });

    // CancelAll is stuck waiting for the running load, which means it already closed the lane
    REQUIRE(cancel.wait_for(100ms) == std::future_status::timeout);
    gRelease = true;
    cancel.get();

    CHECK(gDecodes == 0);
    CHECK(service.State(first) == AssetLoadState::Stale);
    CHECK(service.State(second) == AssetLoadState::Stale);
    CHECK(service.Idle());
}

TEST_CASE("A committed reload drops results made before it", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    cooked.Add("greeting", "hello");
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    AsyncAssetService service(assets, jobs, 1);

    const AssetRequest before = service.Request(Id("greeting"), kTextDecoder);
    service.DrainCompletions();
    REQUIRE(service.State(before) == AssetLoadState::Ready);
    const std::shared_ptr<const Text> held = service.Acquire<Text>(before);

    cooked.items.clear();
    cooked.Add("greeting", "changed");
    cooked.Write();
    assets.BeginReload();
    assets.CommitReload();

    SECTION("the next drain notices")
    {
        service.DrainCompletions();
        CHECK(service.State(before) == AssetLoadState::Stale);
        CHECK(service.Acquire<Text>(before) == nullptr);

        const AssetRequest after = service.Request(Id("greeting"), kTextDecoder);
        service.DrainCompletions();
        CHECK(service.Acquire<Text>(after)->value == "changed");
    }
    SECTION("the next request notices")
    {
        const AssetRequest after = service.Request(Id("greeting"), kTextDecoder);
        CHECK(service.State(before) == AssetLoadState::Stale);
        service.DrainCompletions();
        CHECK(service.Acquire<Text>(after)->value == "changed");
    }
    CHECK(held->value == "hello");
}

TEST_CASE("Many requests all complete on a pool of workers", "[assets][async]")
{
    ResetCounters();
    Cooked cooked;
    constexpr int kCount = 24;
    for (int i = 0; i < kCount; ++i)
        cooked.Add("asset-" + std::to_string(i), "value-" + std::to_string(i));
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(3);
    AsyncAssetService service(assets, jobs, 3);

    std::vector<AssetRequest> requests;
    for (int i = 0; i < kCount; ++i)
        requests.push_back(service.Request(MakeAssetId("asset-" + std::to_string(i)), kTextDecoder));
    REQUIRE(DrainUntilIdle(service));

    CHECK(gDecodes == kCount);
    for (int i = 0; i < kCount; ++i)
    {
        REQUIRE(service.State(requests[i]) == AssetLoadState::Ready);
        CHECK(service.Acquire<Text>(requests[i])->value == "value-" + std::to_string(i));
    }
}

TEST_CASE("A lane of no loads is refused", "[assets][async]")
{
    Cooked cooked;
    cooked.Write();
    AssetsModule assets(cooked.dir);
    JobExecutor jobs(0);
    CHECK_THROWS_AS(AsyncAssetService(assets, jobs, 0), std::invalid_argument);
}
