/**
 * @file UITextures.tests.cpp
 * @author sumin.park
 * @brief Tests for gameui/UITextures without a graphics API: the pixels kept,
 *        and a backend installed and removed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIBackend.h>
#include <gameui/UITextures.h>

#include <assets/ModelAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace mir;

namespace
{
    int gReleased = 0;

    // counts what it makes; a texture is a number, released through a counter
    class CountingBackend final : public IUIBackend
    {
    public:
        UIBackendTexture CreateTexture(uint32_t, uint32_t, std::span<const uint8_t>) override
        {
            ++mCreated;
            return UIBackendTexture{ reinterpret_cast<void *>(static_cast<uintptr_t>(mCreated)),
                                     [](void *) { ++gReleased; } };
        }
        UIBackendFont MakeFont(std::span<const uint8_t>) override { return {}; }
        void Submit(const UIRect &, float, const UIDrawList &, const UITextures &, const UIFonts &) override {}
        UISize MeasureText(std::string_view, float, void *) const override { return {}; }

        int mCreated = 0;
    };

    TextureAsset Solid(uint32_t width, uint32_t height)
    {
        return TextureAsset{ width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, 255) };
    }
}

TEST_CASE("A texture keeps its pixels until a backend makes it", "[gameui][textures]")
{
    gReleased = 0;
    CountingBackend backend;
    UITextures textures;
    const UISprite sprite = textures.Add(Solid(2, 2));
    REQUIRE(sprite.IsValid());
    CHECK(textures.Find(sprite.mTexture) == nullptr);

    textures.SetBackend(&backend);
    CHECK(backend.mCreated == 1);
    CHECK(textures.Find(sprite.mTexture) != nullptr);

    textures.Add(Solid(1, 1));
    CHECK(backend.mCreated == 2);

    // the copies stay: they release themselves when the textures go
    textures.SetBackend(nullptr);
    CHECK(gReleased == 0);
    CHECK(textures.Find(sprite.mTexture) != nullptr);
    textures.Add(Solid(1, 1));
    CHECK(backend.mCreated == 2);
}

TEST_CASE("A texture with the wrong pixel count gives no sprite", "[gameui][textures]")
{
    UITextures textures;
    TextureAsset truncated = Solid(2, 2);
    truncated.rgba.pop_back();
    CHECK_FALSE(textures.Add(truncated).IsValid());
    CHECK(textures.Count() == 0);
}

TEST_CASE("Destroying the textures releases what the backend made, the backend gone or not", "[gameui][textures]")
{
    gReleased = 0;
    {
        UITextures textures;
        {
            CountingBackend backend;
            textures.SetBackend(&backend);
            textures.Add(Solid(3, 3));
            textures.SetBackend(nullptr);
        }
    }
    CHECK(gReleased == 1);
}
