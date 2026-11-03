/**
 * @file Editor.cpp
 * @author Sumin Park
 * @brief The Slate editor shell (dockspace, Hierarchy/Inspector/Output
 *        panels, Debug menu), drawn into the context ImGuiRuntime owns.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/Editor.h>

#include <core/ecs/ComponentGizmo.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/Query.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <editor/EditorWidgets.h>
#include <physics_layers/PhysicsLayers.h>
#include <editor/Selection.h>
#include <editor/history/AuthoredEdit.h>
#include <editor/history/CommandGroup.h>
#include <editor/history/ComponentCommands.h>
#include <editor/history/DocumentNodeCommands.h>
#include <editor/history/EntityCommands.h>
#include <editortheme/EditorTheme.h>
#include <renderer/CameraMath.h>
#include <renderer/ViewSubmission.h>
#include <renderer/components/MeshBounds.h>
#include <renderer/components/MeshRenderer.h>
#include <particles/components/ParticleSystem.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr float kPrefabDropDistance = 10.0f;
        constexpr float kPrefabDropPlaneLimit = 1000.0f;
        constexpr float kPrefabDropMinRate = 1e-4f;

        // ImGui text is UTF-8; path::string() is the ANSI code page on Windows
        std::string ToUtf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        ImGuizmo::OPERATION ToImGuizmoOperation(GizmoOperation operation)
        {
            switch (operation)
            {
            case GizmoOperation::Translate:
                return ImGuizmo::TRANSLATE;
            case GizmoOperation::Rotate:
                return ImGuizmo::ROTATE;
            case GizmoOperation::Scale:
                return ImGuizmo::SCALE;
            }
            return ImGuizmo::TRANSLATE;
        }

        template <typename T>
        void AddTransformField(std::vector<FieldValue> &values, const ComponentOps &ops, std::string_view fieldName,
            const T &value)
        {
            const FieldDesc *field = ops.FindField(fieldName);
            if (field == nullptr)
                return;

            FieldValue fieldValue;
            fieldValue.mField = static_cast<uint32_t>(field - ops.mFields.data());
            static_assert(sizeof(T) <= sizeof(fieldValue.mBytes));
            std::memcpy(fieldValue.mBytes, &value, sizeof(T));
            values.push_back(fieldValue);
        }

        // The selected entities the gizmo can move, in selection order, so the primary stays last.
        std::vector<Entity> GizmoSelection(World &world)
        {
            std::vector<Entity> entities;
            if (const Selection *selection = world.TryResource<Selection>())
            {
                for (const Entity entity : selection->mSelected)
                {
                    if (world.IsAlive(entity) && world.GetComponent<Transform>(entity) != nullptr)
                        entities.push_back(entity);
                }
            }
            return entities;
        }

        // The mean of the entities' origins, on the unscaled axes of the last one.
        std::optional<WorldPose> SelectionPivot(World &world, std::span<const Entity> entities)
        {
            const WorldPose primary = ResolveWorld(world, entities.back());

            // summed as offsets from the primary, so far positions keep their precision
            WorldVec3 sum(0);
            for (const Entity entity : entities)
            {
                const std::optional<WorldOffset> offset =
                    TryWorldDifference(ResolveWorld(world, entity).position, primary.position);
                if (!offset)
                    return std::nullopt;
                sum += offset->Meters();
            }

            const std::optional<WorldOffset> mean =
                WorldOffset::FromMeters(sum / static_cast<WorldReal>(entities.size()));
            const std::optional<WorldPosition> position =
                mean ? TryTranslateWorld(primary.position, *mean) : std::nullopt;
            if (!position)
                return std::nullopt;

            glm::quat rotation{};
            glm::vec3 scale{};
            DecomposeRotationScale(primary.linear, rotation, scale);
            return WorldPose{ *position, glm::mat3_cast(glm::normalize(rotation)) };
        }

        // Radius around the entity's origin that holds its scaled mesh box.
        float BoundsRadius(const MeshBounds &bounds, const glm::mat3 &linear)
        {
            float radius = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 corner{ (i & 1) != 0 ? bounds.mLocalMax.x : bounds.mLocalMin.x,
                                       (i & 2) != 0 ? bounds.mLocalMax.y : bounds.mLocalMin.y,
                                       (i & 4) != 0 ? bounds.mLocalMax.z : bounds.mLocalMin.z };
                radius = std::max(radius, glm::length(linear * corner));
            }
            return radius;
        }

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

        // A segment that passes behind the camera is cut there rather than dropped, so a long
        // gizmo line still shows when the scene camera stands inside it.
        void DrawWorldLine(ImDrawList *drawList, const glm::mat4 &viewProj, float x0, float y0, float width,
            float height, const glm::vec3 &a, const glm::vec3 &b, ImU32 color, float thickness)
        {
            constexpr float kMinW = 0.01f;
            glm::vec4 clipA = viewProj * glm::vec4(a, 1.0f);
            glm::vec4 clipB = viewProj * glm::vec4(b, 1.0f);
            if (clipA.w <= kMinW && clipB.w <= kMinW)
                return;
            if (clipA.w <= kMinW)
                clipA = glm::mix(clipA, clipB, (kMinW - clipA.w) / (clipB.w - clipA.w));
            else if (clipB.w <= kMinW)
                clipB = glm::mix(clipB, clipA, (kMinW - clipB.w) / (clipA.w - clipB.w));

            const auto toScreen = [&](const glm::vec4 &clip)
                {
                    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                    return ImVec2(x0 + (ndc.x * 0.5f + 0.5f) * width, y0 + (ndc.y * 0.5f + 0.5f) * height);
                };
            drawList->AddLine(toScreen(clipA), toScreen(clipB), color, thickness);
        }

        void DrawWorldCircle(ImDrawList *drawList, const glm::mat4 &viewProj, float x0, float y0, float width,
            float height, const glm::vec3 &center, const glm::vec3 &right, const glm::vec3 &up,
            float radius, ImU32 color, int segments)
        {
            glm::vec3 prev = center + right * radius;
            for (int i = 1; i <= segments; ++i)
            {
                const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * glm::pi<float>();
                const glm::vec3 next = center + right * (radius * std::cos(angle)) + up * (radius * std::sin(angle));
                DrawWorldLine(drawList, viewProj, x0, y0, width, height, prev, next, color, 1.5f);
                prev = next;
            }
        }

        // Lets a component's owning module draw viewport gizmos (GizmoCanvas) without depending on ImGui.
        class ScreenGizmoCanvas final : public GizmoCanvas
        {
        public:
            ScreenGizmoCanvas(ImDrawList *drawList, const glm::mat4 &viewProj, float x0, float y0, float width,
                float height)
                : mDrawList(drawList), mViewProj(viewProj), mX0(x0), mY0(y0), mWidth(width), mHeight(height)
            {}

            void Line(const glm::vec3 &a, const glm::vec3 &b, uint32_t colorRGBA, float thickness) const override
            {
                DrawWorldLine(mDrawList, mViewProj, mX0, mY0, mWidth, mHeight, a, b,
                    static_cast<ImU32>(colorRGBA), thickness);
            }

            void Circle(const glm::vec3 &center, const glm::vec3 &right, const glm::vec3 &up, float radius,
                uint32_t colorRGBA, int segments) const override
            {
                DrawWorldCircle(mDrawList, mViewProj, mX0, mY0, mWidth, mHeight, center, right, up, radius,
                    static_cast<ImU32>(colorRGBA), segments);
            }

            float Aspect() const override { return mHeight > 0.0f ? mWidth / mHeight : 1.0f; }

        private:
            ImDrawList *mDrawList;
            glm::mat4 mViewProj;
            float mX0, mY0, mWidth, mHeight;
        };


        // screen pixels from the pie's center to its items, and the radius where nothing is picked
        constexpr float kViewPieRadius = 110.0f;
        constexpr float kViewPieDeadZone = 24.0f;

        ImDrawList *ViewportOverlayDrawList()
        {
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::Begin("##ViewportOverlay", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            ImGui::End();
            ImGui::PopStyleVar();
            return drawList;
        }

        // The tab bar over the scene viewport; the viewport itself starts below it.
        float DrawDocumentTabs(float x, float y, float width, const EditorDocuments &documents, EditorActions &actions,
            bool revealActive)
        {
            const float height = ImGui::GetFrameHeight();

            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            const bool open = ImGui::Begin("##DocumentTabs", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    ImGuiWindowFlags_NoFocusOnAppearing);

            if (open)
            {
                ImGui::BeginDisabled(!documents.mEnabled);
                if (ImGui::BeginTabBar("##DocumentTabBar"))
                {
                    for (const EditorDocumentTab &tab : documents.mTabs)
                    {
                        ImGuiTabItemFlags flags =
                            tab.mUnsaved ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
                        if (revealActive && tab.mDocument == documents.mActive)
                            flags |= ImGuiTabItemFlags_SetSelected;

                        // the ### id keeps the tab stable as its label or unsaved marker changes
                        const std::string label = std::format("{}###doc{:016x}{:016x}", tab.mLabel,
                            tab.mDocument.mHigh, tab.mDocument.mLow);
                        bool tabOpen = true;
                        const bool visible =
                            ImGui::BeginTabItem(label.c_str(), tab.mClosable ? &tabOpen : nullptr, flags);
                        const bool clicked = ImGui::IsItemClicked();
                        if (visible)
                            ImGui::EndTabItem();

                        if (tab.mClosable && !tabOpen)
                        {
                            actions.mDocument =
                                EditorDocumentAction{ EditorDocumentActionKind::Close, tab.mDocument };
                        }
                        else if (clicked && tab.mDocument != documents.mActive)
                        {
                            actions.mDocument =
                                EditorDocumentAction{ EditorDocumentActionKind::Activate, tab.mDocument };
                        }
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndDisabled();
            }

            ImGui::End();
            ImGui::PopStyleVar(2);
            return height;
        }
    } // namespace

    Editor::~Editor() {}

    bool Editor::Initialize(const ImGuiRuntime &imgui, Window &window, std::filesystem::path projectSettingsPath,
        World &world, ISceneOps &sceneOps, bool srgbOutput)
    {
        if (!imgui.IsInitialized())
        {
            MIR_LOG_ERROR("Editor needs an initialized ImGuiRuntime");
            return false;
        }

        mProjectSettingsPath = std::move(projectSettingsPath);

        world.EmplaceResource<Selection>();

        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        mImGuiIniPath = (ExecutableDir() / "imgui.ini").string();
        io.IniFilename = mImGuiIniPath.c_str();

        mPreferencesPath = EditorPreferencesPath();
        mPreferences = EditorPreferences::LoadFile(mPreferencesPath);

        mWindow = &window;
        mSrgbOutput = srgbOutput;
        ApplyTheme();

        if (!mPreferences.backgroundFile.empty())
        {
            std::string error;
            if (!mBackground.Load(EditorPreferencesDir() / mPreferences.backgroundFile, error))
                MIR_LOG_WARN("Editor background image '{}': {}", mPreferences.backgroundFile, error);
        }

        mSceneOps = &sceneOps;
        mInitialized = true;
        return true;
    }

    void Editor::ApplyTheme()
    {
        const EditorThemeKind kind =
            mPreferences.theme == EditorThemeChoice::Slate ? EditorThemeKind::Slate : EditorThemeKind::Midnight;
        EditorTheme::Apply(kind, mSrgbOutput);
        EditorTheme::ScaleForDpi(mWindow->ContentScale());
    }

    void Editor::ApplyPreferencesRequest(const PreferencesRequest &request)
    {
        mThemePending |= request.mApplyTheme;
        bool save = request.mSave;
        if (request.mPickImage)
        {
            if (const std::optional<std::filesystem::path> picked = PickImageFile())
                save |= SetBackgroundImage(*picked);
        }
        if (request.mClearImage)
        {
            ClearBackgroundImage();
            save = true;
        }

        if (save && !mPreferences.SaveFile(mPreferencesPath))
            mPreferencesPanel.SetStatus("Save failed - see log.");
    }

    bool Editor::SetBackgroundImage(const std::filesystem::path &source)
    {
        const std::filesystem::path dir = EditorPreferencesDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string extension = source.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string file = "background" + extension;
        const std::filesystem::path temp = dir / (file + ".tmp");

        std::filesystem::copy_file(source, temp, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            mPreferencesPanel.SetStatus("Cannot copy the image: " + ec.message());
            return false;
        }

        std::string error;
        if (!mBackground.Load(temp, error))
        {
            std::filesystem::remove(temp, ec);
            mPreferencesPanel.SetStatus("Cannot use " + ToUtf8(source.filename()) + ": " + error);
            return false;
        }

        if (!mPreferences.backgroundFile.empty() && mPreferences.backgroundFile != file)
            std::filesystem::remove(dir / mPreferences.backgroundFile, ec);
        std::filesystem::rename(temp, dir / file, ec);
        if (ec)
        {
            // the stored copy is gone, so the image would not come back next run
            mPreferencesPanel.SetStatus("Cannot store the image: " + ec.message());
            mBackground.Clear();
            std::filesystem::remove(temp, ec);
            mPreferences.backgroundFile.clear();
            return true;
        }

        mPreferences.backgroundFile = file;
        mPreferences.backgroundEnabled = true;
        mPreferencesPanel.SetStatus("Using " + ToUtf8(source.filename()) + ".");
        return true;
    }

    void Editor::ClearBackgroundImage()
    {
        mBackground.Clear();
        if (!mPreferences.backgroundFile.empty())
        {
            std::error_code ec;
            std::filesystem::remove(EditorPreferencesDir() / mPreferences.backgroundFile, ec);
            mPreferences.backgroundFile.clear();
        }
        mPreferencesPanel.SetStatus("Cleared.");
    }

    void Editor::SubmitParticleEmitterOffset(World &world, Entity entity, uint32_t placementId, const glm::vec3 &offset)
    {
        const ComponentOps *ops = ComponentRegistry::Instance().FindBySeq(TypeIdOf<ParticleSystem>().seq);
        const ParticleSystem *component = world.GetComponent<ParticleSystem>(entity);
        if (ops == nullptr || component == nullptr || placementId == 0)
            return;

        // The offset sits in the slot that already holds this placement, else in a free one, which also records the id.
        size_t slot = FindEmitterOffsetSlot(*component, placementId);
        const bool claimsSlot = slot == kNoEmitterOffsetSlot;
        if (claimsSlot)
            slot = FindFreeEmitterOffsetSlot(*component);
        if (slot == kNoEmitterOffsetSlot)
            return;

        const auto valueOf = [&](const std::string &fieldName, const void *bytes, size_t size, FieldValue &out)
        {
            const FieldDesc *field = ops->FindField(fieldName);
            if (field == nullptr)
                return false;
            out.mField = static_cast<uint32_t>(field - ops->mFields.data());
            std::memcpy(out.mBytes, bytes, size);
            return true;
        };
        std::vector<FieldValue> values(claimsSlot ? 2 : 1);
        if (!valueOf("emitterOffset" + std::to_string(slot), &offset, sizeof(offset), values[0]) ||
            (claimsSlot && !valueOf("emitterOffsetPlacement" + std::to_string(slot), &placementId, sizeof(placementId), values[1])))
            return;
        const Name *name = world.GetComponent<Name>(entity);
        mHistory.Submit(MakeSetFieldsEditCommand(entity, *ops, std::move(values),
            std::format("Move particle emitter {}", name != nullptr ? name->mValue : "Entity"), mAuthoredEntities));
    }

    void Editor::Shutdown()
    {
        if (!mInitialized)
            return;


        ImGuiIO &io = ImGui::GetIO();
        if (io.IniFilename != nullptr && ImGui::GetCurrentContext()->SettingsLoaded)
            ImGui::SaveIniSettingsToDisk(io.IniFilename);

        io.IniFilename = nullptr;

        mWindow = nullptr;
        mInitialized = false;
    }

    void Editor::DrawTitleBar()
    {
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float scale = mWindow->ContentScale();
        const float barHeight = Window::kTitleBarHeightDip * scale;

        if (!ImGui::BeginViewportSideBar("##TitleBar", viewport, ImGuiDir_Up, barHeight,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::End();
            return;
        }

        ImGui::SetCursorPosY((barHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(mWindow->Title());

        // the unsaved-changes marker (*)
        if (mHistory.IsUnsaved())
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted("*");
            ImGui::SetItemTooltip("The scene has changes since it was last saved");
        }

        mTitleBarInteractiveRects.clear();
        const float buttonWidth = barHeight * 0.9f;
        const float buttonHeight = barHeight * 0.65f;
        const float buttonY = (barHeight - buttonHeight) * 0.5f;
        const float rowWidth = buttonWidth * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rowWidth);

        ImGui::SetWindowFontScale(0.65f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        const auto controlButton = [&](const char *icon)
            {
                ImGui::PushID(icon);
                ImGui::SetCursorPosY(buttonY);
                const bool clicked = ImGui::Button(icon, ImVec2(buttonWidth, buttonHeight));
                const ImVec2 rMin = ImGui::GetItemRectMin();
                const ImVec2 rMax = ImGui::GetItemRectMax();
                mTitleBarInteractiveRects.push_back(PixelRect{ static_cast<int32_t>(rMin.x), static_cast<int32_t>(rMin.y),
                                                              static_cast<uint32_t>(rMax.x - rMin.x),
                                                              static_cast<uint32_t>(rMax.y - rMin.y) });
                ImGui::PopID();
                ImGui::SameLine();
                return clicked;
            };

        if (controlButton(ICON_FA_WINDOW_MINIMIZE))
            mWindow->Minimize();
        if (controlButton(mWindow->IsMaximized() ? ICON_FA_WINDOW_RESTORE : ICON_FA_WINDOW_MAXIMIZE))
            mWindow->ToggleMaximize();
        if (controlButton(ICON_FA_XMARK))
            mWindow->RequestClose();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);

        mWindow->SetTitleBarInteractiveRects(mTitleBarInteractiveRects);

        ImGui::End();
    }

    SceneMenuAction Editor::DrawPlayControls(const EngineModeState &mode)
    {
        SceneMenuAction action = SceneMenuAction::None;

        const bool playing = mode.mMode == EngineMode::Play;
        const bool paused = playing && mode.mPlayState == PlayState::Paused;

        const char *primaryIcon = playing && !paused ? ICON_FA_PAUSE : ICON_FA_PLAY;
        const char *primaryText = playing ? (paused ? "Resume" : "Pause") : "Play";
        const ImVec4 primaryColor = playing && !paused ? EditorTheme::Color(StatusColor::Warning)
                                                       : EditorTheme::Color(ThemeColor::Success);
        const ImGuiStyle &style = ImGui::GetStyle();

        const float buttonWidth = std::max({ ImGui::CalcTextSize(ICON_FA_PLAY).x, ImGui::CalcTextSize(ICON_FA_PAUSE).x,
                                              ImGui::CalcTextSize(ICON_FA_STOP).x }) +
            style.ItemInnerSpacing.x * 1.5f +
            std::max({ ImGui::CalcTextSize("Play").x, ImGui::CalcTextSize("Resume").x,
                       ImGui::CalcTextSize("Pause").x, ImGui::CalcTextSize("Stop").x }) +
            style.FramePadding.x * 2.0f;

        float rowWidth = buttonWidth;
        if (playing)
            rowWidth += style.ItemSpacing.x + buttonWidth;

        ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - rowWidth) * 0.5f);

        const ImVec2 buttonSize(buttonWidth, 0.0f);
        if (widgets::IconTextButton("##play", primaryIcon, primaryText, primaryColor, buttonSize))
            action = playing ? SceneMenuAction::TogglePause : SceneMenuAction::Play;
        if (playing)
        {
            ImGui::SameLine();
            if (widgets::IconTextButton("##stop", ICON_FA_STOP, "Stop", EditorTheme::Color(StatusColor::Danger),
                buttonSize))
                action = SceneMenuAction::Stop;
        }

        return action;
    }

    void Editor::DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject)
    {
        if (!ImGui::BeginMenu("Open Project", !projects.empty()))
            return;

        for (const ProjectMenuEntry &project : projects)
        {
            // ID by directory: names may repeat
            ImGui::PushID(project.mDir.c_str());

            const bool isCurrent = project.mDir == currentProject;
            const bool available = project.mUnavailable.empty();
            if (ImGui::MenuItem(project.mName.c_str(), project.mDir.c_str(), isCurrent, available) && !isCurrent)
            {
                mPendingProject = project;
                mOpenConfirmRequested = true;
            }
            if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", project.mUnavailable.c_str());

            ImGui::PopID();
        }

        ImGui::EndMenu();
    }

    void Editor::DrawOpenProjectConfirm(EditorActions &actions, bool materialDraftsUnsaved)
    {
        constexpr const char *kPopupId = "Open Project##Confirm";

        // outside the menu: OpenPopup needs the same ID stack as BeginPopupModal
        if (mOpenConfirmRequested)
        {
            ImGui::OpenPopup(kPopupId);
            mOpenConfirmRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPopupId, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::Text("Open \"%s\"?", mPendingProject.mName.c_str());
        ImGui::TextDisabled("The editor restarts with this game.");
        ImGui::TextDisabled("Unsaved changes to the current scene will be lost.");
        if (materialDraftsUnsaved)
            ImGui::TextDisabled("Unsaved material drafts will be discarded, never saved automatically.");
        ImGui::Spacing();

        if (ImGui::Button("Open"))
        {
            actions.mOpenProject = mPendingProject.mDir;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    void Editor::DrawViewportGizmo(const EngineModeState &mode, World &world, EditorActions &actions,
        bool prefabDocument)
    {
        // matches EditorCamera's own Edit-only gating in App::Run
        if (mode.mMode != EngineMode::Edit)
            return;

        const bool modalOpen = ImGui::GetTopMostPopupModal() != nullptr;
        // a drag keeps the operation it began with
        if (!modalOpen && !mGizmoWasUsing && !ImGui::GetIO().WantTextInput &&
            !mWindow->RawInput().IsDown(MouseButton::Right))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false))
                mGizmoOperation = GizmoOperation::Translate;
            else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
                mGizmoOperation = GizmoOperation::Rotate;
            else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
                mGizmoOperation = GizmoOperation::Scale;
        }

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const std::vector<Entity> selected = GizmoSelection(world);
        // a drag keeps its pivot even when everything it moves is gone
        const bool hasGizmoTarget = mGizmoWasUsing || !selected.empty();

        // A Prefab tab keeps the toolbar (and its Save) visible with nothing selected.
        if (!hasGizmoTarget && !prefabDocument)
            return;

        const float x = static_cast<float>(mSceneViewportRect.offset.x);
        const float y = static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);

        ImGui::SetNextWindowPos(ImVec2(x + 8.0f, y + 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        const float dpi = mWindow->ContentScale();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * dpi, 4.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * dpi, 2.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * dpi);
        if (ImGui::Begin("##GizmoToolbar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing))
        {
            const auto modeButton = [&](const char *icon, const char *tooltip, GizmoOperation op)
                {
                    if (widgets::ToolbarButton(icon, tooltip, mGizmoOperation == op))
                        mGizmoOperation = op;
                    ImGui::SameLine();
                };

            modeButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Translate (W)", GizmoOperation::Translate);
            modeButton(ICON_FA_ROTATE, "Rotate (E)", GizmoOperation::Rotate);
            modeButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale (R)", GizmoOperation::Scale);

            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();

            const bool local = mGizmoSpace == GizmoSpace::Local;
            if (widgets::ToolbarButton(local ? ICON_FA_CUBE : ICON_FA_GLOBE,
                local ? "Local axes (click for world)" : "World axes (click for local)"))
                mGizmoSpace = local ? GizmoSpace::World : GizmoSpace::Local;

            if (prefabDocument)
            {
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();
                if (widgets::ToolbarButton(ICON_FA_FLOPPY_DISK, "Save Prefab (Ctrl+S)"))
                    actions.mSaveActiveDocument = true;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);

        if (!hasGizmoTarget)
            return;

        if (!mGizmoWasUsing)
        {
            const std::optional<WorldPose> pivot = SelectionPivot(world, selected);
            if (!pivot)
                return;
            mGizmoPivot = *pivot;
        }

        // ImGuizmo works in the scene viewport's camera-relative space: the
        // camera is the origin, so its float matrices stay small however far
        // the entity is from the Scene origin.
        const WorldPose camera = mCamera.Pose();
        const glm::mat4 view = glm::inverse(glm::mat4(camera.linear));

        const std::optional<glm::mat4> resolved = mGizmoPivot.TryMatrixRelativeTo(camera.position);
        if (!resolved)
            return;
        glm::mat4 gizmoMatrix = *resolved;

        // ImGuizmo takes the OpenGL clip convention, not the renderer's, and a
        // finite far plane, which the scene lens may not have. It only projects
        // and unprojects around the entity, so a plane a few times farther
        // than the entity stands in.
        const Camera &lens = mCamera.mLens;
        const float gizmoFar = lens.HasFarPlane()
            ? lens.mFarZ
            : std::max(4.0f * glm::length(glm::vec3(gizmoMatrix[3])), 1000.0f * lens.mNearZ);
        const glm::mat4 proj =
            glm::perspectiveRH_NO(glm::radians(lens.mFovYDegrees), width / height, lens.mNearZ, gizmoFar);

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::BeginFrame();
        ImGuizmo::SetDrawlist(ViewportOverlayDrawList());
        ImGuizmo::SetRect(x, y, width, height);
        ImGuizmo::Enable(!modalOpen && !mViewPieOpen);

        const ImGuizmo::MODE gizmoMode = mGizmoSpace == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
            ToImGuizmoOperation(mGizmoOperation), gizmoMode, glm::value_ptr(gizmoMatrix));


        if (ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ImGuizmo::Enable(false);


        const bool usingGizmo = ImGuizmo::IsUsing();
        if (usingGizmo && !mGizmoWasUsing)
        {
            mGizmoDragStart = mGizmoPivot;
            BeginGizmoDrag(world, selected);
        }

        // the release frame can still carry the last change
        if (changed && (usingGizmo || mGizmoWasUsing))
        {
            const std::optional<WorldOffset> fromCamera =
                WorldOffset::FromMeters(WorldVec3(glm::vec3(gizmoMatrix[3])));
            const std::optional<WorldPosition> position =
                fromCamera ? TryTranslateWorld(camera.position, *fromCamera) : std::nullopt;
            if (position)
            {
                mGizmoPivot = WorldPose{ *position, glm::mat3(gizmoMatrix) };
                ApplyGizmoDrag(world);
            }
        }

        if (mGizmoWasUsing && !usingGizmo)
        {
            mHistory.EndStep();
            mGizmoTargets.clear();
        }
        mGizmoWasUsing = usingGizmo;
    }

    void Editor::BeginGizmoDrag(World &world, std::span<const Entity> selected)
    {
        mGizmoTargets.clear();

        // a child of another selected entity already moves with it
        for (const Entity entity : TopmostInHierarchyOrder(world, selected))
        {
            // A root composes under the identity at the Scene origin.
            const Entity parent = ParentOf(world, entity);
            const WorldPose parentPose = parent.IsNull() ? WorldPose{} : ResolveWorld(world, parent);
            const std::optional<WorldOffset> fromPivot =
                TryWorldDifference(ResolveWorld(world, entity).position, mGizmoDragStart.position);
            const Transform *transform = world.GetComponent<Transform>(entity);

            // a zero-scale parent leaves no local values to write
            if (transform != nullptr && fromPivot && glm::determinant(parentPose.linear) != 0.0f)
                mGizmoTargets.push_back(GizmoTarget{ entity, parentPose, *fromPivot, *transform });
        }
    }

    void Editor::ApplyGizmoDrag(World &world)
    {
        const ComponentOps *ops = ComponentRegistry::Instance().FindBySeq(TypeIdOf<Transform>().seq);
        const std::optional<WorldOffset> moved = TryWorldDifference(mGizmoPivot.position, mGizmoDragStart.position);
        if (ops == nullptr || !moved)
            return;

        const bool translate = mGizmoOperation == GizmoOperation::Translate;
        const bool rotate = mGizmoOperation == GizmoOperation::Rotate;
        const char *verb = translate ? "Move" : rotate ? "Rotate" : "Scale";

        // The start axes are unit length, so this is the drag's rotation, or its stretch along them.
        const glm::mat3 change = mGizmoPivot.linear * glm::transpose(mGizmoDragStart.linear);
        const glm::mat3 about = rotate ? glm::mat3_cast(glm::normalize(glm::quat_cast(change))) : change;
        const WorldMat3 aboutPivot(about);

        std::vector<std::unique_ptr<ICommand>> commands;
        for (const GizmoTarget &target : mGizmoTargets)
        {
            if (!world.IsAlive(target.mEntity) || world.GetComponent<Transform>(target.mEntity) == nullptr)
                continue;

            // Positions move as offsets in world precision, so no far position
            // goes through the float matrix and back.
            std::vector<FieldValue> values;
            const WorldVec3 fromPivot = target.mFromPivot.Meters();
            if (translate || fromPivot != WorldVec3(0))
            {
                const std::optional<WorldOffset> shift =
                    translate ? moved : WorldOffset::FromMeters(aboutPivot * fromPivot - fromPivot);
                const std::optional<ParentLocalTranslation> localShift =
                    shift ? TryWorldOffsetToParentLocal(WorldMat3(target.mParent.linear), *shift) : std::nullopt;
                const std::optional<ParentLocalTranslation> position = localShift
                    ? ParentLocalTranslation::FromMeters(target.mStart.Position() + localShift->Meters())
                    : std::nullopt;
                if (!position)
                    continue;
                AddTransformField(values, *ops, "position", position->Meters());
            }

            if (!translate)
            {
                const glm::mat3 axes = glm::inverse(target.mParent.linear) * about * target.mParent.linear * glm::mat3_cast(target.mStart.Rotation());
                glm::quat turned{};
                glm::vec3 stretched{};
                DecomposeRotationScale(axes, turned, stretched);
                if (rotate)
                    AddTransformField(values, *ops, "rotation", glm::normalize(turned));
                else
                    AddTransformField(values, *ops, "scale", target.mStart.Scale() * stretched);
            }

            const Name *name = world.GetComponent<Name>(target.mEntity);
            commands.push_back(MakeSetFieldsEditCommand(target.mEntity, *ops, std::move(values),
                std::format("{} {}", verb, name != nullptr ? name->mValue : "Entity"), mAuthoredEntities));
        }

        if (commands.empty())
            return;

        if (mGizmoTargets.size() == 1)
            mHistory.Submit(std::move(commands.front()));
        else
            mHistory.Submit(MakeCommandGroup(std::format("{} {} Entities", verb, mGizmoTargets.size()),
                std::move(commands)));
    }

    void Editor::DrawViewportPrefabDrop(bool documentTab, World &world)
    {
        const ImGuiPayload *active = ImGui::GetDragDropPayload();
        if (active == nullptr || (!active->IsDataType(kPrefabContentPayload) &&
            !active->IsDataType(kParticleContentPayload)))
            return;
        const bool particle = active->IsDataType(kParticleContentPayload);
        if (particle && documentTab) return;
        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const ImVec2 min(static_cast<float>(mSceneViewportRect.offset.x),
            static_cast<float>(mSceneViewportRect.offset.y));
        const ImVec2 max(min.x + static_cast<float>(mSceneViewportRect.extent.width),
            min.y + static_cast<float>(mSceneViewportRect.extent.height));

        ImGui::SetNextWindowPos(min);
        ImGui::SetNextWindowSize(ImVec2(max.x - min.x, max.y - min.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##ViewportPrefabDrop", nullptr, flags))
        {
            if (ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID("##PrefabDrop")))
            {
                const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(particle ? kParticleContentPayload : kPrefabContentPayload,
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                if (payload != nullptr)
                {
                    if (!payload->IsDelivery())
                    {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    else
                    {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const float x = mouse.x - min.x;
                        const float y = mouse.y - min.y;
                        const float width = static_cast<float>(mSceneViewportRect.extent.width);
                        const float height = static_cast<float>(mSceneViewportRect.extent.height);
                        if (x >= 0.0f && y >= 0.0f && x < width && y < height)
                        {
                            const glm::vec2 ndc{ (x / width) * 2.0f - 1.0f, (y / height) * 2.0f - 1.0f };
                            const WorldRay ray = MakeCameraRay(mCamera.Pose(), mCamera.mLens, width / height, ndc);

                            WorldReal distance = kPrefabDropDistance;
                            if (std::abs(ray.direction.y) > kPrefabDropMinRate)
                            {
                                const WorldReal crossing = -ray.origin.Meters().y / ray.direction.y;
                                if (crossing > 0.0f && crossing <= kPrefabDropPlaneLimit)
                                    distance = crossing;
                            }

                            const std::optional<WorldOffset> offset =
                                WorldOffset::FromMeters(WorldVec3(ray.direction) * distance);
                            const std::optional<WorldPosition> point =
                                offset ? TryTranslateWorld(ray.origin, *offset) : std::nullopt;
                            if (point)
                            {
                                const std::string contentPath(static_cast<const char *>(payload->Data),
                                    payload->DataSize);
                                if (!documentTab)
                                {
                                    mHistory.Submit(particle ?
                                        MakePlaceParticleSystemCommand(contentPath, kNullEntity, point->Meters()) :
                                        MakePlacePrefabCommand(contentPath, kNullEntity, point->Meters()));
                                }
                                else if (!particle && mAuthoredEntities != nullptr)
                                {
                                    const std::span<const Entity> roots = world.HierarchyIndex().Roots();
                                    const Entity root = roots.empty() ? kNullEntity : roots.front();
                                    const std::optional<ParentLocalTranslation> local = root.IsNull()
                                        ? std::nullopt
                                        : TryWorldToParentLocal(ResolveWorld(world, root), *point);
                                    if (local)
                                    {
                                        EntityAction action;
                                        action.mKind = EntityActionKind::PlacePrefab;
                                        action.mPrefabPath = contentPath;
                                        action.mPlacement = local->Meters();
                                        mHistory.Submit(
                                            MakeDocumentEntityCommand(action, *mAuthoredEntities, world));
                                    }
                                }
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    void Editor::FocusSelection(const EngineModeState &mode, World &world)
    {
        if (mode.mMode != EngineMode::Edit)
            return;

        // F would also land in a focused text field.
        if (ImGui::GetIO().WantTextInput || !ImGui::IsKeyPressed(ImGuiKey_F, false))
            return;

        const Selection *selection = world.TryResource<Selection>();
        const Entity entity = selection != nullptr ? selection->Primary() : kNullEntity;
        if (entity.IsNull() || !world.IsAlive(entity) || world.GetComponent<Transform>(entity) == nullptr)
            return;

        const WorldPose pose = ResolveWorld(world, entity);
        const MeshBounds *bounds = world.GetComponent<MeshBounds>(entity);
        mCamera.FocusOn(pose.position, bounds != nullptr ? BoundsRadius(*bounds, pose.linear) : 0.0f);
    }

    void Editor::UpdateViewportPicking(const EngineModeState &mode, World &world)
    {
        const bool down = mWindow->RawInput().IsDown(MouseButton::Left);
        const bool justPressed = down && !mMouseWasDown;
        mMouseWasDown = down;

        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        if (WantsCaptureMouse())
            return;

        if (ImGuizmo::IsOver() || ImGuizmo::IsUsing())
            return;

        // a click that picks a pie wedge is not a pick in the scene
        if (mViewPieOpen)
            return;

        if (!justPressed)
            return;

        const RawInputSnapshot &input = mWindow->RawInput();
        const float px = static_cast<float>(input.mouseX) - static_cast<float>(mSceneViewportRect.offset.x);
        const float py = static_cast<float>(input.mouseY) - static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);
        if (px < 0.0f || py < 0.0f || px >= width || py >= height)
            return;

        const glm::vec2 ndc{ (px / width) * 2.0f - 1.0f, (py / height) * 2.0f - 1.0f };
        // The ray belongs to the scene viewport's own camera, and each box is
        // placed relative to that origin before anything becomes float.
        const WorldRay ray = MakeCameraRay(mCamera.Pose(), mCamera.mLens, width / height, ndc);

        Entity hitEntity = kNullEntity;
        float hitDistance = std::numeric_limits<float>::max();

        world.GetOrCreateQuery<const WorldTransform, const MeshRenderer, const MeshBounds>().ForEach(
            [&](Entity entity, const WorldTransform &transform, const MeshRenderer &, const MeshBounds &bounds)
            {
                const std::optional<float> distance =
                    RayIntersectsBox(ray, transform.Pose(), bounds.mLocalMin, bounds.mLocalMax);
                if (distance && *distance < hitDistance)
                {
                    hitDistance = *distance;
                    hitEntity = entity;
                }
            });

        // Any component whose owning module registered a pick radius (e.g. Light): a world-space
        // sphere around the entity's transform origin, ranked where the ray enters it.
        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (ops->mPickRadius == nullptr)
                continue;

            world.ForEachAliveEntity([&](Entity entity)
                {
                    if (!ops->Has(world, entity))
                        return;
                    const WorldTransform *transform = world.GetComponent<WorldTransform>(entity);
                    if (transform == nullptr)
                        return;

                    const void *component = ops->GetComponent(world, entity);
                    const std::optional<float> distance =
                        RayIntersectsSphere(ray, transform->Position(), ops->mPickRadius(component));
                    if (distance && *distance < hitDistance)
                    {
                        hitDistance = *distance;
                        hitEntity = entity;
                    }
                });
        }

        ApplyClick(world.Resource<Selection>(), hitEntity, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
    }

    void Editor::DrawSelectionHighlights(const EngineModeState &mode, World &world)
    {
        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const Selection *selection = world.TryResource<Selection>();
        if (selection == nullptr || selection->mSelected.empty())
            return;

        const float x0 = static_cast<float>(mSceneViewportRect.offset.x);
        const float y0 = static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);

        const WorldPose camera = mCamera.Pose();
        const glm::mat4 viewProj = MakeCameraRelativeViewProjection(camera, mCamera.mLens, width / height);

        ImDrawList *drawList = ViewportOverlayDrawList();
        drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
        const Entity primary = selection->Primary();

        for (const Entity entity : selection->mSelected)
        {
            if (!world.IsAlive(entity))
                continue;

            const MeshBounds *bounds = world.GetComponent<MeshBounds>(entity);
            if (bounds == nullptr)
                continue;

            const std::optional<glm::mat4> resolved = ResolveWorld(world, entity).TryMatrixRelativeTo(camera.position);
            if (!resolved)
                continue;
            const glm::mat4 worldMatrix = *resolved;
            const glm::vec3 mn = bounds->mLocalMin;
            const glm::vec3 mx = bounds->mLocalMax;
            const glm::vec3 corners[8] = {
                { mn.x, mn.y, mn.z },{ mx.x, mn.y, mn.z },{ mx.x, mx.y, mn.z },{ mn.x, mx.y, mn.z },
                { mn.x, mn.y, mx.z },{ mx.x, mn.y, mx.z },{ mx.x, mx.y, mx.z },{ mn.x, mx.y, mx.z } };

            ImVec2 screen[8];
            bool behindCamera = false;
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 worldCorner = glm::vec3(worldMatrix * glm::vec4(corners[i], 1.0f));
                if (!ProjectToScreen(viewProj, x0, y0, width, height, worldCorner, screen[i]))
                {
                    behindCamera = true;
                    break;
                }
            }
            if (behindCamera)
                continue;

            const ImU32 color = (entity == primary) ? IM_COL32(255, 170, 0, 255) : IM_COL32(255, 220, 130, 190);

            static constexpr int kEdges[12][2] = {
                { 0, 1 },{ 1, 2 },{ 2, 3 },{ 3, 0 },{ 4, 5 },{ 5, 6 },
                { 6, 7 },{ 7, 4 },{ 0, 4 },{ 1, 5 },{ 2, 6 },{ 3, 7 } };
            for (const auto &edge : kEdges)
                drawList->AddLine(screen[edge[0]], screen[edge[1]], color, 2.0f);
        }

        drawList->PopClipRect();
    }

    void Editor::DrawComponentGizmos(const EngineModeState &mode, World &world)
    {
        if (mode.mMode != EngineMode::Edit)
            return;

        if (mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
            return;

        const Selection *selection = world.TryResource<Selection>();

        const float x0 = static_cast<float>(mSceneViewportRect.offset.x);
        const float y0 = static_cast<float>(mSceneViewportRect.offset.y);
        const float width = static_cast<float>(mSceneViewportRect.extent.width);
        const float height = static_cast<float>(mSceneViewportRect.extent.height);

        const WorldPose camera = mCamera.Pose();
        const glm::mat4 viewProj = MakeCameraRelativeViewProjection(camera, mCamera.mLens, width / height);

        ImDrawList *drawList = ViewportOverlayDrawList();
        drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);

        const ScreenGizmoCanvas canvas(drawList, viewProj, x0, y0, width, height);

        // Any component whose owning module registered a gizmo drawer (e.g. Light).
        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (ops->mDrawGizmo == nullptr)
                continue;

            world.ForEachAliveEntity([&](Entity entity)
                {
                    if (!ops->Has(world, entity))
                        return;
                    const WorldTransform *transform = world.GetComponent<WorldTransform>(entity);
                    if (transform == nullptr)
                        return;

                    const std::optional<glm::mat4> resolved = transform->Pose().TryMatrixRelativeTo(camera.position);
                    if (!resolved)
                        return;

                    const void *component = ops->GetComponent(world, entity);
                    const bool selected = selection != nullptr && selection->Contains(entity);
                    ops->mDrawGizmo(component, *resolved, selected, canvas);
                });
        }

        drawList->PopClipRect();
    }

    void Editor::DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects,
        bool materialDraftsUnsaved)
    {
        constexpr const char *kPopupId = "New Project";

        // outside the menu: OpenPopup needs the same ID stack as BeginPopupModal
        if (mNewProjectRequested)
        {
            mNewProjectName[0] = '\0';
            mNewProjectRayTracing = false;
            ImGui::OpenPopup(kPopupId);
            mNewProjectRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPopupId, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::InputTextWithHint("Name", "Project name...", mNewProjectName, sizeof(mNewProjectName));
        ImGui::Checkbox("Ray Tracing", &mNewProjectRayTracing);
        if (materialDraftsUnsaved)
            ImGui::TextDisabled("Unsaved material drafts will be discarded, never saved automatically.");
        ImGui::Spacing();

        const std::string_view name(mNewProjectName);
        const bool collides = std::any_of(projects.begin(), projects.end(), [&](const ProjectMenuEntry &project)
            {
                return project.mDir == "games/" + std::string(name);
            });
        const bool canCreate = !name.empty() && !collides;

        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create"))
        {
            actions.mCreateProjectName = mNewProjectName;
            actions.mCreateProjectRayTracing = mNewProjectRayTracing;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();

        if (collides)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "A project named \"%s\" already exists.",
                mNewProjectName);
        }

        ImGui::EndPopup();
    }

    EditorActions Editor::DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
        PostProcessingSettings &postProcessingSettings, const EngineModeState &mode,
        std::span<const ProjectMenuEntry> projects, std::string_view currentProject, World &activeWorld,
        World &sceneWorld, const EditorDocuments &documents, const AssetImportView &assetImport,
        const VulkanRenderer &renderer, const ContentBrowserView &contentBrowser, bool materialPanelVisible,
        bool materialDraftsUnsaved, float dt)
    {
        EditorActions actions;

        const bool documentTab = documents.mActive != kSceneDocument;
        GroundQuery *ground = activeWorld.TryResource<GroundQuery>();
        mSnapFrameSubmissions = mHistory.SubmissionCount();
        PollSnapToTerrain(activeWorld, documents.mActive, mode, dt);
        SnapToTerrainAction snap = SnapToTerrainActionFor(activeWorld, documents.mActive, ground);
        snap.mWaiting = mSnapToTerrain.Active();
        if (snap.mWaiting)
            snap.mStatus = mSnapToTerrain.Status();
        bool snapRequested = false;

        if (mThemePending)
        {
            ApplyTheme();
            mThemePending = false;
        }

        if (enableLayout)
        {
            mBackground.CollectRetired();

            // Panels go translucent so the image shows; the title and menu bars keep the theme.
            const bool showBackground = mPreferences.backgroundEnabled && mBackground.HasImage();
            const auto pushPanelAlpha = [&]
            {
                if (!showBackground)
                    return 0;
                for (const ImGuiCol col : {ImGuiCol_WindowBg, ImGuiCol_ChildBg})
                {
                    ImVec4 color = ImGui::GetStyleColorVec4(col);
                    switch (mPreferences.backgroundPanels)
                    {
                    case BackgroundPanels::Theme:
                        color.w *= 1.0f - mPreferences.backgroundStrength;
                        break;
                    case BackgroundPanels::Black:
                        color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f - mPreferences.backgroundStrength);
                        break;
                    case BackgroundPanels::Transparent:
                        color.w = 0.0f;
                        break;
                    }
                    ImGui::PushStyleColor(col, color);
                }
                return 2;
            };

            if (mWindow->HasCustomTitleBar())
                DrawTitleBar();

            int pushedColors = pushPanelAlpha();

            const ImGuiID dockspaceId =
                ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

            ImGuiDockNode *dockspaceNode = ImGui::DockBuilderGetNode(dockspaceId);
            if (dockspaceNode != nullptr && dockspaceNode->IsEmpty())
            {
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID center = dockspaceId;
                const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
                const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
                const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

                ImGui::DockBuilderDockWindow("Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", right);
                ImGui::DockBuilderDockWindow("Output", bottom);

                ImGui::DockBuilderFinish(dockspaceId);
            }

            SwitchDocumentCamera(documents, activeWorld);

            if (ImGui::Begin("Hierarchy"))
            {
                const EntityAction entityAction = mHierarchyPanel.Draw(activeWorld, mAuthoredEntities, documentTab);
                if (!documentTab && entityAction.mKind == EntityActionKind::CreatePrefab)
                {
                    if (mAuthoredEntities != nullptr)
                        mHistory.Submit(MakeCreatePrefabCommand(*mAuthoredEntities, entityAction.mTarget));
                }
                else if (documentTab && mAuthoredEntities != nullptr)
                {
                    mHistory.Submit(MakeDocumentEntityCommand(entityAction, *mAuthoredEntities, activeWorld));
                }
                else if (entityAction.mKind == EntityActionKind::PlaceParticleSystem)
                {
                    if (mode.mMode == EngineMode::Edit)
                        mHistory.Submit(MakePlaceParticleSystemCommand(entityAction.mPrefabPath,
                            kNullEntity, WorldVec3(0)));
                }
                else if (entityAction.mKind == EntityActionKind::AttachParticleSystem &&
                    mode.mMode == EngineMode::Edit &&
                    entityAction.mPrefabPath.starts_with("assets/") &&
                    entityAction.mPrefabPath.ends_with(".particle.json"))
                {
                    const std::string path = entityAction.mPrefabPath.substr(7);
                    if (path.size() < kFieldStringCapacity)
                    {
                        const ComponentOps *ops = ComponentRegistry::Instance().FindBySeq(TypeIdOf<ParticleSystem>().seq);
                        if (ops != nullptr)
                        {
                            FieldValue value;
                            std::memcpy(value.mBytes, path.c_str(), path.size() + 1);
                            if (ops->Has(activeWorld, entityAction.mTarget))
                                mHistory.Submit(MakeSetFieldsCommand(entityAction.mTarget, *ops,
                                    { value }, "Assign Particle System"));
                            else
                                mHistory.Submit(MakeAddComponentCommand(entityAction.mTarget, *ops,
                                    { value }));
                        }
                    }
                }
                else if (documentTab || entityAction.mKind != EntityActionKind::PlacePrefab ||
                    mode.mMode == EngineMode::Edit)
                {
                    mHistory.Submit(MakeEntityCommand(entityAction));
                }
            }
            ImGui::End();

            if (ImGui::Begin("Inspector"))
            {
                snapRequested |= mInspectorPanel.Draw(activeWorld, assetImport, mHistory, mAuthoredEntities, snap);
                if (!documentTab && mode.mMode == EngineMode::Edit)
                {
                    const auto &selection = activeWorld.Resource<Selection>().mSelected;
                    if (selection.size() == 1)
                        if (const auto *particle = activeWorld.GetComponent<ParticleSystem>(selection.front());
                            particle != nullptr && particle->systemName[0] != '\0' &&
                            ImGui::Button("Open Particle Editor"))
                            mParticleOpenRequest = particle->systemName;
                }
            }
            ImGui::End();

            if (ImGui::Begin("Output"))
                mLogPanel.Draw();
            ImGui::End();

            if (const ImGuiDockNode *centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId);
                centralNode != nullptr)
            {
                const bool revealActive = !mShownDocumentKnown || mShownDocument != documents.mActive;
                const float barHeight = DrawDocumentTabs(centralNode->Pos.x, centralNode->Pos.y, centralNode->Size.x,
                    documents, actions, revealActive);
                mShownDocument = documents.mActive;
                mShownDocumentKnown = true;
                const float viewportHeight = std::max(centralNode->Size.y - barHeight, 0.0f);
                mSceneViewportRect = VkRect2D{
                    .offset{ static_cast<int32_t>(centralNode->Pos.x),
                            static_cast<int32_t>(centralNode->Pos.y + barHeight) },
                    .extent{ static_cast<uint32_t>(std::max(centralNode->Size.x, 0.0f)),
                            static_cast<uint32_t>(viewportHeight) } };
            }

            if (mode.mMode == EngineMode::Edit)
                DrawViewportPrefabDrop(documentTab, activeWorld);

            // the dock area below the bars, minus the scene viewport
            if (const ImGuiDockNode *rootNode = ImGui::DockBuilderGetNode(dockspaceId);
                showBackground && rootNode != nullptr)
            {
                const glm::vec2 sceneMin(mSceneViewportRect.offset.x, mSceneViewportRect.offset.y);
                const glm::vec2 sceneSize(mSceneViewportRect.extent.width, mSceneViewportRect.extent.height);
                const glm::vec2 areaMin = rootNode->Pos;
                mBackground.Draw(mPreferences.backgroundFit, areaMin, areaMin + glm::vec2(rootNode->Size), sceneMin,
                    sceneMin + sceneSize);
            }

            UpdateViewportPicking(mode, activeWorld);
            FocusSelection(mode, activeWorld);
            UpdateHistoryShortcuts(actions, mode);
            DrawSelectionHighlights(mode, activeWorld);
            DrawComponentGizmos(mode, activeWorld);
            DrawViewportGizmo(mode, activeWorld, actions, documentTab);
            UpdateViewPie(mode);

            ImGui::PopStyleColor(pushedColors);
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    ImGui::BeginDisabled(mode.mMode == EngineMode::Play);

                    if (ImGui::MenuItem("New Scene"))
                        actions.mScene = SceneMenuAction::New;
                    if (ImGui::MenuItem("Save Scene"))
                        actions.mScene = SceneMenuAction::Save;
                    if (ImGui::MenuItem("Load Scene"))
                        actions.mScene = SceneMenuAction::Load;

                    ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem("New Project..."))
                        mNewProjectRequested = true;
                    DrawOpenProjectMenu(projects, currentProject);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Build Settings..."))
                        mShowBuildSettings = mFocusBuildSettings = true;
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit"))
                {
                    DrawEditMenu();
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Project"))
                {
                    ImGui::MenuItem("Project Settings", nullptr, &mShowProjectSettings);
                    ImGui::MenuItem("Input Settings", nullptr, &mShowInputSettings);
                    ImGui::MenuItem("Post-Processing", nullptr, &mShowPostProcessing);
                    ImGui::MenuItem("Asset Import", nullptr, &mShowAssetImport);
                    ImGui::MenuItem("Particles", nullptr, &mShowParticles);
                    ImGui::MenuItem("Content Browser", nullptr, &mShowContentBrowser);
                    if (ImGui::MenuItem("Material", nullptr, materialPanelVisible))
                        actions.mToggleMaterialEditor = true;
                    ImGui::Separator();
                    ImGui::MenuItem("Preferences", nullptr, &mShowPreferences);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Debug"))
                {
                    DrawDebugViewMenu(mode);
                    ImGui::Separator();
                    ImGui::MenuItem("Style Editor", nullptr, &mShowStyleEditor);
                    ImGui::MenuItem("Profiler", nullptr, &mShowProfiler);
                    ImGui::EndMenu();
                }
                if (const SceneMenuAction playAction = DrawPlayControls(mode); playAction != SceneMenuAction::None)
                    actions.mScene = playAction;

                ImGui::EndMainMenuBar();
            }
            pushedColors = pushPanelAlpha();

            DrawOpenProjectConfirm(actions, materialDraftsUnsaved);
            DrawNewProjectModal(actions, projects, materialDraftsUnsaved);

            if (mShowStyleEditor)
            {
                if (ImGui::Begin("Style Editor", &mShowStyleEditor))
                    ImGui::ShowStyleEditor();
                ImGui::End();
            }

            if (mShowProfiler)
            {
                if (ImGui::Begin("Profiler", &mShowProfiler))
                {
                    const VulkanRenderer::GpuMemoryUsage gpuMemory = renderer.GetGpuMemoryUsage();
                    mProfilerPanel.Draw(gpuMemory.mUsageBytes, gpuMemory.mBudgetBytes);
                }
                ImGui::End();
            }

            if (mShowProjectSettings)
            {
                if (ImGui::Begin("Project Settings", &mShowProjectSettings))
                    mProjectSettingsPanel.Draw(renderSettings, mProjectSettingsPath, mShowInputSettings,
                        sceneWorld.TryResource<PhysicsLayerUpdate>(), mAudio, mode.mMode == EngineMode::Edit);
                ImGui::End();
            }
            else
            {
                mProjectSettingsPanel.OnHidden();
            }

            if (mShowPreferences)
            {
                if (ImGui::Begin("Preferences", &mShowPreferences))
                    ApplyPreferencesRequest(mPreferencesPanel.Draw(mPreferences, mBackground.HasImage()));
                ImGui::End();
            }

            if (mShowBuildSettings)
            {
                if (std::exchange(mFocusBuildSettings, false))
                    ImGui::SetNextWindowFocus();
                if (ImGui::Begin("Build Settings", &mShowBuildSettings) &&
                    mBuildSettingsPanel.Draw(mPreferences, mBuildGame) && !mPreferences.SaveFile(mPreferencesPath))
                    mBuildSettingsPanel.SetStatus("Save failed - see log.");
                ImGui::End();
            }
            else
            {
                mBuildSettingsPanel.OnHidden();
            }

            if (mShowInputSettings)
            {
                if (ImGui::Begin("Input Settings", &mShowInputSettings))
                    mInputSettingsPanel.Draw(inputMap, mWindow->RawInput(), mProjectSettingsPath);
                else
                    mInputSettingsPanel.CancelListening();
                ImGui::End();
            }
            else
            {
                mInputSettingsPanel.OnHidden();
            }

            if (mShowPostProcessing)
            {
                if (ImGui::Begin("Post-Processing", &mShowPostProcessing))
                    mPostProcessingPanel.Draw(postProcessingSettings, mProjectSettingsPath,
                        PostEffectContext{ renderer.RayTracingActive(renderSettings.rayTracing),
                            renderer.DlssAvailable() });
                ImGui::End();
            }
            else
            {
                mPostProcessingPanel.OnHidden();
            }

            const std::span<const std::string> dropped =
                mWindow != nullptr ? mWindow->DroppedFiles() : std::span<const std::string>{};
            if (!dropped.empty())
            {
                // A drop is a one-frame event, so the panel has to be open,
                // uncollapsed and drawn this frame or the files are lost.
                mShowAssetImport = true;
                ImGui::SetNextWindowCollapsed(false);
                // SetNextWindowFocus also selects the tab when the panel is
                // docked behind another one.
                ImGui::SetNextWindowFocus();
            }

            if (mShowAssetImport)
            {
                if (ImGui::Begin("Asset Import", &mShowAssetImport))
                    actions.mAssetImport = mAssetImportPanel.Draw(assetImport, dropped);
                ImGui::End();
            }

            if (mShowContentBrowser)
            {
                if (ImGui::Begin("Content Browser", &mShowContentBrowser))
                    actions.mContentBrowser = mContentBrowserPanel.Draw(contentBrowser);
                ImGui::End();
            }

            ImGui::PopStyleColor(pushedColors);
        }
        else
        {
            mSceneViewportRect = VkRect2D{};
        }

        const bool snapTransitionRequested =
            actions.mScene == SceneMenuAction::New || actions.mScene == SceneMenuAction::Load ||
            actions.mScene == SceneMenuAction::Play || actions.mScene == SceneMenuAction::Stop ||
            !actions.mOpenProject.empty() || !actions.mCreateProjectName.empty() ||
            actions.mDocument.mKind != EditorDocumentActionKind::None;
        if (snapRequested)
            StartSnapToTerrain(activeWorld, documents.mActive);
        FinishSnapToTerrain(activeWorld, documents.mActive, ground, mode.mMode == EngineMode::Edit,
                            snapTransitionRequested);

        if (mSceneOps != nullptr)
            mHistory.Flush(sceneWorld, *mSceneOps);

        return actions;
    }

    void Editor::DrawEditMenu()
    {
        if (mParticleHistoryFocused)
        {
            if (ImGui::MenuItem("Undo Particle Edit", "Ctrl+Z", false, mParticleCanUndo))
                mParticleHistoryRequest = -1;
            if (ImGui::MenuItem("Redo Particle Edit", "Ctrl+Y", false, mParticleCanRedo))
                mParticleHistoryRequest = 1;
            return;
        }

        //keeps the item's id while the step name in its label changes
        const std::string undo = mHistory.CanUndo() ? "Undo " + mHistory.UndoName() : std::string("Undo");

        if (ImGui::MenuItem((undo + "###Undo").c_str(), "Ctrl+Z", false, mHistory.CanUndo()))
            mHistory.RequestUndo();

        const std::string redo = mHistory.CanRedo() ? "Redo " + mHistory.RedoName() : std::string("Redo");

        if (ImGui::MenuItem((redo + "###Redo").c_str(), "Ctrl+Y", false, mHistory.CanRedo()))
            mHistory.RequestRedo();
    }

    void Editor::PollSnapToTerrain(World &world, DocumentId document, const EngineModeState &mode, float dt)
    {
        if (!mSnapToTerrain.Active())
            return;
        GroundQuery *ground = world.TryResource<GroundQuery>();
        if (ground == nullptr)
        {
            mSnapToTerrain.Cancel("The scene has no terrain.");
            return;
        }
        mSnapToTerrain.Update(world, document, *ground, dt, mode.mMode == EngineMode::Edit);
    }

    void Editor::StartSnapToTerrain(World &world, DocumentId document)
    {
        GroundQuery *ground = world.TryResource<GroundQuery>();
        if (ground == nullptr)
        {
            MIR_LOG_WARN("Snap to Terrain: the scene has no terrain.");
            return;
        }
        const SnapToTerrainAction action = mSnapToTerrain.Begin(world, document, *ground);
        if (!action.mEnabled)
            MIR_LOG_WARN("Snap to Terrain: {}", action.mReason);
    }

    void Editor::FinishSnapToTerrain(World &world, DocumentId document, const GroundQuery *ground, bool editing,
                                     bool transitionRequested)
    {
        if (transitionRequested && mSnapToTerrain.Active())
            mSnapToTerrain.Cancel("The scene changed before the snap committed.");

        const std::string failure = mSnapToTerrain.TakeFailure();
        if (!failure.empty())
            MIR_LOG_WARN("Snap to Terrain: {}", failure);

        if (mSnapToTerrain.CurrentState() != SnapToTerrainSession::State::Ready)
            return;
        if (ground == nullptr)
        {
            mSnapToTerrain.Cancel("The scene has no terrain.");
            MIR_LOG_WARN("Snap to Terrain: {}", mSnapToTerrain.TakeFailure());
            return;
        }

        const SnapToTerrainCommit commit{mSnapFrameSubmissions, mHistory.SubmissionCount(), transitionRequested};
        const std::optional<SnapToTerrainEdit> edit =
            mSnapToTerrain.TakeEdit(world, document, *ground, editing, commit);
        if (!edit)
        {
            const std::string refused = mSnapToTerrain.TakeFailure();
            if (!refused.empty())
                MIR_LOG_WARN("Snap to Terrain: {}", refused);
            return;
        }

        const Entity entity = mSnapToTerrain.Target();
        std::unique_ptr<ICommand> command = MakeSnapToTerrainCommand(entity, *edit, mAuthoredEntities);
        if (!command)
        {
            MIR_LOG_ERROR("Snap to Terrain: the Transform edit could not be built.");
            return;
        }
        mHistory.Submit(std::move(command), true);
        MIR_LOG_INFO("Snap to Terrain: {} samples, {:.3f} m residual, source spacing {:.3f} m", edit->mSamples,
                     edit->mResidual, edit->mSourceSpacing);
    }

    void Editor::DrawDebugViewMenu(const EngineModeState &mode)
    {
        // shown in Play, but only Edit changes it, as with the pie
        ImGui::BeginDisabled(mode.mMode != EngineMode::Edit);
        if (ImGui::BeginMenu("Shading"))
        {
            for (const ViewPieItem item : kViewPieItems)
            {
                if (item == ViewPieItem::Colliders)
                    continue;
                if (ImGui::MenuItem(ViewPieLabel(item), "Z", ViewPieItemActive(mDebugView, item)))
                    ApplyViewPieItem(mDebugView, item);
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem(ViewPieLabel(ViewPieItem::Colliders), "Z", &mDebugView.colliders);
        ImGui::EndDisabled();
    }

    void Editor::UpdateViewPie(const EngineModeState &mode)
    {
        // view changes are Edit-only; Play keeps rendering what Edit picked
        if (mode.mMode != EngineMode::Edit || ImGui::GetTopMostPopupModal() != nullptr ||
            mSceneViewportRect.extent.width == 0 || mSceneViewportRect.extent.height == 0)
        {
            mViewPieOpen = false;
            return;
        }

        const ImGuiIO &io = ImGui::GetIO();
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        if (!mViewPieOpen)
        {
            // Ctrl+Z is undo, and a held right button is flying the camera
            if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper || WantsCaptureMouse() ||
                mWindow->RawInput().IsDown(MouseButton::Right) || !ImGui::IsKeyPressed(ImGuiKey_Z, false))
                return;

            const glm::vec2 min(mSceneViewportRect.offset.x, mSceneViewportRect.offset.y);
            const glm::vec2 max = min + glm::vec2(mSceneViewportRect.extent.width, mSceneViewportRect.extent.height);
            if (mouse.x < min.x || mouse.y < min.y || mouse.x >= max.x || mouse.y >= max.y)
                return;

            mViewPieOpen = true;
            mViewPieSticky = false;
            mViewPieCenter = mouse;
        }

        const std::optional<ViewPieItem> hovered = ViewPieItemAt(mouse - mViewPieCenter, kViewPieDeadZone);
        bool choose = false;
        bool close = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (!close && !mViewPieSticky && !ImGui::IsKeyDown(ImGuiKey_Z))
        {
            // released over a wedge picks it; released in the dead zone keeps the pie for a click
            choose = hovered.has_value();
            close = choose;
            mViewPieSticky = !choose;
        }
        else if (!close && mViewPieSticky && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            choose = hovered.has_value();
            close = true;
        }

        if (choose)
            ApplyViewPieItem(mDebugView, *hovered);
        if (close)
        {
            mViewPieOpen = false;
            return;
        }
        DrawViewPie(hovered);
    }

    void Editor::DrawViewPie(std::optional<ViewPieItem> hovered)
    {
        // a window over the viewport, so while the pie is up it takes the mouse from picking and the camera
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(mSceneViewportRect.offset.x),
            static_cast<float>(mSceneViewportRect.offset.y)));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(mSceneViewportRect.extent.width),
            static_cast<float>(mSceneViewportRect.extent.height)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##ViewPie", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing);

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImGuiStyle &style = ImGui::GetStyle();
        const ImVec2 center(mViewPieCenter.x, mViewPieCenter.y);

        drawList->AddCircle(center, kViewPieDeadZone, ImGui::GetColorU32(ImGuiCol_Border), 0, 2.0f);
        if (hovered)
            drawList->AddLine(center, ImGui::GetMousePos(), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 2.0f);

        for (const ViewPieItem item : kViewPieItems)
        {
            const bool active = ViewPieItemActive(mDebugView, item);
            const std::string label = active ? std::string(ICON_FA_CHECK "  ") + ViewPieLabel(item)
                                             : std::string(ViewPieLabel(item));
            const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            const ImVec2 size(textSize.x + style.FramePadding.x * 2.0f, textSize.y + style.FramePadding.y * 2.0f);

            // each box sits past the radius on its own side, centered on its axis
            const glm::vec2 dir = ViewPieDirection(item);
            const ImVec2 min(center.x + dir.x * kViewPieRadius + (dir.x - 1.0f) * size.x * 0.5f,
                center.y + dir.y * kViewPieRadius + (dir.y - 1.0f) * size.y * 0.5f);
            const ImVec2 max(min.x + size.x, min.y + size.y);

            const ImGuiCol fill = item == hovered ? ImGuiCol_ButtonHovered : active ? ImGuiCol_Button : ImGuiCol_PopupBg;
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(fill), style.FrameRounding);
            drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
            drawList->AddText(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void Editor::UpdateHistoryShortcuts(EditorActions &actions, const EngineModeState &mode)
    {
        const ImGuiWindow *particleWindow = ImGui::FindWindowByName("Particle Editor");
        const ImGuiWindow *focusedWindow = ImGui::GetCurrentContext()->NavWindow;
        mParticleHistoryFocused = particleWindow != nullptr && focusedWindow != nullptr &&
            focusedWindow->RootWindow == particleWindow->RootWindow;
        if (mParticleHistoryFocused)
            return;
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantTextInput || !io.KeyCtrl)
            return;

        if (ImGui::IsKeyPressed(ImGuiKey_Z))
            mHistory.RequestUndo();
        else if (ImGui::IsKeyPressed(ImGuiKey_Y))
            mHistory.RequestRedo();
        else if (mode.mMode == EngineMode::Edit && ImGui::IsKeyPressed(ImGuiKey_S))
            actions.mSaveActiveDocument = true;
    }

    void Editor::SwitchDocumentCamera(const EditorDocuments &documents, World &world)
    {
        if (documents.mActive == mCameraDocument)
            return;

        mDocumentCameras[mCameraDocument] = mCamera;
        const auto cached = mDocumentCameras.find(documents.mActive);
        if (cached != mDocumentCameras.end())
        {
            mCamera = cached->second;
        }
        else
        {
            mCamera = EditorCamera{};
            if (documents.mActive != kSceneDocument)
                FocusDocumentRoot(world);
            mDocumentCameras.emplace(documents.mActive, mCamera);
        }
        mCamera.Cut();
        mCameraDocument = documents.mActive;
    }

    void Editor::FocusDocumentRoot(World &world)
    {
        const HierarchyIndex &index = world.HierarchyIndex();
        if (index.Roots().empty())
            return;

        const Entity root = index.Roots().front();
        if (!world.IsAlive(root) || world.GetComponent<Transform>(root) == nullptr)
            return;

        const WorldPose pose = ResolveWorld(world, root);
        const MeshBounds *bounds = world.GetComponent<MeshBounds>(root);
        mCamera.FocusOn(pose.position, bounds != nullptr ? BoundsRadius(*bounds, pose.linear) : 0.0f);
    }

    bool Editor::WantsCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
    bool Editor::WantsCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }

    void Editor::UpdateCamera(float dt)
    {
        const bool lookHeld = !WantsCaptureMouse() && mWindow->RawInput().IsDown(MouseButton::Right);
        mCamera.Update(mWindow->RawInput(), lookHeld, dt);
    }
} // namespace mir
