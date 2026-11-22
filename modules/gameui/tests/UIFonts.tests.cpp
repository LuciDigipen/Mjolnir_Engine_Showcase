/**
 * @file UIFonts.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIFonts: what counts as a font file, a font made by
 *        the backend once there is one and released with the table, and a
 *        font the backend cannot make.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIBackend.h>
#include <gameui/UIFonts.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

using namespace mir;

namespace
{
    int gReleased = 0;

    class CountingBackend final : public IUIBackend
    {
    public:
        UIBackendTexture CreateTexture(uint32_t, uint32_t, std::span<const uint8_t>) override { return {}; }
        UIBackendFont MakeFont(std::span<const uint8_t> bytes) override
        {
            ++mMade;
            mLastSize = bytes.size();
            if (mFail)
                return {};
            return UIBackendFont{ reinterpret_cast<void *>(static_cast<uintptr_t>(mMade)), [](void *) { ++gReleased; } };
        }
        void Submit(const UIRect &, float, const UIDrawList &, const UITextures &, const UIFonts &) override {}
        UISize MeasureText(std::string_view, float, void *) const override { return {}; }

        int mMade = 0;
        size_t mLastSize = 0;
        bool mFail = false;
    };

    void Put(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
    {
        bytes[at] = static_cast<uint8_t>(value >> 24);
        bytes[at + 1] = static_cast<uint8_t>(value >> 16);
        bytes[at + 2] = static_cast<uint8_t>(value >> 8);
        bytes[at + 3] = static_cast<uint8_t>(value);
    }

    // a font's tag and a directory of one 16 byte table at 28, and nothing a font needs; the backend here
    // never reads it
    std::vector<uint8_t> FontHeader(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth)
    {
        std::vector<uint8_t> bytes(200, 0);
        bytes[0] = first;
        bytes[1] = second;
        bytes[2] = third;
        bytes[3] = fourth;
        bytes[5] = 1;
        Put(bytes, 12 + 8, 28);
        Put(bytes, 12 + 12, 16);
        return bytes;
    }

    // one font, its directory at 16
    std::vector<uint8_t> Collection()
    {
        std::vector<uint8_t> bytes(200, 0);
        Put(bytes, 0, 0x74746366u);
        Put(bytes, 4, 0x00010000u);
        Put(bytes, 8, 1);
        Put(bytes, 12, 16);
        Put(bytes, 16, 0x00010000u);
        bytes[16 + 5] = 1;
        Put(bytes, 28 + 8, 60);
        Put(bytes, 28 + 12, 16);
        return bytes;
    }
}

TEST_CASE("Only whole TrueType, OpenType and collection files are fonts", "[gameui][fonts]")
{
    UIFonts fonts;
    CHECK(fonts.Add(FontHeader(0x00, 0x01, 0x00, 0x00)).IsValid());
    CHECK(fonts.Add(FontHeader('O', 'T', 'T', 'O')).IsValid());
    CHECK(fonts.Add(FontHeader('t', 'r', 'u', 'e')).IsValid());
    CHECK(fonts.Add(Collection()).IsValid());
    CHECK(fonts.Count() == 4);

    // a PNG, too short to be a font, and nothing
    CHECK_FALSE(fonts.Add(FontHeader(0x89, 'P', 'N', 'G')).IsValid());
    const std::vector<uint8_t> shortFont = { 0x00, 0x01, 0x00, 0x00 };
    CHECK_FALSE(fonts.Add(shortFont).IsValid());
    CHECK_FALSE(fonts.Add({}).IsValid());

    // cut off: a table past the end, a directory past the end, no tables, and a collection's font past the end
    std::vector<uint8_t> tablePastEnd = FontHeader(0x00, 0x01, 0x00, 0x00);
    Put(tablePastEnd, 12 + 12, 500);
    CHECK_FALSE(fonts.Add(tablePastEnd).IsValid());
    std::vector<uint8_t> manyTables = FontHeader(0x00, 0x01, 0x00, 0x00);
    manyTables[5] = 50;
    CHECK_FALSE(fonts.Add(manyTables).IsValid());
    std::vector<uint8_t> noTables = FontHeader(0x00, 0x01, 0x00, 0x00);
    noTables[5] = 0;
    CHECK_FALSE(fonts.Add(noTables).IsValid());
    std::vector<uint8_t> fontPastEnd = Collection();
    Put(fontPastEnd, 12, 500);
    CHECK_FALSE(fonts.Add(fontPastEnd).IsValid());
    CHECK(fonts.Count() == 4);
}

TEST_CASE("A font waits for a backend to make it, and is released with the table", "[gameui][fonts]")
{
    gReleased = 0;
    CountingBackend backend;
    {
        UIFonts fonts;
        const UIFont font = fonts.Add(FontHeader(0x00, 0x01, 0x00, 0x00));
        REQUIRE(font == UIFont{ 1 });
        CHECK(fonts.Find(font) == nullptr);
        CHECK(fonts.Find(UIFont{}) == nullptr);
        CHECK(fonts.Find(UIFont{ 2 }) == nullptr);

        fonts.SetBackend(&backend);
        CHECK(backend.mMade == 1);
        CHECK(backend.mLastSize == 200);
        CHECK(fonts.Find(font) != nullptr);

        // made once, and a font added now is made at once
        fonts.SetBackend(&backend);
        CHECK(backend.mMade == 1);
        const UIFont second = fonts.Add(FontHeader('O', 'T', 'T', 'O'));
        CHECK(backend.mMade == 2);
        CHECK(fonts.Find(second) != fonts.Find(font));
        CHECK(gReleased == 0);
    }
    CHECK(gReleased == 2);
}

TEST_CASE("A font the backend cannot make is not added, and draws in the default if made later", "[gameui][fonts]")
{
    CountingBackend backend;
    backend.mFail = true;
    UIFonts fonts;
    const UIFont before = fonts.Add(FontHeader(0x00, 0x01, 0x00, 0x00));
    REQUIRE(before.IsValid());

    fonts.SetBackend(&backend);
    CHECK(backend.mMade == 1);
    CHECK(fonts.Find(before) == nullptr);

    CHECK_FALSE(fonts.Add(FontHeader('O', 'T', 'T', 'O')).IsValid());
    CHECK(backend.mMade == 2);
    CHECK(fonts.Count() == 1);

    // the next font takes the id the refused one did not
    backend.mFail = false;
    CHECK(fonts.Add(FontHeader('t', 'r', 'u', 'e')) == UIFont{ 2 });
}
