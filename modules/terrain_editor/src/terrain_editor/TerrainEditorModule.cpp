/**
 * @file TerrainEditorModule.cpp
 * @author Jackey Cen
 * @brief Terrain editing, registered with the editor as an extension.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditorModule.h>

#include <editor/EditorExtension.h>
#include <editor/EditorModule.h>
#include <editor/EditorWidgets.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/World.h>
#include <renderer/CameraMath.h>
#include <renderer/ViewSubmission.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <optional>

namespace mir
{
    namespace
    {
        constexpr int kTerrainRingSegments = 48;

        bool ProjectToScreen(const glm::mat4 &viewProj, float x0, float y0, float width, float height,
            const glm::vec3 &worldPos, ImVec2 &out)
        {
            const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
            if (clip.w <= 0.0f)
                return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            out = ImVec2(x0 + (ndc.x * 0.5f + 0.5f) * width, y0 + (ndc.y * 0.5f + 0.5f) * height);
            return true;
        }
    }

    class TerrainEditorModule::Extension final : public IEditorExtension
    {
    public:
        Extension(TerrainEditMode &mode, TerrainEditPanel &panel) : mMode(mode), mPanel(panel) {}

        void Update(const EditorExtensionContext &context) override
        {
            mMode.Update(context.mMode.mMode == EngineMode::Edit, context.mDocument);
        }

        std::span<const char *const> WindowNames() const override
        {
            static constexpr const char *kWindows[]{ "Terrain" };
            return kWindows;
        }

        void DrawWindows(const EditorExtensionContext &) override
        {
            if (!mMode.Active())
                return;
            if (ImGui::Begin("Terrain"))
                mPanel.Draw(mMode);
            ImGui::End();
        }

        void DrawToolbar(const EditorExtensionContext &context) override
        {
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::BeginDisabled(context.mDocument != kSceneDocument);
            if (widgets::ToolbarButton(ICON_FA_MOUNTAIN, "Terrain edit mode", mMode.Active()))
                mMode.Toggle(context.mMode.mMode == EngineMode::Edit, context.mDocument);
            ImGui::EndDisabled();
        }

        bool OwnsViewport() const override { return mMode.Active(); }

        void DrawOverlay(const EditorExtensionContext &context) override;

    private:
        TerrainEditMode &mMode;
        TerrainEditPanel &mPanel;
    };

    void TerrainEditorModule::Extension::DrawOverlay(const EditorExtensionContext &context)
    {
        if (!mMode.Active() || context.mMode.mMode != EngineMode::Edit || context.mOverlay == nullptr)
            return;

        const float x0 = static_cast<float>(context.mViewport.x);
        const float y0 = static_cast<float>(context.mViewport.y);
        const float width = static_cast<float>(context.mViewport.width);
        const float height = static_cast<float>(context.mViewport.height);
        if (width <= 0.0f || height <= 0.0f)
            return;

        if (context.mWantsCaptureMouse)
            return;

        const GroundQuery *ground = context.mWorld.TryResource<GroundQuery>();
        if (ground == nullptr || !ground->Bound())
            return;

        const RawInputSnapshot &input = context.mInput;
        const float px = static_cast<float>(input.mouseX) - x0;
        const float py = static_cast<float>(input.mouseY) - y0;
        if (px < 0.0f || py < 0.0f || px >= width || py >= height)
            return;

        const glm::vec2 ndc{ (px / width) * 2.0f - 1.0f, (py / height) * 2.0f - 1.0f };
        const WorldPose camera = context.mCamera.Pose();
        const WorldRay ray = MakeCameraRay(camera, context.mCamera.mLens, width / height, ndc);
        const std::optional<WorldPosition> cursor = FindTerrainCursor(ray, kTerrainCursorMaxDistance,
            [ground](WorldPosition point) { return ground->Sample(point); });
        if (!cursor)
            return;

        const glm::mat4 viewProj = MakeCameraRelativeViewProjection(camera, context.mCamera.mLens, width / height);
        const glm::dvec3 center(cursor->Meters());
        const double radius = static_cast<double>(mMode.Radius());

        ImDrawList *drawList = context.mOverlay;
        drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);

        ImVec2 previous{};
        bool hasPrevious = false;
        for (int i = 0; i <= kTerrainRingSegments; ++i)
        {
            const double angle = static_cast<double>(i) / kTerrainRingSegments * 2.0 * glm::pi<double>();
            const std::optional<WorldPosition> query = WorldPosition::FromDoubleMeters(
                { center.x + radius * std::cos(angle), center.y, center.z + radius * std::sin(angle) });
            if (!query)
            {
                hasPrevious = false;
                continue;
            }

            const GroundQueryResult result = ground->Sample(*query);
            if (!result.IsReady())
            {
                hasPrevious = false;
                continue;
            }

            const std::optional<WorldOffset> offset = TryWorldDifference(*result.surface, camera.position);
            const std::optional<glm::vec3> local = offset ? TryRendererLocalFloat(*offset) : std::nullopt;
            ImVec2 screen{};
            if (!local || !ProjectToScreen(viewProj, x0, y0, width, height, *local, screen))
            {
                hasPrevious = false;
                continue;
            }

            if (hasPrevious)
                drawList->AddLine(previous, screen, IM_COL32(255, 190, 60, 220), 2.0f);
            previous = screen;
            hasPrevious = true;
        }

        drawList->PopClipRect();
    }

    TerrainEditorModule::TerrainEditorModule(ModuleContext, EditorModule &editor)
        : mExtension(std::make_unique<Extension>(mMode, mPanel))
    {
        mRegistration = editor.Get().AddExtension(*mExtension);
    }

    TerrainEditorModule::~TerrainEditorModule() = default;
} // namespace mir
