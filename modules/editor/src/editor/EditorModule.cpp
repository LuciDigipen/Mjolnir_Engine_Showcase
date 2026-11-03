/**
 * @file EditorModule.cpp
 * @author sumin.park
 * @brief The editor: its UI drawer, scene ops, asset import and content browser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorModule.h>

#include "ParticleEditorPanel.h"

#include <editor/CreateGameProject.h>
#include <editor/IdeSettings.h>

#include "MaterialEditor.h"

#include <animation/ModelPlayer.h>
#include <assets/AssetId.h>
#include <assets/AssetIdRoot.h>
#include <assets/AssetPath.h>
#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <editor/Selection.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <core/platform/Process.h>
#include <gameui/GameUIArea.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputMap.h>
#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabModule.h>
#include <prefab/PrefabSave.h>
#include <project/PlayerInfo.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/RendererModule.h>
#include <scene/SceneModule.h>
#include <window/WindowModule.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        // games found at startup, each with why it cannot open, if it cannot
        std::vector<ProjectMenuEntry> BuildProjectMenu()
        {
            std::vector<ProjectMenuEntry> menu;
            for (GameProject &project : DiscoverGameProjects())
            {
                // listed with the reason, not hidden: a game that vanished from the menu reads as lost
                ProjectMenuEntry entry{ project.mDir, project.mName, {} };
                try
                {
                    ResolveAssetIdRoot(project);
                }
                catch (const SetupError &error)
                {
                    entry.mUnavailable = error.What() + " (" + error.Cause() + ")";
                }
                menu.push_back(std::move(entry));
            }
            return menu;
        }

        DocumentId DocumentOf(PrefabId id) { return DocumentId{ id.mHigh, id.mLow }; }

        bool IsMaterialContentPath(std::string_view path) { return path.ends_with(".material.json"); }

        // The selected entity's authored material binding, when a component
        // exposes one by the documented name. The editor never includes terrain.
        std::optional<std::string> SelectedEntityMaterial(World &world, std::string_view assetIdRoot)
        {
            Selection *selection = world.TryResource<Selection>();
            if (!selection)
                return std::nullopt;
            const Entity entity = selection->Primary();
            if (entity.IsNull())
                return std::nullopt;

            for (const ComponentOps *ops : ComponentRegistry::Instance().All())
            {
                const FieldDesc *field = ops->FindField("materialPath");
                if (field == nullptr || field->mKind != FieldKind::String || !ops->Has(world, entity))
                    continue;
                const void *component = ops->GetComponent(world, entity);
                if (component == nullptr)
                    continue;

                char value[kFieldStringCapacity]{};
                field->Read(component, value);
                const std::string relative(value, strnlen(value, sizeof(value)));
                if (!IsMaterialContentPath(relative))
                    continue;
                const AssetPathResult logical = MakeLogicalAssetPath(assetIdRoot, relative);
                if (logical.Ok())
                    return logical.path;
            }
            return std::nullopt;
        }

        // Edit only: runs the open previews after PrefabSystem committed their documents, before Render
        class PrefabPreviewSystem final : public ISystem
        {
        public:
            PrefabPreviewSystem(PrefabWorkspace &workspace, PrefabCoordinator &prefabs, Editor &editor,
                std::function<void()> refreshHistory)
                : mWorkspace(workspace), mPrefabs(prefabs), mEditor(editor), mRefreshHistory(std::move(refreshHistory))
            {
            }

            void OnUpdate(SystemContext &context) override
            {
                if (IsEditing(context.world))
                    mWorkspace.Update(context.dt, context.elapsed, context.frame);
                mRefreshHistory();
                if (!IsEditing(context.world))
                    return;

                // reference clearing and binding changes at the safe point happen outside History
                if (mPrefabs.ConsumeSceneMaintenance() && !mEditor.IsSceneUnsaved())
                    mEditor.SetSceneUnsaved(true);
            }

        private:
            PrefabWorkspace &mWorkspace;
            PrefabCoordinator &mPrefabs;
            Editor &mEditor;
            std::function<void()> mRefreshHistory;
        };

        // true while a scene entity plays the model, by its id or, when it has none, by its name
        bool IsModelReferenced(World &world, const std::string &assetIdRoot, AssetId model, std::string_view logicalPath)
        {
            bool referenced = false;
            world.GetOrCreateQuery<const ModelPlayer>().ForEach([&](Entity, const ModelPlayer &player)
                {
                    if (!player.ModelAsset().IsNull())
                    {
                        referenced |= !model.IsNull() && player.ModelAsset() == model;
                        return;
                    }
                    if (player.ModelName().empty() || logicalPath.empty())
                        return;
                    const AssetPathResult named = MakeLogicalAssetPath(assetIdRoot, player.ModelName());
                    referenced |= named.Ok() && named.path == logicalPath;
                });
            return referenced;
        }
    }

    EditorModule::EditorModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window,
        RendererModule &renderer, SceneModule &scene, PrefabModule &prefab, AssetsModule &assets,
        AsyncAssetService *assetService, const AssetResolver &resolver, const GameProject &project, JobExecutor &jobs,
        bool enableLayout, std::shared_ptr<ModelLibrary> models)
        : mWorld(context.world), mRenderer(renderer), mScene(scene), mPrefab(prefab), mResolver(resolver),
          mProject(project), mAssets(&assets),
          mEnableLayout(enableLayout), mProjectMenu(BuildProjectMenu()),
          mPrefabAuthoring(context.world, prefab.Coordinator(), mProject.mRootDir, prefab.SourceDir()),
          mPrefabWorkspace(prefab.Coordinator(), mPrefabAuthoring),
          mSceneOps(context.world, scene.Scene(), mPrefabWorkspace, mProject.mRootDir)
    {
        mImport.Initialize(mProject);
        mParticleEditor = std::make_unique<ParticleEditorPanel>(mImport.SourceAssetRoot(),mProject.mAssetIdRoot);
        mContentBrowser.Initialize(mProject);
        mShaderHotReload.Initialize(mProject.mRootDir, &renderer.Renderer(), jobs);

        // The first frames run without assets; the import swaps them in once cooked
        if (!assets.HadManifest() && mImport.SourceHasAssets())
            mImport.QueueFullCook();

        if (!mEditor.Initialize(imgui.Runtime(), window.Get(), mProject.mProjectSettingsPath, context.world, mSceneOps,
                renderer.Renderer().SwapchainIsSrgb()))
        {
            mEditor.Shutdown();
            mImport.Shutdown();
            throw SetupError(ErrorCategory::Internal, "The editor could not start", {},
                "the log above names the step that failed",
                "This is an engine bug. Report it with the log attached: " + LogFilePath().string());
        }
        mEditor.SetAuthoringBackend(&mPrefabAuthoring);
        mEditor.SetAuthoredEntities(&mPrefabAuthoring);
        mEditor.SetBuildGame(BuildGame{ ProjectRoot(), mProject.mRootDir.lexically_normal(), mProject.mName,
            mProject.mProjectSettingsPath, ExecutableDir() / kPlayerInfoFileName });
        mBuildTool = BuildToolPath();
        mMaterialEditor = std::make_unique<MaterialEditor>();
        mPrefabWorkspace.BindResolveSystems(&renderer.Renderer(), &renderer.SurfaceMaterials(), assetService,
            resolver, std::move(models));
        context.scheduler.AddSystem<PrefabPreviewSystem>(SystemPhase::LateUpdate,
            SystemOrder{SystemOrder::kNormal}, mPrefabWorkspace,
            prefab.Coordinator(), mEditor, [this] { RefreshPrefabHistory(); });

        mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin, [this](SystemContext &frame)
            {
                mFrameDt = frame.dt;
                mImport.Update(IsEditing(mWorld));
                mShaderHotReload.Update();
                if (mImport.PendingApply() && IsEditing(mWorld))
                    mWorld.Resource<RequestQueue>().Push(Request::ApplyImportedAssets);
                mContentBrowser.Update();
                if (mContentBrowser.ConsumePrefabSourceChange())
                    mPrefab.Rescan();
                RefreshPrefabHistory();

                mBuild.Update();
                // the scene was written at the top of this frame, before this slot
                if (std::exchange(mBuildAfterSave, false))
                {
                    if (UnsavedForBuild().empty())
                        StartBuild(mPendingBuild);
                    else
                        mBuild.Fail("the save did not finish, so nothing was built; see the log");
                }
            });

        // Scene-local editor work ends here, independent prefab Worlds retain their resources.
        mSceneUnload = context.scheduler.AddStage(Stage::SceneUnload, [this](SystemContext &)
            {
                mEditor.CancelSnapToTerrain();
                mParticleEditor->OnHidden(mWorld);
            });

        // every handle the undo history holds dies with the scene it came from
        mSceneLoad = context.scheduler.AddStage(Stage::SceneLoad, [this, &scene](SystemContext &)
            {
                const SceneLoadResult &load = scene.LastLoad();
                mEditor.CancelSnapToTerrain();
                mEditor.ClearHistory();
                if (load.cause != SceneLoadCause::FromSnapshot)
                {
                    // a load that rebinds prefab instances marks the scene dirty
                    mEditor.SetSceneUnsaved(load.incomplete || scene.Scene().mDirty);
                    return;
                }

                // Play's edits are gone with Play's World, so the mark goes back to what it was
                mEditor.SetSceneUnsaved(mUnsavedBeforePlay);
                RestorePrefabPlayMarks(mPrefabMarksBeforePlay,
                    [this](DocumentId document) { mEditor.SetDocumentUnsaved(document, true); });
                // a prefab deleted during Play fails to open, and the Scene tab stays
                if (mPrefabBeforePlay.has_value())
                    mPrefabWorkspace.Open(*mPrefabBeforePlay);
                mPrefabBeforePlay.reset();
            });
        // the prefabs were written a frame before the scene; one changed since keeps the
        // scene unsaved, so the next Save writes both
        mSceneSaved = context.scheduler.AddStage(Stage::SceneSaved, [this](SystemContext &)
            {
                if (!PlanSaveScene(mPrefab.Coordinator().Registry(), mScene.Scene(), UnsavedPrefabs()).mSteps.empty())
                {
                    MIR_LOG_WARN("Save Scene: a prefab the scene uses changed before the scene was written; save again");
                    return;
                }
                mEditor.SetSceneUnsaved(false);
            });
        // after PrefabModule's, which committed and froze the prefabs
        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin, [this](SystemContext &)
            {
                mParticleEditor->OnHidden(mWorld);
                PrefabCoordinator &prefabs = mPrefab.Coordinator();
                if (prefabs.ConsumeSceneMaintenance() && !mEditor.IsSceneUnsaved())
                    mEditor.SetSceneUnsaved(true);
                mUnsavedBeforePlay = mEditor.IsSceneUnsaved();

                std::vector<DocumentId> documents;
                for (const PrefabId id : prefabs.DocumentIds())
                    documents.push_back(DocumentOf(id));
                RefreshPrefabHistory();
                mPrefabMarksBeforePlay = CapturePrefabPlayMarks(documents,
                    [this](DocumentId document) { return mEditor.IsDocumentUnsaved(document); });

                mEditor.CancelSnapToTerrain();
                mEditor.ClearHistory();
                mPrefabBeforePlay = mPrefabWorkspace.ActiveDocument();
                mPrefabWorkspace.ActivateScene();

                
                if (mAudio != nullptr && mEnableLayout)
                    mAudioBeforePlay = mAudio->Live();
            });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, [this](SystemContext &)
            {
                if (mAudio != nullptr && mAudioBeforePlay.has_value())
                    mAudio->SetLive(*mAudioBeforePlay);
                mAudioBeforePlay.reset();
            });

        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this](SystemContext &)
            {
                mPrefabWorkspace.CommitReload();
                mImport.MarkApplied();
                MIR_LOG_INFO("Applied imported assets at an Edit frame boundary");
            });

        mDrawer = imgui.AddDrawer(DrawerGroup::Tools, [this, &window, &assets]
            {
                if (window.Get().Width() == 0 || window.Get().Height() == 0)
                    return;

                World *preview = ActivePreviewWorld();
                const EditorActions actions = mEditor.DrawLayout(mEnableLayout, mWorld.Resource<InputMap>(),
                    mWorld.Resource<RenderSettings>(), mWorld.Resource<PostProcessingSettings>(),
                    mWorld.Resource<EngineModeState>(), mProjectMenu, mProject.mDir,
                    preview != nullptr ? *preview : mWorld, mWorld, MakeEditorDocuments(), mImport.View(),
                    mRenderer.Renderer(), MakeContentBrowserView(), mShowMaterialEditor,
                    mMaterialEditor->HasUnsavedDrafts(), MakeBuildJobView(), mFrameDt);
                if (const std::string particlePath = mEditor.TakeParticleOpenRequest();
                    !particlePath.empty() && IsEditing(mWorld))
                {
                    mParticleEditor->OpenSystem(particlePath, mWorld);
                    mEditor.ParticleEditorVisible() = true;
                }
                const int particleHistoryRequest = mEditor.TakeParticleHistoryRequest();
                if (particleHistoryRequest < 0) mParticleEditor->RequestUndoDraft();
                else if (particleHistoryRequest > 0) mParticleEditor->RequestRedoDraft();
                if (mEnableLayout)
                    mParticleEditor->Draw(mEditor.ParticleEditorVisible(),mWorld,assets.Cache(),mImport,mEditor.SceneCamera(),mFrameDt);
                else mParticleEditor->OnHidden(mWorld);
                if (const auto offsetEdit = mParticleEditor->TakeInstanceOffsetEdit())
                    mEditor.SubmitParticleEmitterOffset(mWorld, offsetEdit->mEntity, offsetEdit->mPlacementId, offsetEdit->mValue);
                mEditor.SetParticleHistoryAvailability(mParticleEditor->CanUndoDraft(),
                    mParticleEditor->CanRedoDraft());
                mPrefabWorkspace.CloseMissingDocuments();
                ApplyActions(actions);

                std::optional<std::string> selectedMaterial;
                const std::string &selectedContent = mEditor.ContentBrowserSelection();
                if (IsMaterialContentPath(selectedContent))
                    selectedMaterial = mContentBrowser.AssetLogicalPath(selectedContent);
                if (!selectedMaterial)
                    selectedMaterial = SelectedEntityMaterial(mWorld, mProject.mAssetIdRoot);
                mMaterialEditor->Select(selectedMaterial);

                MaterialEditorContext materialContext;
                materialContext.mMaterials = &mRenderer.SurfaceMaterials();
                materialContext.mCatalog = &mRenderer.Renderer().SurfaceMaterials();
                materialContext.mReimportMaterial = [this](std::string_view logicalPath, bool force)
                { return mImport.ReimportMaterial(logicalPath, force); };
                materialContext.mMaterialCookRevision = [this](std::string_view logicalPath)
                { return mImport.MaterialCookRevision(logicalPath); };
                materialContext.mMaterialCookFailureRevision = [this](std::string_view logicalPath)
                { return mImport.MaterialCookFailureRevision(logicalPath); };
                materialContext.mLoadCooked = [this](std::string_view logicalPath, MaterialCookedView &view,
                                                  std::string &error)
                {
                    SurfaceMaterialAsset asset;
                    if (mAssets == nullptr ||
                        !LoadTypedAsset(mAssets->Cache(), mResolver.ResolveFull(logicalPath), asset, error))
                        return false;
                    view.shaderType = asset.shaderType;
                    view.parametersJson = asset.parametersJson;
                    view.frontends = asset.frontends;
                    view.resolution = asset.resolution;
                    view.textures.clear();
                    for (const SurfaceMaterialTexture &texture : asset.textures)
                        view.textures.push_back({texture.role, texture.path,
                            texture.colorSpace == TextureColorSpace::Srgb ? "srgb" : "linear"});
                    return true;
                };
                materialContext.mSourceAssetRoot = mImport.SourceAssetRoot();
                materialContext.mAssetIdRoot = mProject.mAssetIdRoot;
                materialContext.mProjectKey = mProject.mDir;
                mMaterialEditor->Draw(mEnableLayout ? &mShowMaterialEditor : nullptr,
                    mEnableLayout && IsEditing(mWorld), mImport.View(), materialContext);
                FeedViewport();
            });
    }

    EditorModule::~EditorModule()
    {
        mReloadCommit.Reset();
        mPlayEnd.Reset();
        mPlayBegin.Reset();
        mSceneSaved.Reset();
        mSceneLoad.Reset();
        mSceneUnload.Reset();
        mDrawer.Reset();
        mFrameBegin.Reset();
        mEditor.Shutdown();
        // stops a running cooker and a running build
        mImport.Shutdown();
        mBuild.Cancel();
        mShaderHotReload.Shutdown();
    }

    bool EditorModule::HasUnsavedMaterialDrafts() const
    {
        return mMaterialEditor != nullptr && mMaterialEditor->HasUnsavedDrafts();
    }

    void EditorModule::FeedViewport()
    {
        RenderSystem &rendering = mRenderer.Rendering();
        const VkRect2D viewport = mEditor.SceneViewportRect();
        rendering.SetViewportRect(0, mEnableLayout ? viewport : VkRect2D{});
        // set in Edit only, but drawn in Play too
        rendering.SetViewportDebugView(0, mEnableLayout ? mEditor.SceneDebugView() : DebugViewSettings{});

        // the game UI draws later this frame, into the same scene viewport
        GameUIArea &area = mWorld.Resource<GameUIArea>();
        area.mSet = mEnableLayout;
        area.mRect = UIRect{ static_cast<float>(viewport.offset.x), static_cast<float>(viewport.offset.y),
                             static_cast<float>(viewport.extent.width), static_cast<float>(viewport.extent.height) };

        // only Edit feeds the editor camera, and only Edit shows a Prefab tab's preview
        const bool editorView = IsEditing(mWorld) && mEnableLayout;
        if (editorView)
        {
            mEditor.UpdateCamera(mFrameDt);
            EditorCamera &camera = mEditor.SceneCamera();
            rendering.SetExternalCamera(0, camera.Pose(), camera.mLens);
            if (camera.TakeCut())
                rendering.CutViewport(0);
        }
        else
        {
            rendering.SetViewportCamera(0, kNullEntity);
        }
        rendering.SetSourceWorld(editorView ? ActivePreviewWorld() : nullptr);
    }

    void EditorModule::ApplyActions(const EditorActions &actions)
    {
        if (actions.mToggleMaterialEditor)
            mShowMaterialEditor = !mShowMaterialEditor;

        RequestQueue &requests = mWorld.Resource<RequestQueue>();
        switch (actions.mScene)
        {
        case SceneMenuAction::New:
            requests.Push(Request::NewScene);
            break;
        case SceneMenuAction::Save:
            SaveActiveDocument();
            break;
        case SceneMenuAction::Load:
            requests.Push(Request::LoadScene);
            break;
        case SceneMenuAction::Play:
            requests.Push(Request::Play);
            break;
        case SceneMenuAction::TogglePause:
            requests.Push(IsPaused(mWorld) ? Request::Resume : Request::Pause);
            break;
        case SceneMenuAction::Stop:
            requests.Push(Request::Stop);
            break;
        case SceneMenuAction::None:
            break;
        }

        bool assetActionAccepted = true;
        if (actions.mAssetImport.mKind == AssetImportActionKind::Remove)
        {
            AssetId target = mResolver.ResolveFull(actions.mAssetImport.mModelPath);
            for (const auto &model : mImport.View().mModels)
                if (model.mPath == actions.mAssetImport.mModelPath && model.mAssetId != 0)
                    target = AssetId{ model.mAssetId };
            assetActionAccepted =
                !IsModelReferenced(mWorld, mProject.mAssetIdRoot, target, actions.mAssetImport.mModelPath);
            if (!assetActionAccepted)
                mImport.Reject(actions.mAssetImport.mModelPath,
                    "cannot remove this asset while a scene entity references it");
        }
        if (assetActionAccepted)
            mImport.Handle(actions.mAssetImport);

        if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenInIde)
        {
            OpenInExternalIde(actions.mContentBrowser.mTargetPath);
        }
        else if (actions.mContentBrowser.mKind == ContentBrowserActionKind::NewParticleSystem)
        {
            const auto &folder = actions.mContentBrowser.mTargetPath;
            const std::string assetFolder = folder == "assets" ? "" :
                (folder.starts_with("assets/") ? folder.substr(7) : "particles");
            mParticleEditor->PrepareNewSystem(assetFolder);
            mEditor.ParticleEditorVisible() = true;
        }
        else if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenParticleSystem)
        {
            const auto &path = actions.mContentBrowser.mTargetPath;
            if (IsEditing(mWorld) && path.starts_with("assets/"))
            {
                mParticleEditor->OpenSystem(path.substr(7), mWorld);
                mEditor.ParticleEditorVisible() = true;
            }
        }
        else if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenPrefab)
        {
            if (IsEditing(mWorld))
                OpenPrefabAsset(actions.mContentBrowser.mTargetPath);
        }
        else
        {
            bool contentBrowserActionAccepted = true;
            if (actions.mContentBrowser.mKind == ContentBrowserActionKind::Delete)
            {
                if (const std::optional<std::string> logical =
                    mContentBrowser.AssetLogicalPath(actions.mContentBrowser.mTargetPath);
                    logical.has_value())
                {
                    contentBrowserActionAccepted =
                        !IsModelReferenced(mWorld, mProject.mAssetIdRoot, mResolver.ResolveFull(*logical), *logical);
                    if (!contentBrowserActionAccepted)
                        mContentBrowser.Reject(actions.mContentBrowser.mTargetPath,
                            "cannot delete this asset while a scene entity references it");
                }
            }
            if (contentBrowserActionAccepted)
                mContentBrowser.Handle(actions.mContentBrowser);
        }

        if (actions.mOpenScene)
        {
            if (actions.mOpenScene->saveCurrent)
            {
                mPrefabWorkspace.SaveScene(mScene.Scene(), UnsavedPrefabs(), MarkSavedPrefabs(), [&]
                    {
                        requests.PushOpenScene(*actions.mOpenScene);
                        return true;
                    });
            }
            else
                requests.PushOpenScene(*actions.mOpenScene);
        }

        if (!actions.mOpenProject.empty())
            requests.PushOpenProject(actions.mOpenProject);

        if (!actions.mCreateProjectName.empty() &&
            CreateGameProject(actions.mCreateProjectName, actions.mCreateProjectRayTracing))
        {
            requests.PushOpenProject("games/" + actions.mCreateProjectName);
        }

        if (IsEditing(mWorld))
        {
            const DocumentId document = actions.mDocument.mDocument;
            const PrefabId prefab{ document.mHigh, document.mLow };
            switch (actions.mDocument.mKind)
            {
            case EditorDocumentActionKind::Activate:
                if (document == kSceneDocument)
                    mPrefabWorkspace.ActivateScene();
                else
                    mPrefabWorkspace.Open(prefab);
                break;
            case EditorDocumentActionKind::Close:
                if (document != kSceneDocument)
                    mPrefabWorkspace.Close(prefab);
                break;
            case EditorDocumentActionKind::None:
                break;
            }
        }

        if (actions.mSaveActiveDocument)
            SaveActiveDocument();

        ApplyBuildAction(actions.mBuild);
    }

    void EditorModule::ApplyBuildAction(const BuildAction &action)
    {
        switch (action.mCommand)
        {
        case BuildCommand::Build:
            if (mBuild.Running() || mBuildAfterSave)
                return;
            if (const std::string blocked = BuildBlockedReason(); !blocked.empty())
            {
                MIR_LOG_WARN("Build: {}", blocked);
                return;
            }
            if (const std::string unsaved = UnsavedForBuild(); !unsaved.empty())
            {
                if (mNonInteractive)
                {
                    MIR_LOG_WARN("Build: unsaved changes in {} (--non-interactive answers Cancel)", unsaved);
                    return;
                }
                mPendingBuild = action;
                mBuildAskSave = unsaved;
                return;
            }
            StartBuild(action);
            break;
        case BuildCommand::SaveThenBuild:
            mBuildAskSave.clear();
            if (!SaveForBuild())
            {
                mBuild.Fail("the save failed, so nothing was built; see the log");
                return;
            }
            mPendingBuild = action;
            mBuildAfterSave = true;
            break;
        case BuildCommand::DeclineSave:
            mBuildAskSave.clear();
            break;
        case BuildCommand::Cancel:
            mBuild.Cancel();
            break;
        case BuildCommand::None:
            break;
        }
    }

    void EditorModule::StartBuild(const BuildAction &action)
    {
        if (const std::string blocked = BuildBlockedReason(); !blocked.empty())
        {
            mBuild.Fail(blocked);
            return;
        }

        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        const std::filesystem::path result =
            (ec ? ExecutableDir() : temp) / ("mjolnir-editor-build-" + std::to_string(CurrentProcessId()) + ".json");
        mBuild.Start(BuildJobRequest{ mBuildTool, mProject.mRootDir, action.mPlayerDir, action.mOutDir, result });
    }

    std::string EditorModule::BuildBlockedReason() const
    {
        if (!IsEditing(mWorld))
            return "Stop Play to build";
        if (mImport.View().mProcessRunning)
            return "Wait for the asset import to finish";
        if (mBuildTool.empty())
            return "asset_workspace.json names no MjolnirBuild; rebuild the editor";
        return {};
    }

    std::string EditorModule::UnsavedForBuild() const
    {
        std::size_t prefabs = 0;
        const PrefabWorkspace::PrefabUnsaved unsaved = UnsavedPrefabs();
        for (const PrefabId id : mPrefab.Coordinator().DocumentIds())
        {
            if (unsaved(id))
                ++prefabs;
        }

        std::string text = mEditor.IsSceneUnsaved() ? "the scene" : "";
        if (prefabs > 0)
            text += (text.empty() ? "" : " and ") + std::to_string(prefabs) + (prefabs == 1 ? " Prefab" : " Prefabs");
        return text;
    }

    bool EditorModule::SaveForBuild()
    {
        bool saved = true;
        const PrefabWorkspace::PrefabUnsaved unsaved = UnsavedPrefabs();
        for (const PrefabId id : mPrefab.Coordinator().DocumentIds())
        {
            // an earlier save may have written it as a dependency
            if (unsaved(id))
                saved = mPrefabWorkspace.SavePrefab(id, unsaved, MarkSavedPrefabs()) && saved;
        }
        if (!saved || !mEditor.IsSceneUnsaved())
            return saved;

        return mPrefabWorkspace.SaveScene(mScene.Scene(), unsaved, MarkSavedPrefabs(), [this]
            {
                mWorld.Resource<RequestQueue>().Push(Request::SaveScene);
                return true;
            });
    }

    BuildJobView EditorModule::MakeBuildJobView() const
    {
        BuildJobView view;
        view.mRunning = mBuild.Running();
        view.mSaving = mBuildAfterSave;
        view.mSeconds = mBuild.Seconds();
        view.mBlocked = BuildBlockedReason();
        view.mAskSave = mBuildAskSave;
        view.mResult = mBuild.Result() ? &*mBuild.Result() : nullptr;
        return view;
    }

    void EditorModule::SaveActiveDocument()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("EditorModule::SaveActiveDocument: ignored while Play is running (Stop first)");
            return;
        }

        if (const std::optional<PrefabId> active = mPrefabWorkspace.ActiveDocument())
        {
            mPrefabWorkspace.SavePrefab(*active, UnsavedPrefabs(), MarkSavedPrefabs());
            return;
        }

        // the prefabs are written now and the scene at the top of the next frame; a prefab
        // that fails to write leaves the scene unwritten
        mPrefabWorkspace.SaveScene(mScene.Scene(), UnsavedPrefabs(), MarkSavedPrefabs(), [this]
            {
                mWorld.Resource<RequestQueue>().Push(Request::SaveScene);
                return true;
            });
    }

    PrefabWorkspace::PrefabUnsaved EditorModule::UnsavedPrefabs() const
    {
        return [this](PrefabId id)
            {
                return !mPrefab.Coordinator().Registry().HasSavedFile(id) ||
                    mEditor.IsDocumentUnsaved(DocumentOf(id));
            };
    }

    PrefabWorkspace::PrefabSaved EditorModule::MarkSavedPrefabs()
    {
        return [this](PrefabId id) { mEditor.SetDocumentUnsaved(DocumentOf(id), false); };
    }

    ContentBrowserView EditorModule::MakeContentBrowserView()
    {
        ContentBrowserView view = mContentBrowser.View();
        const PrefabRegistry &registry = mPrefab.Coordinator().Registry();

        std::set<std::string> known;
        for (const ContentBrowserEntryView &entry : view.mEntries)
            known.insert(entry.mRelativePath);

        for (const PrefabId id : mPrefab.Coordinator().DocumentIds())
        {
            if (!registry.HasMemoryDocument(id) || registry.HasSavedFile(id))
                continue;
            const std::optional<std::filesystem::path> intended = registry.IntendedPathOf(id);
            if (!intended)
                continue;

            const std::string relative = intended->lexically_relative(mProject.mRootDir).generic_string();
            if (relative.empty() || relative.rfind("..", 0) == 0)
                continue;

            for (std::size_t slash = relative.find('/'); slash != std::string::npos;
                 slash = relative.find('/', slash + 1))
            {
                const std::string directory = relative.substr(0, slash);
                if (!known.insert(directory).second)
                    continue;
                ContentBrowserEntryView entry;
                entry.mRelativePath = directory;
                entry.mName = std::filesystem::path(directory).filename().string();
                entry.mKind = ContentBrowserEntryKind::Directory;
                view.mEntries.push_back(std::move(entry));
            }

            if (!known.insert(relative).second)
                continue;
            ContentBrowserEntryView entry;
            entry.mRelativePath = relative;
            entry.mName = intended->filename().string();
            entry.mKind = ContentBrowserEntryKind::File;
            entry.mUnsaved = true;
            view.mEntries.push_back(std::move(entry));
        }
        return view;
    }

    void EditorModule::RefreshPrefabHistory()
    {
        mPrefabWorkspace.InvalidateExternalSavePoints([this](PrefabId id)
            { mEditor.SetDocumentUnsaved(DocumentOf(id), true); });
    }

    World *EditorModule::ActivePreviewWorld()
    {
        if (!IsEditing(mWorld))
            return nullptr;
        const std::optional<PrefabId> active = mPrefabWorkspace.ActiveDocument();
        return active.has_value() ? mPrefabWorkspace.PreviewWorld(*active) : nullptr;
    }

    EditorDocuments EditorModule::MakeEditorDocuments()
    {
        EditorDocuments documents;
        documents.mTabs.push_back(
            EditorDocumentTab{ kSceneDocument, mScene.Scene().mName, mEditor.IsSceneUnsaved(), false });
        documents.mActive = kSceneDocument;
        documents.mEnabled = IsEditing(mWorld);

        const PrefabWorkspace::PrefabUnsaved unsaved = UnsavedPrefabs();
        for (const PrefabId id : mPrefabWorkspace.OpenDocuments())
            documents.mTabs.push_back(EditorDocumentTab{ DocumentOf(id), mPrefabWorkspace.LabelOf(id), unsaved(id), true });

        if (const std::optional<PrefabId> active = mPrefabWorkspace.ActiveDocument())
            documents.mActive = DocumentOf(*active);
        return documents;
    }

    void EditorModule::OpenPrefabAsset(const std::string &relativePath)
    {
        const std::optional<PrefabId> id = mPrefabWorkspace.FindByPath(mProject.mRootDir / relativePath);
        if (!id.has_value())
        {
            MIR_LOG_ERROR("Content browser: '{}' is not a registered Prefab", relativePath);
            mContentBrowser.Reject(relativePath, "not a registered Prefab");
            return;
        }

        if (!mPrefabWorkspace.Open(*id))
        {
            MIR_LOG_ERROR("Content browser: could not open '{}'", relativePath);
            mContentBrowser.Reject(relativePath, "could not open this Prefab");
        }
    }

    void EditorModule::OpenInExternalIde(const std::string &relativePath)
    {
        const IdeSettings settings = IdeSettings::LoadFile(mProject.mProjectSettingsPath).value_or(IdeSettings{});
        const std::filesystem::path ide =
            settings.path.empty() ? DetectDefaultIde().value_or(std::filesystem::path{})
            : std::filesystem::path(settings.path);
        if (ide.empty())
        {
            MIR_LOG_WARN("Content browser: no IDE configured or detected for '{}'", relativePath);
            mContentBrowser.Reject(
                relativePath, "no IDE configured and none could be auto-detected - set one in Project Settings");
            return;
        }

        const std::filesystem::path absolute = mProject.mRootDir / relativePath;
        const std::vector<std::string> args = settings.BuildArgs(mProject.mRootDir, absolute);

        std::string argsLogged;
        for (const std::string &arg : args)
            argsLogged += (argsLogged.empty() ? "" : " ") + arg;
        MIR_LOG_INFO("Content browser: opening '{}' (project root '{}') with '{}' {}", absolute.string(),
            mProject.mRootDir.string(), ide.string(), argsLogged);

        if (!LaunchProcess(ide, args, false).has_value())
        {
            MIR_LOG_ERROR("Content browser: could not launch '{}' for '{}'", ide.string(), relativePath);
            mContentBrowser.Reject(relativePath, "could not launch '" + ide.string() + "'");
        }
    }
}
