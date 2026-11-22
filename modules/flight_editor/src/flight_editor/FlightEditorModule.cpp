/**
 * @file FlightEditorModule.cpp
 * @author Jackey Cen
 * @brief Editing *.aircraft.json files from the Content Browser, registered with the editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightEditorModule.h>

#include <flight_editor/FlightModelCreate.h>
#include <flight_editor/FlightModelDraft.h>
#include <flight_editor/FlightModelTemplates.h>

#include <flight/AircraftConfig.h>
#include <flight_debug/FlightDebugRows.h>

#include <editor/AssetImportManager.h>
#include <editor/EditorExtension.h>
#include <editor/EditorModule.h>

#include <core/ecs/EngineMode.h>
#include <core/log/Log.h>

#include <imgui.h>

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr const char *kCreateItem = "New Flight Model Settings";
        constexpr const char *kWindowName = "Flight Model";
        constexpr auto kPollInterval = std::chrono::milliseconds(250);

        std::string_view RawImportStateName(RawImportState state)
        {
            switch (state)
            {
            case RawImportState::Queued:
                return "queued";
            case RawImportState::Cooking:
                return "cooking";
            case RawImportState::WaitingForStop:
                return "waiting for Stop";
            case RawImportState::WaitingForApply:
                return "waiting to apply";
            case RawImportState::Applied:
                return "applied";
            case RawImportState::Failed:
                return "failed";
            }
            return "unknown";
        }

        std::string ImportStatusText(const std::optional<RawImportStatus> &status)
        {
            if (!status)
                return "not queued";
            std::string text(RawImportStateName(status->mState));
            if (!status->mReason.empty())
                text += ": " + status->mReason;
            return text;
        }

        // Scaled so a value of any magnitude still moves a useful amount per pixel.
        float DragSpeed(float value)
        {
            return std::max(0.01f, std::fabs(value) * 0.001f);
        }

        bool EndsWithAircraftJson(std::string_view path)
        {
            return path.ends_with(".aircraft.json");
        }
    } // namespace

    class FlightEditorModule::Extension final : public IEditorExtension
    {
    public:
        Extension(EditorModule &editor)
            : mEditor(editor)
        {
        }

        void Update(const EditorExtensionContext &context) override
        {
            mEditing = context.mMode.mMode == EngineMode::Edit;
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (mDraft.HasFile() && now - mLastPoll >= kPollInterval)
            {
                mLastPoll = now;
                mDraft.Poll(AssetsRoot());
            }
        }

        std::span<const char *const> WindowNames() const override
        {
            static constexpr const char *kWindows[]{ kWindowName };
            return kWindows;
        }

        void DrawWindows(const EditorExtensionContext &context) override
        {
            if (mCreateDialogPending)
            {
                ImGui::OpenPopup(kCreateItem);
                mCreateDialogPending = false;
            }
            if (mAskOpenPending)
            {
                ImGui::OpenPopup("Unsaved flight model");
                mAskOpenPending = false;
            }
            if (mConflictPending)
            {
                ImGui::OpenPopup("Flight model changed on disk");
                mConflictPending = false;
            }

            DrawWindow(context);
            DrawCreateDialog();
            DrawAskOpenDialog();
            DrawConflictDialog();
        }

        std::span<const char *const> ContentBrowserCreateItems() const override
        {
            static constexpr const char *kItems[]{ kCreateItem };
            return kItems;
        }

        void OnContentBrowserCreateItem(std::string_view item, std::optional<std::string_view> assetsFolder) override
        {
            if (item != kCreateItem)
                return;

            mCreateName[0] = '\0';
            mCreateTemplate = FlightModelTemplate::Powered;
            mCreateFolder = assetsFolder ? std::optional<std::string>(*assetsFolder) : std::nullopt;
            mCreateDialogPending = true;
            mError.clear();
        }

        bool OpenContentBrowserFile(std::string_view assetsRelativePath) override
        {
            if (!EndsWithAircraftJson(assetsRelativePath))
                return false;
            RequestOpen(assetsRelativePath);
            return true;
        }

        std::span<const char *const> UnsavedDraftNames() const override
        {
            if (!mDraft.Dirty())
                return {};
            static constexpr const char *kNames[]{ "flight model drafts" };
            return kNames;
        }

    private:
        const std::filesystem::path &AssetsRoot() const { return mEditor.Import().SourceAssetRoot(); }

        void RequestOpen(std::string_view assetsRelativePath)
        {
            if (mDraft.HasFile() && mDraft.Path() == assetsRelativePath)
            {
                mWindowOpen = true;
                return;
            }
            if (EvaluateFlightModelOpen(mDraft.Dirty(), mDraft.Path(), assetsRelativePath) ==
                FlightModelOpenAction::Ask)
            {
                mPendingOpen = std::string(assetsRelativePath);
                mAskOpenPending = true;
                return;
            }
            OpenPath(assetsRelativePath);
        }

        void OpenPath(std::string_view assetsRelativePath)
        {
            std::string error;
            mWindowOpen = true;
            mLastPoll = std::chrono::steady_clock::now();
            if (!mDraft.Open(AssetsRoot(), assetsRelativePath, error))
            {
                mStatus = error;
                return;
            }
            mStatus = "Opened.";
            mError.clear();
        }

        void OpenPending()
        {
            if (!mPendingOpen)
                return;
            const std::string path = std::move(*mPendingOpen);
            mPendingOpen.reset();
            OpenPath(path);
        }

        bool SaveDraft(bool overwrite)
        {
            const SavedSourceCallback onSaved = [this](const std::filesystem::path &saved)
            {
                if (!mEditor.Import().QueueChanged(saved))
                    MIR_LOG_ERROR("Flight Model: saved '{}', but it is outside the source assets root", saved.string());
            };

            std::string error;
            const FlightModelSaveResult result = mDraft.Save(AssetsRoot(), onSaved, error, overwrite);
            switch (result)
            {
            case FlightModelSaveResult::Saved:
                mStatus = "Saved and queued for import.";
                mError.clear();
                return true;
            case FlightModelSaveResult::Invalid:
                mError = error;
                return false;
            case FlightModelSaveResult::ChangedOnDisk:
                mError = error;
                mConflictPending = true;
                return false;
            }
            return false;
        }

        bool CreateFromDialog()
        {
            const FlightModelCreateRequest request{ mCreateName, mCreateTemplate, mCreateFolder };
            std::string assetsRelativePath;
            std::filesystem::path absolutePath;
            std::string error;
            if (!CreateFlightModelFile(request, AssetsRoot(), assetsRelativePath, absolutePath, error))
            {
                mError = error;
                return false;
            }

            if (!mEditor.Import().QueueChanged(absolutePath))
                MIR_LOG_ERROR("Flight Model: created '{}', but it is outside the source assets root",
                    absolutePath.string());

            mWindowOpen = true;
            if (mDraft.HasFile() && mDraft.Dirty())
            {
                mPendingOpen = assetsRelativePath;
                mAskOpenPending = true;
            }
            else
            {
                OpenPath(assetsRelativePath);
            }
            mStatus = std::format("Created '{}'.", assetsRelativePath);
            mError.clear();
            return true;
        }

        void DrawWindow(const EditorExtensionContext &context)
        {
            if (!mWindowOpen || !mDraft.HasFile())
                return;

            ImGui::SetNextWindowSize(ImVec2(430.0f, 720.0f), ImGuiCond_FirstUseEver);
            if (context.mInspectorDockId != 0)
                ImGui::SetNextWindowDockID(context.mInspectorDockId, ImGuiCond_FirstUseEver);

            bool open = true;
            if (ImGui::Begin(kWindowName, &open))
            {
                ImGui::TextUnformatted(mDraft.Path().c_str());
                ImGui::Separator();
                if (mDraft.Loaded())
                    DrawDraft(context);
                else
                    DrawParseError(context);
            }
            ImGui::End();

            if (!open)
                mWindowOpen = false;
        }

        void DrawParseError(const EditorExtensionContext &context)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", mDraft.ParseError().c_str());
            ImGui::PopTextWrapPos();
            if (context.mOpenAssetInIde && ImGui::Button("Open in IDE"))
                context.mOpenAssetInIde(mDraft.Path());
        }

        void DrawDraft(const EditorExtensionContext &context)
        {
            AircraftConfig *config = mDraft.Config();
            if (config == nullptr)
                return;

            if (config->aero.l3.has_value())
                ImGui::TextUnformatted("This config has an L3 profile; these L2 edits do not change it.");

            const std::filesystem::path absolutePath = AssetsRoot() / std::filesystem::path(mDraft.Path());
            ImGui::Text("Import: %s", ImportStatusText(mEditor.Import().ChangedStatus(absolutePath)).c_str());

            ImGui::BeginDisabled(!mEditing);
            if (ImGui::Button("Save"))
            {
                if (mDraft.DiskChanged(AssetsRoot()))
                    mConflictPending = true;
                else
                    SaveDraft(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert"))
            {
                std::string error;
                if (!mDraft.Revert(AssetsRoot(), error))
                    mStatus = error;
                else
                    mStatus = "Reverted from disk.";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (context.mOpenAssetInIde && ImGui::Button("Open in IDE"))
                context.mOpenAssetInIde(mDraft.Path());
            ImGui::SameLine();
            if (mDraft.Dirty())
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "unsaved");
            else
                ImGui::TextDisabled("saved");

            if (!mStatus.empty())
                ImGui::TextUnformatted(mStatus.c_str());
            if (!mError.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", mError.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::BeginDisabled(!mEditing);
            std::string group;
            for (const FlightDebugField &field : BuildFlightDebugEditableFields(*config))
            {
                if (field.group != group)
                {
                    group = field.group;
                    ImGui::SeparatorText(group.c_str());
                }

                float value = field.get(*config);
                if (field.degrees)
                    value = FlightDebugDegrees(value);

                const std::string id = field.group + '/' + field.label;
                ImGui::PushID(id.c_str());
                ImGui::SetNextItemWidth(170.0f);
                if (ImGui::DragFloat(field.label.c_str(), &value, DragSpeed(value)))
                {
                    field.set(*config, field.degrees ? glm::radians(value) : value);
                    mDraft.MarkEdited();
                    mError.clear();
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }

        void DrawCreateDialog()
        {
            if (!ImGui::BeginPopupModal(kCreateItem, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

            ImGui::InputText("Name", mCreateName, sizeof(mCreateName));

            const std::array<std::string, 2> templateNames{
                std::string(FlightModelTemplateName(FlightModelTemplate::Powered)),
                std::string(FlightModelTemplateName(FlightModelTemplate::Glider)),
            };
            const std::array<const char *, 2> templateItems{ templateNames[0].c_str(), templateNames[1].c_str() };
            int templateIndex = static_cast<int>(mCreateTemplate);
            if (ImGui::Combo("Template", &templateIndex, templateItems.data(),
                    static_cast<int>(templateItems.size())))
                mCreateTemplate = static_cast<FlightModelTemplate>(templateIndex);

            const FlightModelCreateRequest request{ mCreateName, mCreateTemplate, mCreateFolder };
            const FlightModelCreatePreview preview = PreviewFlightModelCreate(request, AssetsRoot());
            ImGui::TextDisabled("%s", preview.assetsRelativePath.c_str());

            const std::string reason = mEditing ? preview.error : std::string("Create in Edit.");
            if (!reason.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", reason.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::BeginDisabled(!reason.empty());
            if (ImGui::Button("Create") && CreateFromDialog())
                ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void DrawAskOpenDialog()
        {
            if (!ImGui::BeginPopupModal("Unsaved flight model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Save the changes to '%s' before opening '%s'?", mDraft.Path().c_str(),
                mPendingOpen ? mPendingOpen->c_str() : "");
            ImGui::PopTextWrapPos();

            if (ImGui::Button("Save"))
            {
                if (SaveDraft(false))
                {
                    OpenPending();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard"))
            {
                OpenPending();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                mPendingOpen.reset();
                ImGui::CloseCurrentPopup();
            }

            if (!mError.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", mError.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        }

        void DrawConflictDialog()
        {
            if (!ImGui::BeginPopupModal("Flight model changed on disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("'%s' changed on disk since it was opened or saved.", mDraft.Path().c_str());
            ImGui::PopTextWrapPos();

            if (ImGui::Button("Overwrite"))
            {
                if (SaveDraft(true))
                    ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload"))
            {
                std::string error;
                if (!mDraft.Revert(AssetsRoot(), error))
                    mStatus = error;
                else
                    mStatus = "Reloaded from disk.";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();

            if (!mError.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", mError.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        }

        EditorModule &mEditor;
        FlightModelDraft mDraft;
        std::string mStatus;
        std::string mError;
        std::optional<std::string> mPendingOpen;
        std::optional<std::string> mCreateFolder;
        FlightModelTemplate mCreateTemplate = FlightModelTemplate::Powered;
        char mCreateName[128]{};
        std::chrono::steady_clock::time_point mLastPoll{};
        bool mEditing = false;
        bool mWindowOpen = false;
        bool mCreateDialogPending = false;
        bool mAskOpenPending = false;
        bool mConflictPending = false;
    };

    FlightEditorModule::FlightEditorModule(ModuleContext, EditorModule &editor)
        : mExtension(std::make_unique<Extension>(editor))
    {
        mRegistration = editor.Get().AddExtension(*mExtension);
    }

    FlightEditorModule::~FlightEditorModule() = default;
} // namespace mir
