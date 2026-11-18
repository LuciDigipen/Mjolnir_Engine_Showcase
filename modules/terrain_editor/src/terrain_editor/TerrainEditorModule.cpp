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
#include <editor/Selection.h>
#include <editor/history/ComponentCommands.h>
#include <editor/history/History.h>
#include <terrain_editor/TerrainBake.h>
#include <terrain_editor/TerrainBakeJob.h>
#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditOverlay.h>
#include <terrain_editor/TerrainHeightBrush.h>
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>
#include <terrain_editor/TerrainMaterialLayersDraft.h>
#include <terrain_editor/TerrainPaintBrush.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/World.h>
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <project/GameProject.h>
#include <renderer/CameraMath.h>
#include <renderer/TerrainMaterialAsset.h>
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainScene.h>
#include <terrain/components/TerrainInstance.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

        std::string BakeStatus(const TerrainBakeReport &report)
        {
            switch (report.mOutcome)
            {
            case TerrainBakeOutcome::Published:
                return report.mRelease ? std::format("Published release {}", TerrainIdHex(report.mRelease->value))
                                       : "Published.";
            case TerrainBakeOutcome::UpToDate:
                return report.mRelease ? std::format("Up to date; release {}", TerrainIdHex(report.mRelease->value))
                                       : "Up to date.";
            case TerrainBakeOutcome::Canceled:
                return "Bake canceled.";
            case TerrainBakeOutcome::Failed:
                return report.mMessage;
            }
            return {};
        }

        std::string FieldString(const char *value)
        {
            return std::string(value, strnlen(value, kFieldStringCapacity));
        }

        std::vector<std::string> ScanTerrainSurfaceMaterials(const std::filesystem::path &assetsRoot)
        {
            std::vector<std::string> materials;
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(assetsRoot, ec), end;
            if (ec)
                return materials;

            for (; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                std::error_code fileError;
                if (!it->is_regular_file(fileError) || fileError)
                    continue;

                const std::filesystem::path path = it->path();
                const std::string name = path.filename().string();
                if (!name.ends_with(".material.json") || name.ends_with(".terrainmaterial.json"))
                    continue;

                const std::optional<std::string> text = ReadText(path);
                if (!text)
                    continue;

                nlohmann::json document;
                try
                {
                    document = nlohmann::json::parse(*text);
                }
                catch (const nlohmann::json::exception &)
                {
                    continue;
                }
                if (!document.is_object())
                    continue;

                const auto frontends = document.find("frontends");
                if (frontends == document.end() || !frontends->is_array())
                    continue;
                const bool terrain = std::ranges::any_of(*frontends, [](const nlohmann::json &frontend) {
                    return frontend.is_string() && frontend.get<std::string>() == "terrain";
                });
                if (!terrain)
                    continue;

                std::error_code relativeError;
                const std::filesystem::path relative = std::filesystem::relative(path, assetsRoot, relativeError);
                if (!relativeError)
                    materials.push_back(relative.generic_string());
            }
            std::ranges::sort(materials);
            return materials;
        }
    }

    class TerrainEditorModule::Extension final : public IEditorExtension
    {
    public:
        Extension(TerrainEditMode &mode, TerrainEditPanel &panel, TerrainLayerWorkspace &workspace,
            std::filesystem::path releasesRoot, TerrainBakeProject bakeProject, TerrainOverlayApplier overlay,
            std::function<bool(const std::filesystem::path &)> queueCook)
            : mMode(mode), mPanel(panel), mWorkspace(workspace), mReleasesRoot(std::move(releasesRoot)),
              mBakeProject(std::move(bakeProject)), mApplyOverlay(std::move(overlay)), mQueueCook(std::move(queueCook))
        {
        }

        void Update(const EditorExtensionContext &context) override
        {
            mMode.Update(context.mMode.mMode == EngineMode::Edit, context.mDocument);
            mBake.Update();
            CollectBakeResult();
            ApplyBakeDiscard(context);
            if (const TerrainWorld *world = context.mWorld.TryResource<TerrainWorld>())
                mWorkspace.Sync(*world);
            else
                mWorkspace.Clear();

            SyncTerrain();
            SyncMaterialDraft(context);
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
            {
                if (!(SelectedAsset() == mMaterialDraftAsset))
                    ReloadMaterialDraft(context, SelectedAsset());

                TerrainLayerDocument *document = mWorkspace.SelectedDocument();
                RefreshPaintedLayers(document, SelectedAsset());

                std::string chosenMaterial = ChosenMaterial();
                const TerrainPanelActions actions = mPanel.Draw(mMode, mWorkspace, context.mHistory,
                    Availability(context), BakeView(context), MaterialLayersView(context, document),
                    mWeights.Layers(), chosenMaterial);
                if (mWeights.Ready() && chosenMaterial != ChosenMaterial())
                    mMaterialLayers[SelectedAsset().value] = std::move(chosenMaterial);
                if (actions.mRescanMaterials)
                    mSurfaceMaterials = ScanTerrainSurfaceMaterials(mWorkspace.AssetsDir());
                if (actions.mRevertMaterials)
                    ReloadMaterialDraft(context, SelectedAsset());
                if (actions.mApplyMaterials)
                    ApplyMaterialLayers(context);
                if (actions.mStartBake)
                    StartBake();
                if (actions.mCancelBake)
                    mBake.Cancel();
            }
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

        std::string ChosenMaterial() const
        {
            const auto found = mMaterialLayers.find(SelectedAsset().value);
            if (found == mMaterialLayers.end())
                return {};
            const std::span<const std::string> layers = mWeights.Layers();
            if (std::find(layers.begin(), layers.end(), found->second) == layers.end())
                return {};
            return found->second;
        }

        TerrainBrushAvailability Availability(const EditorExtensionContext &context)
        {
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            const bool editing = context.mMode.mMode == EngineMode::Edit;
            const bool sceneDocument = context.mDocument == kSceneDocument;
            const bool documentEditable = document != nullptr && document->Editable();
            const bool hasLayer = mWorkspace.SelectedLayer().has_value();

            TerrainBrushAvailability availability;
            if (mMode.Brush() == TerrainBrush::Paint)
            {
                availability = EvaluateTerrainPaintAvailability(mMode.Active(), editing, sceneDocument,
                    documentEditable, hasLayer, mWeights.Ready(), mWeights.Error(), !ChosenMaterial().empty(),
                    mMaterialRecipeHasMaterials);
            }
            else
            {
                availability = EvaluateTerrainBrushAvailability(mMode.Active(), editing, sceneDocument,
                    documentEditable, hasLayer, mBase.Ready(), mBase.Error());
            }
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
                mWeights.Clear();
                return;
            }

            mBase.Ensure(mReleasesRoot, asset);
            mWeights.Ensure(mReleasesRoot, asset);
            const uint64_t baseKey = mBase.KeyValue();
            const auto knownKey = mBaseKeys.find(asset.value);
            if (knownKey == mBaseKeys.end() || knownKey->second != baseKey)
            {
                mBaseKeys[asset.value] = baseKey;
                mDocumentChanges.erase(asset.value);
            }
            const uint64_t weightsKey = mWeights.KeyValue();
            const auto knownWeightsKey = mWeightsKeys.find(asset.value);
            if (knownWeightsKey == mWeightsKeys.end() || knownWeightsKey->second != weightsKey)
            {
                mWeightsKeys[asset.value] = weightsKey;
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
            if (session.HasHeightEdits() && !mBase.Ready())
            {
                mOverlayMessage = mBase.Error();
                return;
            }
            if (session.HasWeightEdits() && !mWeights.Ready())
            {
                mOverlayMessage = mWeights.Error();
                return;
            }

            const std::span<const std::string> releaseWeightLayers =
                mBase.Ready() ? mBase.ReleaseWeightLayers() : mWeights.Layers();
            std::string reason;
            std::shared_ptr<const TerrainOverlay> overlay =
                session.Build(asset, document->Layers(), mBase.Provider(), mWeights.Provider(), mWeights.Layers(),
                    releaseWeightLayers, weightsKey, reason);
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

        Entity TerrainEntityFor(World &world, std::string_view recipePath) const
        {
            if (const Selection *selection = world.TryResource<Selection>())
            {
                const Entity primary = selection->Primary();
                if (const TerrainInstance *instance =
                        primary.IsNull() ? nullptr : world.GetComponent<TerrainInstance>(primary);
                    instance != nullptr && FieldString(instance->recipePath) == recipePath)
                {
                    return primary;
                }
            }

            Entity only = kNullEntity;
            std::size_t count = 0;
            world.GetOrCreateQuery<const TerrainInstance>().ForEach(
                [&](Entity entity, const TerrainInstance &instance) {
                    if (FieldString(instance.recipePath) != recipePath)
                        return;
                    only = entity;
                    ++count;
                });
            return count == 1 ? only : kNullEntity;
        }

        std::string BoundMaterial(World &world, std::string_view recipePath) const
        {
            const Entity entity = TerrainEntityFor(world, recipePath);
            if (entity.IsNull())
                return {};
            const TerrainInstance *instance = world.GetComponent<TerrainInstance>(entity);
            return instance != nullptr ? FieldString(instance->materialPath) : std::string();
        }

        void SyncMaterialDraft(const EditorExtensionContext &context)
        {
            const AssetId asset = SelectedAsset();
            if (!(asset == mMaterialDraftAsset) || mMaterialReload)
                ReloadMaterialDraft(context, asset);
        }

        void ReloadMaterialDraft(const EditorExtensionContext &context, AssetId asset)
        {
            if (!(asset == mMaterialDraftAsset))
            {
                mMaterialStatus.clear();
                mMaterialFailed = false;
            }
            mMaterialDraftAsset = asset;
            mMaterialReload = false;
            mMaterialDraft.Clear();
            mMaterialDraftError.clear();
            mMaterialRecipeHasMaterials = true;

            const std::span<const TerrainEditAsset> assets = mWorkspace.Assets();
            const std::size_t index = mWorkspace.SelectedAsset();
            if (asset.IsNull() || index >= assets.size())
            {
                mMaterialDraftError = "No terrain asset is selected.";
                return;
            }

            TerrainMaterialLayersInput input;
            input.assetsRoot = mWorkspace.AssetsDir();
            input.recipePath = input.assetsRoot / assets[index].recipePath;
            input.boundMaterial = BoundMaterial(context.mWorld, assets[index].recipePath);

            std::string error;
            if (!mMaterialDraft.Load(input, error))
            {
                mMaterialDraftError = std::move(error);
                return;
            }
            mMaterialRecipeHasMaterials = mMaterialDraft.RecipeHasMaterials();
        }

        void RefreshPaintedLayers(TerrainLayerDocument *document, AssetId asset)
        {
            const uint64_t change = document != nullptr ? document->ChangeCount() : 0;
            // Every dab bumps the change count; rescan once the stroke ends.
            if (mPainting || (asset == mPaintedAsset && change == mPaintedChange))
                return;

            mPaintedAsset = asset;
            mPaintedChange = change;
            mPaintedLayers.clear();
            if (document == nullptr)
                return;

            std::set<std::string> names;
            for (const TerrainEditLayer &layer : document->Layers())
            {
                for (const auto &[tile, edit] : layer.tiles)
                {
                    (void)tile;
                    for (const TerrainEditWeight &weight : edit.weights)
                        names.insert(weight.layer);
                }
            }
            mPaintedLayers.assign(names.begin(), names.end());
        }

        TerrainMaterialLayersView MaterialLayersView(const EditorExtensionContext &context,
            TerrainLayerDocument *document)
        {
            TerrainMaterialLayersView view;
            view.mDraft = mMaterialDraftError.empty() && mMaterialDraft.Loaded() ? &mMaterialDraft : nullptr;
            view.mMaterials = mSurfaceMaterials;
            view.mPaintedLayers = mPaintedLayers;
            view.mLoadError = mMaterialDraftError;
            view.mStatus = mMaterialStatus;
            view.mFailed = mMaterialFailed;
            if (view.mDraft == nullptr)
                return view;

            if (context.mMode.mMode != EngineMode::Edit)
                view.mDisabledReason = "Material layers work in Edit.";
            else if (context.mDocument != kSceneDocument)
                view.mDisabledReason = "Material layers write the scene's terrain, not a Prefab.";
            else if (document == nullptr)
                view.mDisabledReason = "The selected terrain has no Edit Layers.";
            else if (!document->Editable())
                view.mDisabledReason = "The selected terrain's Edit Layers cannot be read.";
            else if (mBake.Running())
                view.mDisabledReason = "A bake is running.";
            else
                view.mCanApply = true;
            return view;
        }

        bool SubmitMaterialPath(const EditorExtensionContext &context, Entity entity, std::string_view path)
        {
            const ComponentOps *ops = ComponentRegistry::Instance().FindBySeq(TypeIdOf<TerrainInstance>().seq);
            const FieldDesc *field = ops != nullptr ? ops->FindField("materialPath") : nullptr;
            if (field == nullptr || field->mKind != FieldKind::String)
                return false;

            FieldValue value;
            value.mField = static_cast<uint32_t>(field - ops->mFields.data());
            std::memcpy(value.mBytes, path.data(),
                std::min(path.size(), static_cast<std::size_t>(kFieldStringCapacity) - 1));
            context.mHistory.Submit(MakeSetFieldsCommand(entity, *ops, { value }, "Bind Terrain Material"), true);
            return true;
        }

        void FailMaterial(std::string message)
        {
            mMaterialStatus = std::move(message);
            mMaterialFailed = true;
        }

        void SucceedMaterial(std::string message)
        {
            mMaterialStatus = std::move(message);
            mMaterialFailed = false;
        }

        void ApplyMaterialLayers(const EditorExtensionContext &context)
        {
            mMaterialStatus.clear();
            mMaterialFailed = false;

            const std::span<const TerrainEditAsset> assets = mWorkspace.Assets();
            const std::size_t index = mWorkspace.SelectedAsset();
            if (SelectedAsset().IsNull() || index >= assets.size() || !mMaterialDraft.Loaded())
            {
                FailMaterial("No terrain material layers are loaded.");
                return;
            }
            if (const std::string invalid = mMaterialDraft.Validate(); !invalid.empty())
            {
                FailMaterial(invalid);
                return;
            }

            // Everything that can refuse is checked before the recipe is rewritten.
            const Entity entity = TerrainEntityFor(context.mWorld, assets[index].recipePath);
            if (entity.IsNull())
            {
                FailMaterial("Select the terrain entity to bind.");
                return;
            }
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            if (document == nullptr || !document->Editable())
            {
                FailMaterial("The selected terrain's Edit Layers cannot be read.");
                return;
            }

            const TerrainMaterialLayersWriteResult written = WriteTerrainMaterialLayersFiles(mMaterialDraft);
            if (!written.error.empty())
            {
                FailMaterial(written.error);
                return;
            }
            mMaterialRecipeHasMaterials = true;
            MigrateTerrainMaterialChannels(*document, mMaterialDraft.MakePlan());
            // The runtime only sees the layered material once it is cooked; auto import may be off.
            if (!written.layeredMaterial.empty() && mQueueCook)
                mQueueCook(mWorkspace.AssetsDir() / written.layeredMaterial);

            const std::string binding = written.layeredMaterial.empty() ? mMaterialDraft.Layers().front().material
                                                                        : written.layeredMaterial;
            std::string note;
            const TerrainInstance *instance = context.mWorld.GetComponent<TerrainInstance>(entity);
            const std::string current = instance != nullptr ? FieldString(instance->materialPath) : std::string();
            if (current != binding)
            {
                // A layered file this draft did not load belongs to another setup.
                if (IsTerrainMaterialPath(current) && current != mMaterialDraft.LayeredMaterialPath())
                {
                    note = std::format(" The terrain entity already uses '{}'; its material binding was left alone.",
                        current);
                }
                else if (!SubmitMaterialPath(context, entity, binding))
                    note = " The terrain entity's material could not be changed.";
            }

            mMaterialReload = true;
            SucceedMaterial("Applied." + note);
            StartBake();
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
            if (asset.IsNull())
                return;

            const std::optional<TerrainLocalPosition> local = LocalCursor(context, asset, cursor);
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            const std::optional<uint32_t> sequence = mWorkspace.SelectedLayer();
            if (!local || document == nullptr || !document->Editable() || !sequence)
                return;

            TerrainEditLayer *layer = document->MutableLayer(*sequence);
            if (layer == nullptr)
                return;

            if (mMode.Brush() == TerrainBrush::Paint)
            {
                const std::optional<TerrainLayout> reference = mWeights.Layout();
                const std::string material = ChosenMaterial();
                if (!mWeights.Ready() || !reference || material.empty())
                    return;
                mPaintStroke = std::make_unique<TerrainPaintStroke>(*layer, *reference, material,
                    [document]() { document->MarkChanged(); });
                mStroke.reset();
            }
            else
            {
                const std::optional<TerrainLayout> reference = mBase.ReferenceLayout();
                if (!mBase.Ready() || !reference)
                    return;
                TerrainBrushTileSource source{ *reference,
                    [this](TileCoord tile) { return mBase.Layout(tile); }, mBase.Provider() };
                mStroke = std::make_unique<TerrainBrushStroke>(*layer, document->Layers(), std::move(source),
                    local->Meters().y, [document]() { document->MarkChanged(); });
                mPaintStroke.reset();
            }

            mStrokeDocument = document;
            mStrokeAsset = asset;
            mStrokeSequence = *sequence;
            mStrokeError.clear();
            mPainting = true;
        }

        void Dab(const EditorExtensionContext &context, WorldPosition cursor)
        {
            if (!mStroke && !mPaintStroke)
                return;
            const std::optional<TerrainLocalPosition> local = LocalCursor(context, mStrokeAsset, cursor);
            if (!local)
                return;

            const double seconds = std::clamp(static_cast<double>(ImGui::GetIO().DeltaTime), 0.0, 0.1);
            TerrainBrushDabResult result;
            if (mPaintStroke)
                result = mPaintStroke->Dab(*local, BrushSettings(), seconds);
            else
                result = mStroke->Dab(*local, BrushSettings(), seconds);
            if (!result.Ok())
            {
                mStrokeError = result.error;
                FinishStroke(context.mHistory);
                return;
            }
            if (!result.changed.empty())
            {
                TerrainEditOverlaySession &session = mOverlays[mStrokeAsset.value];
                if (mPaintStroke)
                    session.MarkWeightEdited(result.changed);
                else
                    session.MarkEdited(result.changed);
            }
        }

        void FinishStroke(History &history)
        {
            if (mPainting && mStrokeDocument != nullptr)
            {
                const bool paint = mPaintStroke != nullptr;
                const bool empty = paint ? !mPaintStroke || mPaintStroke->Empty() : !mStroke || mStroke->Empty();
                if (!empty)
                {
                    TerrainEditTileStates before = paint ? mPaintStroke->Before() : mStroke->Before();
                    TerrainEditTileStates after = paint ? mPaintStroke->After() : mStroke->After();
                    if (!after.empty())
                    {
                        history.Submit(std::make_unique<TerrainBrushCommand>(*mStrokeDocument, mStrokeSequence,
                            std::move(before), std::move(after),
                            paint ? "Paint Terrain Material" : "Edit Terrain Height"), true);
                    }
                }
            }
            mStroke.reset();
            mPaintStroke.reset();
            mStrokeDocument = nullptr;
            mPainting = false;
        }

        TerrainBakeView BakeView(const EditorExtensionContext &context)
        {
            TerrainBakeView view;
            view.mBaking = mBake.Running();
            view.mSeconds = mBake.Seconds();
            view.mCanCancel = view.mBaking;

            if (view.mBaking)
                view.mStatus = std::format("Baking {:.1f} s", view.mSeconds);
            else if (!mBakeError.empty())
            {
                view.mStatus = mBakeError;
                view.mFailed = true;
            }
            else if (const std::optional<TerrainBakeResult> &result = mBake.Result())
            {
                view.mStatus = BakeStatus(result->mReport);
                view.mFailed = result->mReport.mOutcome == TerrainBakeOutcome::Failed;
            }
            else
                view.mStatus = "Idle";

            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            if (context.mMode.mMode != EngineMode::Edit)
                view.mReason = "Bake works in Edit.";
            else if (context.mDocument != kSceneDocument)
                view.mReason = "Bake publishes the scene's terrain, not a Prefab.";
            else if (mWorkspace.Assets().empty())
                view.mReason = "The scene has no terrain assets.";
            else if (mBakeProject.mTool.empty())
                view.mReason = "The editor does not know where TerrainTool is; rebuild the editor.";
            else if (document == nullptr)
                view.mReason = "The selected terrain has no Edit Layers.";
            else if (!document->Editable())
                view.mReason = "The selected terrain's Edit Layers cannot be read.";
            else
                view.mCanStart = !view.mBaking;
            return view;
        }

        void StartBake()
        {
            const std::span<const TerrainEditAsset> assets = mWorkspace.Assets();
            const std::size_t index = mWorkspace.SelectedAsset();
            TerrainLayerDocument *document = mWorkspace.SelectedDocument();
            if (index >= assets.size() || document == nullptr || mBake.Running())
                return;

            std::string error;
            std::optional<TerrainBakeRequest> request =
                MakeTerrainBakeRequest(mBakeProject, assets[index].recipePath, error);
            if (!request)
            {
                mBakeError = std::move(error);
                return;
            }

            TerrainBakeStart start;
            start.mRequest = std::move(*request);
            start.mStartChange = document->ChangeCount();
            start.mPrepare = SaveTerrainLayersForBake(*document);

            mBakeDocument = document;
            mPendingBake.reset();
            mBakeError.clear();
            if (!mBake.Start(std::move(start)))
            {
                mBakeError = mBake.Message();
                return;
            }
            MIR_LOG_INFO("Terrain bake: syncing {}", assets[index].recipePath);
        }

        void CollectBakeResult()
        {
            if (mBake.Completed() == mSeenBakeCompleted)
                return;
            mSeenBakeCompleted = mBake.Completed();

            const std::optional<TerrainBakeResult> &result = mBake.Result();
            if (!result)
                return;

            if (result->mReport.mOutcome == TerrainBakeOutcome::Failed)
                MIR_LOG_ERROR("Terrain bake: {}", result->mReport.mMessage);
            else
                MIR_LOG_INFO("Terrain bake: {}", BakeStatus(result->mReport));

            if (result->mReport.mOutcome == TerrainBakeOutcome::Published ||
                result->mReport.mOutcome == TerrainBakeOutcome::UpToDate)
            {
                mPendingBake = PendingBake{ result->mAsset, result->mStartChange, result->mReport, mBakeDocument };
                if (result->mAsset == SelectedAsset())
                    mMaterialReload = true;
            }
        }

        void ApplyBakeDiscard(const EditorExtensionContext &context)
        {
            if (!mPendingBake)
                return;

            const TerrainReleaseInfo *loaded = nullptr;
            if (const TerrainWorld *world = context.mWorld.TryResource<TerrainWorld>())
            {
                for (const TerrainWorld::AssetState &asset : world->Assets())
                {
                    if (asset.asset == mPendingBake->mAsset && asset.release)
                    {
                        loaded = &asset.release->Info();
                        break;
                    }
                }
            }

            const uint64_t change = mPendingBake->mDocument ? mPendingBake->mDocument->ChangeCount()
                                                           : mPendingBake->mStartChange;
            switch (EvaluateTerrainBakeDiscard(mPendingBake->mReport, mPendingBake->mStartChange, change, loaded))
            {
            case TerrainBakeOverlayAction::Keep:
                mPendingBake.reset();
                return;
            case TerrainBakeOverlayAction::Wait:
                return;
            case TerrainBakeOverlayAction::Discard:
                break;
            }

            const AssetId asset = mPendingBake->mAsset;
            mPendingBake.reset();
            mOverlays[asset.value].Clear();
            mDocumentChanges[asset.value] = change;
            if (mApplyOverlay)
            {
                const TerrainOverlayApplyResult applied = mApplyOverlay(asset, nullptr);
                if (!applied.applied)
                    mOverlayMessage = applied.reason;
            }
            MIR_LOG_INFO("Terrain bake: dropped the overlay; the loaded release holds its edits");
        }

        struct PendingBake
        {
            AssetId mAsset;
            uint64_t mStartChange = 0;
            TerrainBakeReport mReport;
            TerrainLayerDocument *mDocument = nullptr;
        };

        TerrainEditMode &mMode;
        TerrainEditPanel &mPanel;
        TerrainLayerWorkspace &mWorkspace;
        std::filesystem::path mReleasesRoot;
        TerrainBakeProject mBakeProject;
        TerrainOverlayApplier mApplyOverlay;
        std::function<bool(const std::filesystem::path &)> mQueueCook;
        TerrainBaseCache mBase;
        TerrainBaseWeightsCache mWeights;
        std::map<uint64_t, TerrainEditOverlaySession> mOverlays;
        std::map<uint64_t, uint64_t> mDocumentChanges;
        std::map<uint64_t, uint64_t> mBaseKeys;
        std::map<uint64_t, uint64_t> mWeightsKeys;
        std::map<uint64_t, std::string> mMaterialLayers;
        TerrainMaterialLayersDraft mMaterialDraft;
        AssetId mMaterialDraftAsset;
        std::string mMaterialDraftError;
        std::string mMaterialStatus;
        std::vector<std::string> mSurfaceMaterials;
        std::vector<std::string> mPaintedLayers;
        AssetId mPaintedAsset;
        uint64_t mPaintedChange = ~0ull;
        bool mMaterialReload = false;
        bool mMaterialFailed = false;
        bool mMaterialRecipeHasMaterials = true;
        std::unique_ptr<TerrainBrushStroke> mStroke;
        std::unique_ptr<TerrainPaintStroke> mPaintStroke;
        TerrainLayerDocument *mStrokeDocument = nullptr;
        AssetId mStrokeAsset;
        AssetId mStatusAsset;
        uint32_t mStrokeSequence = 0;
        bool mPainting = false;
        bool mLeftWasDown = false;
        std::string mStrokeError;
        std::string mOverlayMessage;
        TerrainBakeJob mBake;
        TerrainLayerDocument *mBakeDocument = nullptr;
        std::optional<PendingBake> mPendingBake;
        uint64_t mSeenBakeCompleted = 0;
        std::string mBakeError;
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
              TerrainBakeProject{ project.mRootDir, project.mAssetsDir, project.mAssetIdRoot, TerrainToolPath() },
              std::move(overlay),
              [&editor](const std::filesystem::path &path) { return editor.Import().QueueChanged(path); }))
    {
        mRegistration = editor.Get().AddExtension(*mExtension);
    }

    TerrainEditorModule::~TerrainEditorModule() = default;
} // namespace mir
