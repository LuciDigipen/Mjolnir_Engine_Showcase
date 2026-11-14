/**
 * @file SettingsPanels.tests.cpp
 * @author sumin.park
 * @brief Verifies settings stay inactive until a successful Save.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/ProjectSettingsPanel.h>

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    class SettingsPanelFixture
    {
    public:
        SettingsPanelFixture()
        {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1000, 1000);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            mPath = std::filesystem::temp_directory_path() /
                ("mir_settings_panel_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".json");
            std::filesystem::remove(mPath);
            // Measure the collapsed rendering controls using the same public ImGui layout.
            Begin();
            ImGui::SeparatorText("Input");
            ImGui::TextWrapped("Actions and bindings live in their own panel.");
            ImGui::Button("Open Input Settings");
            ImGui::SeparatorText("Rendering");
            bool enabled = false;
            ImGui::Checkbox("Ray Tracing", &enabled);
            mToggle = Center();
            if (ImGui::TreeNode("Diffuse Global Illumination")) ImGui::TreePop();
            if (ImGui::TreeNode("Screen Space Global Illumination")) ImGui::TreePop();
            ImGui::Button("Save##Rendering");
            mSave = Center();
            End();
            Frame();
        }

        ~SettingsPanelFixture()
        {
            ImGui::DestroyContext();
            std::filesystem::remove(mPath);
        }

        void Frame()
        {
            Begin();
            mPanel.Draw(mSettings, mPath, mShowInput, nullptr, nullptr, true);
            End();
        }

        void Click(ImVec2 point)
        {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            Frame();
            io.AddMouseButtonEvent(0, true);
            Frame();
            io.AddMouseButtonEvent(0, false);
            Frame();
        }

        mir::ProjectSettingsPanel mPanel;
        mir::RenderSettings mSettings;
        std::filesystem::path mPath;
        ImVec2 mToggle;
        ImVec2 mSave;

    private:
        static ImVec2 Center()
        {
            const auto low = ImGui::GetItemRectMin();
            const auto high = ImGui::GetItemRectMax();
            return ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
        }

        static void Begin()
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(900, 900));
            ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings);
        }

        static void End()
        {
            ImGui::End();
            ImGui::Render();
        }

        bool mShowInput = false;
    };
}

TEST_CASE("Rendering edits apply only after Save succeeds", "[editor][settings-panel]")
{
    SettingsPanelFixture fixture;
    fixture.Click(fixture.mToggle);
    CHECK_FALSE(fixture.mSettings.rayTracing);
    CHECK_FALSE(std::filesystem::exists(fixture.mPath));
    fixture.Click(fixture.mSave);
    CHECK(fixture.mSettings.rayTracing);
    const auto saved = mir::RenderSettings::LoadFile(fixture.mPath);
    REQUIRE(saved);
    CHECK(saved->rayTracing);
}

TEST_CASE("Failed settings saves leave runtime values unchanged and retain the draft", "[editor][settings-panel]")
{
    SettingsPanelFixture fixture;
    fixture.Click(fixture.mToggle);
    const auto validPath = fixture.mPath;
    std::ofstream(validPath) << "{}";
    fixture.mPath = validPath / "settings.json";
    fixture.Click(fixture.mSave);
    CHECK_FALSE(fixture.mSettings.rayTracing);
    fixture.mPath = validPath;
    fixture.Click(fixture.mSave);
    CHECK(fixture.mSettings.rayTracing);
}

TEST_CASE("Closing settings discards unapplied edits", "[editor][settings-panel]")
{
    SettingsPanelFixture fixture;
    fixture.Click(fixture.mToggle);
    fixture.mPanel.OnHidden();
    fixture.Frame();
    fixture.Click(fixture.mSave);
    CHECK_FALSE(fixture.mSettings.rayTracing);
    const auto saved = mir::RenderSettings::LoadFile(fixture.mPath);
    REQUIRE(saved);
    CHECK_FALSE(saved->rayTracing);
}
