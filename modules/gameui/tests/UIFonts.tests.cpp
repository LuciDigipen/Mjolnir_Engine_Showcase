/**
 * @file UIFonts.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIFonts: what counts as a font file, and a font made
 *        by the backend once there is one and released with the table.
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
            return UIBackendFont{ reinterpret_cast<void *>(static_cast<uintptr_t>(mMade)), [](void *) { ++gReleased; } };
        }
        void Submit(const UIRect &, float, const UIDrawList &, const UITextures &, const UIFonts &) override {}
        UISize MeasureText(std::string_view, float, void *) const override { return {}; }

        int mMade = 0;
        size_t mLastSize = 0;
    };

    // a TrueType header and nothing a font needs after it; the backend here never reads it
    std::vector<uint8_t> FontHeader(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth)
    {
        std::vector<uint8_t> bytes(200, 0);
        bytes[0] = first;
        bytes[1] = second;
        bytes[2] = third;
        bytes[3] = fourth;
        return bytes;
    }
}

TEST_CASE("Only TrueType, OpenType and collection files are fonts", "[gameui][fonts]")
{
    UIFonts fonts;
    CHECK(fonts.Add(FontHeader(0x00, 0x01, 0x00, 0x00)).IsValid());
    CHECK(fonts.Add(FontHeader('O', 'T', 'T', 'O')).IsValid());
    CHECK(fonts.Add(FontHeader('t', 'r', 'u', 'e')).IsValid());
    CHECK(fonts.Add(FontHeader('t', 't', 'c', 'f')).IsValid());
    CHECK(fonts.Count() == 4);

    // a PNG, too short to be a font, and nothing
    CHECK_FALSE(fonts.Add(FontHeader(0x89, 'P', 'N', 'G')).IsValid());
    const std::vector<uint8_t> shortFont = { 0x00, 0x01, 0x00, 0x00 };
    CHECK_FALSE(fonts.Add(shortFont).IsValid());
    CHECK_FALSE(fonts.Add({}).IsValid());
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
