/**
 * @file BackgroundImage.tests.cpp
 * @author Sumin Park
 * @brief Tests for editor/BackgroundImage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BackgroundImage.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    void WriteOnePixelBmp(const std::filesystem::path &path)
    {
        std::array<std::uint8_t, 58> bytes{};
        bytes[0] = 'B';
        bytes[1] = 'M';
        const auto write = [&](std::size_t offset, const auto &value)
        {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        write(2, std::uint32_t{ 58 });
        write(10, std::uint32_t{ 54 });
        write(14, std::uint32_t{ 40 });
        write(18, std::int32_t{ 1 });
        write(22, std::int32_t{ 1 });
        write(26, std::uint16_t{ 1 });
        write(28, std::uint16_t{ 24 });
        write(34, std::uint32_t{ 4 });
        bytes[54] = 0;
        bytes[55] = 0;
        bytes[56] = 255;

        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    struct ShutdownProbe
    {
        bool mRan = false;
        int mUserTextures = -1;
    };

    void ProbeShutdown(ImGuiContext *ctx, ImGuiContextHook *hook)
    {
        auto *probe = static_cast<ShutdownProbe *>(hook->UserData);
        probe->mRan = true;
        probe->mUserTextures = ctx->UserTextures.Size;
    }
}

TEST_CASE("Background textures are released at context shutdown after the backend", "[editor][background]")
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "mir_background_image_tests";
    std::filesystem::create_directories(directory);
    const std::filesystem::path imagePath = directory / "pixel.bmp";
    WriteOnePixelBmp(imagePath);

    ImGui::CreateContext();
    {
        mir::BackgroundImage image;
        std::string error;
        REQUIRE(image.Load(imagePath, error));
        REQUIRE(image.HasImage());
        REQUIRE(image.Load(imagePath, error));
        REQUIRE(image.HasImage());
    }

    ImGuiContext *const ctx = ImGui::GetCurrentContext();
    REQUIRE(ctx != nullptr);
    CHECK(ctx->UserTextures.Size == 2);

    ShutdownProbe probe;
    ImGuiContextHook hook{};
    hook.Type = ImGuiContextHookType_Shutdown;
    hook.Callback = ProbeShutdown;
    hook.UserData = &probe;
    ImGui::AddContextHook(ctx, &hook);

    ImGui::DestroyContext();
    CHECK(probe.mRan);
    CHECK(probe.mUserTextures == 0);
}
