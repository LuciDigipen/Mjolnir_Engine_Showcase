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
#include <editor/history/History.h>
#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditOverlay.h>
#include <terrain_editor/TerrainHeightBrush.h>
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/World.h>
#include <project/GameProject.h>
#include <renderer/CameraMath.h>
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainScene.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

        bool InsideBounds(const TerrainWorldRect &bounds, WorldPosition point)
        {
            const glm::dvec3 meters(point.Meters());
            return meters.x >= bounds.minX && meters.x <= bounds.maxX && meters.z >= bounds.minZ &&
                   meters.z <= bounds.maxZ;
        }
    }

    class TerrainEditorModule::Extension final : public IEditorExtension
    {
    public:
        Extension(TerrainEditMode &mode, TerrainEditPanel &panel, TerrainLayerWorkspace &workspace,
            std::filesystem::path releasesRoot, TerrainOverlayApplier overlay)
            : mMode(mode), mPanel(panel), mWorkspace(workspace), mReleasesRoot(std::move(releasesRoot)),
              mApplyOverlay(std::move(overlay))
        {
        }

        void Update(const EditorExtensionContext &context) override
        {
            mMode.Update(context.mMode.mMode == EngineMode::Edit, context.mDocument);
            if (const TerrainWorld *world = context.mWorld.TryResource<TerrainWorld>())
                mWorkspace.Sync(*world);
            else
                mWorkspace.Clear();

            SyncTerrain();
            if (mPainting && (SelectedAsset() != mStrokeAsset || mWorkspace.SelectedLayer() != mStrokeSequence))
                FinishStroke(context.mHistory);
        }

        std::span<const char *const> WindowNames() const override
        {
            static constexpr const char *kWindows[]{ "Terrain" };
            return kWindows;
        }

        void DrawWindows(const EditorExtensionContext &context) override
        {
            if (!mMode.Active())
                return;
            if (ImGui::Begin("Terrain"))
                mPanel.Draw(mMode, mWorkspace, context.mHistory, Availability(context));
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
        AssetId SelectedAsset() const
        {
            const std::span<const TerrainEditAsset> assets = mWorkspace.Assets();
            const std::size_t index = mWorkspace.SelectedAsset();
            return index < assets.size() ? assets[index].asset : kNullAssetId;
        }

        std::optional<TerrainLocalPosition> LocalCursor(const EditorExtensionContext &context, AssetId asset,
            WorldPosition cursor) const
        {
            const TerrainWorld *world = context.mWorld.TryResource<TerrainWorld>();
            if (world == nullptr)
                return std::nullopt;
            for (const TerrainInstanceState &instance : world->Instances())
            {
                if (!(instance.placement.asset == asset) || !InsideBounds(instance.bounds, cursor))
                    continue;
                return TryWorldToTerrainLocal(cursor, instance.placement.translation);
            }
            return std::nullopt;
        }

        TerrainBrushAvailability Availability(const EditorExtensionContext &context)
        {
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            TerrainBrushAvailability availability = EvaluateTerrainBrushAvailability(
                mMode.Active(), context.mMode.mMode == EngineMode::Edit, context.mDocument == kSceneDocument,
                document != nullptr && document->Editable(), mWorkspace.SelectedLayer().has_value(), mBase.Ready(),
                mBase.Error());
            if (!mStrokeError.empty())
            {
                availability.enabled = false;
                availability.message = mStrokeError;
            }
            if (!mOverlayMessage.empty())
                availability.message =
                    availability.message.empty() ? mOverlayMessage : availability.message + "\n" + mOverlayMessage;
            return availability;
        }

        void SyncTerrain()
        {
            const AssetId asset = SelectedAsset();
            if (!(asset == mStatusAsset))
            {
                mStatusAsset = asset;
                mStrokeError.clear();
                mOverlayMessage.clear();
            }
            if (asset.IsNull())
            {
                mBase.Clear();
                return;
            }

            mBase.Ensure(mReleasesRoot, asset);
            const uint64_t baseKey = mBase.KeyValue();
            const auto knownKey = mBaseKeys.find(asset.value);
            if (knownKey == mBaseKeys.end() || knownKey->second != baseKey)
            {
                mBaseKeys[asset.value] = baseKey;
                mDocumentChanges.erase(asset.value);
            }

            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            if (document == nullptr || !document->Editable())
                return;

            const uint64_t change = document->ChangeCount();
            const auto knownChange = mDocumentChanges.find(asset.value);
            if (knownChange != mDocumentChanges.end() && knownChange->second == change)
                return;

            TerrainEditOverlaySession &session = mOverlays[asset.value];
            if (session.Empty())
            {
                mDocumentChanges[asset.value] = change;
                mOverlayMessage.clear();
                return;
            }
            if (!mBase.Ready())
            {
                mOverlayMessage = mBase.Error();
                return;
            }

            std::string reason;
            std::shared_ptr<const TerrainOverlay> overlay = session.Build(asset, document->Layers(), mBase.Provider(),
                reason);
            if (!overlay)
            {
                mOverlayMessage = std::move(reason);
                return;
            }
            mDocumentChanges[asset.value] = change;
            if (!mApplyOverlay)
                return;

            const TerrainOverlayApplyResult applied = mApplyOverlay(asset, std::move(overlay));
            mOverlayMessage = applied.applied ? std::string() : applied.reason;
        }

        TerrainBrushSettings BrushSettings() const
        {
            TerrainBrushSettings settings;
            settings.brush = mMode.Brush();
            settings.radius = static_cast<double>(mMode.Radius());
            settings.strength = static_cast<double>(mMode.Strength());
            settings.falloff = static_cast<double>(mMode.Falloff());
            settings.lower = ImGui::GetIO().KeyCtrl;
            return settings;
        }

        void BeginStroke(const EditorExtensionContext &context, WorldPosition cursor)
        {
            const AssetId asset = SelectedAsset();
            if (asset.IsNull() || !mBase.Ready() || mMode.Brush() == TerrainBrush::Paint)
                return;

            const std::optional<TerrainLocalPosition> local = LocalCursor(context, asset, cursor);
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            const std::optional<uint32_t> sequence = mWorkspace.SelectedLayer();
            const std::optional<TerrainLayout> reference = mBase.ReferenceLayout();
            if (!local || document == nullptr || !document->Editable() || !sequence || !reference)
                return;

            TerrainEditLayer *layer = document->MutableLayer(*sequence);
            if (layer == nullptr)
                return;

            TerrainBrushTileSource source{ *reference,
                [this](TileCoord tile) { return mBase.Layout(tile); }, mBase.Provider() };
            mStroke = std::make_unique<TerrainBrushStroke>(*layer, document->Layers(), std::move(source),
                local->Meters().y, [document]() { document->MarkChanged(); });
            mStrokeDocument = document;
            mStrokeAsset = asset;
            mStrokeSequence = *sequence;
            mStrokeError.clear();
            mPainting = true;
        }

        void Dab(const EditorExtensionContext &context, WorldPosition cursor)
        {
            if (!mStroke)
                return;
            const std::optional<TerrainLocalPosition> local = LocalCursor(context, mStrokeAsset, cursor);
            if (!local)
                return;

            const double seconds = std::clamp(static_cast<double>(ImGui::GetIO().DeltaTime), 0.0, 0.1);
            TerrainBrushDabResult result = mStroke->Dab(*local, BrushSettings(), seconds);
            if (!result.Ok())
            {
                mStrokeError = result.error;
                FinishStroke(context.mHistory);
                return;
            }
            if (!result.changed.empty())
                mOverlays[mStrokeAsset.value].MarkEdited(result.changed);
        }

        void FinishStroke(History &history)
        {
            if (mPainting && mStroke && mStrokeDocument != nullptr && !mStroke->Empty())
            {
                TerrainEditTileStates after = mStroke->After();
                if (!after.empty())
                {
                    history.Submit(std::make_unique<TerrainBrushCommand>(*mStrokeDocument, mStrokeSequence,
                        mStroke->Before(), std::move(after)), true);
                }
            }
            mStroke.reset();
            mStrokeDocument = nullptr;
            mPainting = false;
        }

        TerrainEditMode &mMode;
        TerrainEditPanel &mPanel;
        TerrainLayerWorkspace &mWorkspace;
        std::filesystem::path mReleasesRoot;
        TerrainOverlayApplier mApplyOverlay;
        TerrainBaseCache mBase;
        std::map<uint64_t, TerrainEditOverlaySession> mOverlays;
        std::map<uint64_t, uint64_t> mDocumentChanges;
        std::map<uint64_t, uint64_t> mBaseKeys;
        std::unique_ptr<TerrainBrushStroke> mStroke;
        TerrainLayerDocument *mStrokeDocument = nullptr;
        AssetId mStrokeAsset;
        AssetId mStatusAsset;
        uint32_t mStrokeSequence = 0;
        bool mPainting = false;
        bool mLeftWasDown = false;
        std::string mStrokeError;
        std::string mOverlayMessage;
    };

    void TerrainEditorModule::Extension::DrawOverlay(const EditorExtensionContext &context)
    {
        const bool leftDown = context.mInput.IsDown(MouseButton::Left);
        const bool justPressed = leftDown && !mLeftWasDown;
        mLeftWasDown = leftDown;
        if (mPainting && !leftDown)
            FinishStroke(context.mHistory);

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

        if (justPressed)
            BeginStroke(context, *cursor);
        if (mPainting && leftDown)
            Dab(context, *cursor);
    }

    TerrainEditorModule::TerrainEditorModule(ModuleContext, EditorModule &editor, const GameProject &project,
        TerrainOverlayApplier overlay)
        : mWorkspace(project.mAssetsDir),
          mExtension(std::make_unique<Extension>(mMode, mPanel, mWorkspace, TerrainReleasesRoot(project.mRootDir),
              std::move(overlay)))
    {
        mRegistration = editor.Get().AddExtension(*mExtension);
    }

    TerrainEditorModule::~TerrainEditorModule() = default;
} // namespace mir
