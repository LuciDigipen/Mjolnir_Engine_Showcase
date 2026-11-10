/**
 * @file ContentBrowserPanel.cpp
 * @author Rahul Nair
 * @brief Draws the content browser panel and turns clicks into EditorActions content-browser requests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/ContentBrowserPanel.h>

#include <editor/ContentThumbnailCache.h>
#include <editor/EditorWidgets.h>
#include <editor/EditorPreferences.h>
#include <editortheme/EditorTheme.h>

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace mir
{
    namespace
    {
        std::string Lowered(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string ParentOf(const std::string &relativePath)
        {
            const auto slash = relativePath.find_last_of('/');
            return slash == std::string::npos ? std::string{} : relativePath.substr(0, slash);
        }

        bool IsTextureFile(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return false;
            const auto extension = Lowered(name.substr(dot));
            return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
        }

        const char *IconFor(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return ICON_FA_FILE;
            const std::string extension = Lowered(name.substr(dot));
            if (IsTextureFile(name))
                return ICON_FA_FILE_IMAGE;
            if (extension == ".glb" || extension == ".gltf")
                return ICON_FA_CUBE;
            if (extension == ".wav" || extension == ".ogg" || extension == ".mp3")
                return ICON_FA_FILE_AUDIO;
            if (extension == ".lua" || extension == ".json")
                return ICON_FA_FILE_CODE;
            return ICON_FA_FILE;
        }

        bool IsTextFile(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            if (dot == std::string::npos)
                return false;
            const std::string extension = Lowered(name.substr(dot));
            return extension == ".lua" || extension == ".json";
        }

        bool IsPrefabFile(const std::string &name)
        {
            const auto dot = name.find_last_of('.');
            return dot != std::string::npos && Lowered(name.substr(dot)) == ".prefab";
        }

        std::string FormatSize(uint64_t bytes)
        {
            constexpr const char *kUnits[] = { "B", "KB", "MB", "GB", "TB" };
            constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

            double value = static_cast<double>(bytes);
            size_t unit = 0;
            while (value >= 1024.0 && unit + 1 < kUnitCount)
            {
                value /= 1024.0;
                ++unit;
            }

            char text[32]{};
            if (unit == 0)
                std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
            else
                std::snprintf(text, sizeof(text), "%.1f %s", value, kUnits[unit]);
            return text;
        }

        // Built fresh from the flat entry list every Draw - directories are
        // explicit entries, so an empty or just-created folder still shows up.
        struct FolderNode
        {
            std::string mName;
            std::string mRelativePath;
            std::map<std::string, FolderNode> mChildren;
        };

        void InsertFolder(FolderNode &root, const std::string &relativePath)
        {
            FolderNode *node = &root;
            std::string accumulated;
            size_t start = 0;
            while (start <= relativePath.size())
            {
                const size_t slash = relativePath.find('/', start);
                const size_t length = (slash == std::string::npos ? relativePath.size() : slash) - start;
                const std::string segment = relativePath.substr(start, length);
                accumulated = accumulated.empty() ? segment : accumulated + '/' + segment;
                node = &node->mChildren[segment];
                node->mName = segment;
                node->mRelativePath = accumulated;
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }

        void DrawFolderTree(const FolderNode &node, std::string &currentFolder)
        {
            for (const auto &[name, child] : node.mChildren)
            {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (child.mChildren.empty())
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                const bool selected = currentFolder == child.mRelativePath;
                if (selected)
                    flags |= ImGuiTreeNodeFlags_Selected;
                const int selectionColors = widgets::PushRowSelectionColors(selected);
                const bool open =
                    ImGui::TreeNodeEx(child.mName.c_str(), flags, ICON_FA_FOLDER " %s", child.mName.c_str());
                ImGui::PopStyleColor(selectionColors);
                if (ImGui::IsItemClicked())
                    currentFolder = child.mRelativePath;
                if (open && !child.mChildren.empty())
                {
                    DrawFolderTree(child, currentFolder);
                    ImGui::TreePop();
                }
            }
        }
    } // namespace

    ContentBrowserAction ContentBrowserPanel::Draw(const ContentBrowserView &view, EditorPreferences &preferences)
    {
        ContentBrowserAction action;
        if (!view.mLastRenamedFrom.empty())
        {
            if (mSelectedPath == view.mLastRenamedFrom)
                mSelectedPath = view.mLastRenamedTo;
            if (mRenameTarget == view.mLastRenamedFrom)
                mRenameTarget.clear();
        }

        ImGui::TextDisabled("%s", view.mContentRoot.c_str());
        if (!view.mLastError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
            ImGui::TextWrapped("%s", view.mLastError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        FolderNode root;
        for (const auto &entry : view.mEntries)
            if (entry.mKind == ContentBrowserEntryKind::Directory)
                InsertFolder(root, entry.mRelativePath);

        const float treeWidth = ImGui::GetContentRegionAvail().x * 0.3f;
        if (ImGui::BeginChild("ContentBrowserTree", ImVec2(treeWidth, 0), ImGuiChildFlags_Borders))
        {
            ImGuiTreeNodeFlags rootFlags =
                ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (mCurrentFolder.empty())
                rootFlags |= ImGuiTreeNodeFlags_Selected;
            const int selectionColors = widgets::PushRowSelectionColors(mCurrentFolder.empty());
            const bool rootOpen = ImGui::TreeNodeEx("##content-root", rootFlags, ICON_FA_FOLDER_TREE " %s",
                view.mContentRoot.empty() ? "Content" : view.mContentRoot.c_str());
            ImGui::PopStyleColor(selectionColors);
            if (ImGui::IsItemClicked())
                mCurrentFolder.clear();
            if (rootOpen)
            {
                DrawFolderTree(root, mCurrentFolder);
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        if (ImGui::BeginChild("ContentBrowserFiles", ImVec2(0, 0), ImGuiChildFlags_Borders))
            DrawEntries(view, action, preferences);
        ImGui::EndChild();

        DrawDeleteConfirm(action);
        return action;
    }

    void ContentBrowserPanel::DrawEntries(const ContentBrowserView &view, ContentBrowserAction &action, EditorPreferences &preferences)
    {
        const std::string label = "/" + mCurrentFolder;
        ImGui::TextDisabled("%s", label.c_str());

        if (!mCurrentFolder.empty() && ImGui::SmallButton(ICON_FA_FOLDER_OPEN " .."))
            mCurrentFolder = ParentOf(mCurrentFolder);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER_PLUS " New Folder"))
        {
            mCreatingFolder = true;
            mNewFolderBuffer[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("New Particle System"))
        {
            action.mKind = ContentBrowserActionKind::NewParticleSystem;
            action.mTargetPath = mCurrentFolder;
        }

        if (mCreatingFolder)
        {
            ImGui::SetNextItemWidth(200.0f);
            const bool submitted = ImGui::InputTextWithHint("##new-folder", "folder name", mNewFolderBuffer,
                sizeof(mNewFolderBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                mCreatingFolder = false;
            else if (submitted && mNewFolderBuffer[0] != '\0')
            {
                action.mKind = ContentBrowserActionKind::CreateFolder;
                action.mTargetPath = mCurrentFolder;
                action.mNewName = mNewFolderBuffer;
                mCreatingFolder = false;
            }
        }

        ImGui::Separator();

        preferences.contentThumbnailSize = std::clamp(preferences.contentThumbnailSize, 48, 128);
        preferences.contentThumbnailCapacity = std::clamp(preferences.contentThumbnailCapacity, 0,
            static_cast<int>(ContentThumbnailCache::kMaxThumbnails));
        const bool wasGrid = preferences.contentGrid;
        bool changed = false;
        if (ImGui::RadioButton("List", !preferences.contentGrid))
        {
            changed = preferences.contentGrid;
            preferences.contentGrid = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Grid", preferences.contentGrid))
        {
            changed |= !preferences.contentGrid;
            preferences.contentGrid = true;
        }
        if (preferences.contentGrid)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            changed |= ImGui::SliderInt("Size", &preferences.contentThumbnailSize, 48, 128, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::SameLine();
        if (ImGui::Button("Options"))
            ImGui::OpenPopup("preview-options");
        if (ImGui::BeginPopup("preview-options"))
        {
            ImGui::SetNextItemWidth(180);
            changed |= ImGui::SliderInt("Cached thumbnails", &preferences.contentThumbnailCapacity, 0,
                static_cast<int>(ContentThumbnailCache::kMaxThumbnails), "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextDisabled("Zero disables texture previews.");
            ImGui::EndPopup();
        }
        if (wasGrid != preferences.contentGrid)
            ImGui::SetScrollY(0);
        action.mPreferencesChanged = changed;
        ImGui::Separator();

        std::vector<const ContentBrowserEntryView *> entries;
        for (const auto &entry : view.mEntries)
            if (ParentOf(entry.mRelativePath) == mCurrentFolder)
                entries.push_back(&entry);

        const bool grid = preferences.contentGrid;
        const float size = static_cast<float>(preferences.contentThumbnailSize);
        const float cellWidth = size + 24.0f;
        const int columns = grid ? std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth), 1, 64) : 3;
        if (ImGui::BeginTable(grid ? "content-grid" : "content-list", columns,
            ImGuiTableFlags_RowBg | (grid ? ImGuiTableFlags_SizingStretchSame :
                ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)))
        {
            if (!grid)
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();
            }
            const int rows = grid ? (static_cast<int>(entries.size()) + columns - 1) / columns : static_cast<int>(entries.size());
            const float rowHeight = grid ? size + ImGui::GetTextLineHeight() + 24.0f : ImGui::GetFrameHeight();
            ImGuiListClipper clipper;
            clipper.Begin(rows, rowHeight);
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    ImGui::TableNextRow(0, rowHeight);
                    const int count = grid ? columns : 1;
                    for (int column = 0; column < count; ++column)
                    {
                        const int index = grid ? row * columns + column : row;
                        if (index >= static_cast<int>(entries.size()))
                            break;
                        const auto &entry = *entries[index];
                        ImGui::TableSetColumnIndex(column);
                        DrawEntry(entry, action, grid, size, view);
                        if (!grid)
                        {
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(entry.mKind == ContentBrowserEntryKind::Directory ? "Folder" : "File");
                            ImGui::TableSetColumnIndex(2);
                            if (entry.mKind == ContentBrowserEntryKind::File)
                                ImGui::TextUnformatted(FormatSize(entry.mSizeBytes).c_str());
                        }
                    }
                }
            ImGui::EndTable();
        }
    }

    void ContentBrowserPanel::DrawEntry(const ContentBrowserEntryView &entry, ContentBrowserAction &action,
        bool grid, float size, const ContentBrowserView &view)
    {
        ImGui::PushID(entry.mRelativePath.c_str());
        if (mRenameTarget == entry.mRelativePath)
        {
            if (grid)
                ImGui::Dummy(ImVec2(size, size));
            ImGui::SetNextItemWidth(-1);
            const bool submitted = ImGui::InputText("##rename", mRenameBuffer, sizeof(mRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                mRenameTarget.clear();
            else if (submitted && mRenameBuffer[0] != '\0')
            {
                action.mKind = ContentBrowserActionKind::Rename;
                action.mTargetPath = entry.mRelativePath;
                action.mNewName = mRenameBuffer;
                mRenameTarget.clear();
            }
        }
        else
        {
            const char *icon = entry.mKind == ContentBrowserEntryKind::Directory ? ICON_FA_FOLDER : IconFor(entry.mName);
            const bool selected = entry.mRelativePath == mSelectedPath;
            const int selectionColors = widgets::PushRowSelectionColors(selected);
            bool showTooltip = false;
            if (grid)
            {
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                ImGui::Selectable("##entry", selected, ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(width, size + ImGui::GetTextLineHeight() + 16));
                const bool hovered = ImGui::IsItemHovered();
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 top(origin.x + std::max(0.0f, (width - size) * 0.5f), origin.y + 4);
                ContentBrowserPreview preview;
                const bool texture = entry.mKind == ContentBrowserEntryKind::File && IsTextureFile(entry.mName);
                if (texture && view.mPreview && ImGui::IsItemVisible())
                    preview = view.mPreview(entry.mRelativePath);
                if (preview.mImage && preview.mWidth && preview.mHeight)
                {
                    const float scale = size / std::max(preview.mWidth, preview.mHeight);
                    const ImVec2 extent(preview.mWidth * scale, preview.mHeight * scale);
                    const ImVec2 min(top.x + (size - extent.x) * 0.5f, top.y + (size - extent.y) * 0.5f);
                    const ImVec2 max(min.x + extent.x, min.y + extent.y);
                    for (float y = min.y; y < max.y; y += 8)
                        for (float x = min.x; x < max.x; x += 8)
                        {
                            const bool dark = (static_cast<int>((x - min.x) / 8) + static_cast<int>((y - min.y) / 8)) % 2;
                            draw->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + 8, max.x), std::min(y + 8, max.y)),
                                dark ? IM_COL32(65, 65, 65, 255) : IM_COL32(105, 105, 105, 255));
                        }
                    draw->AddImage(ImTextureRef(static_cast<ImTextureID>(preview.mImage)), min, max);
                }
                else
                {
                    const float iconSize = std::min(48.0f, size * 0.5f);
                    auto *font = ImGui::GetFont();
                    const ImVec2 text = font->CalcTextSizeA(iconSize, 10000, 0, icon);
                    draw->AddText(font, iconSize,
                        ImVec2(top.x + (size - text.x) * 0.5f, top.y + (size - text.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), icon);
                }
                std::string label = entry.mName + (entry.mUnsaved ? " *" : "");
                const float labelWidth = std::max(1.0f, width - 8);
                if (ImGui::CalcTextSize(label.c_str()).x > labelWidth)
                {
                    while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > labelWidth)
                    {
                        size_t last = label.size() - 1;
                        while (last > 0 && (static_cast<unsigned char>(label[last]) & 0xC0) == 0x80)
                            --last;
                        label.resize(last);
                    }
                    label += "...";
                }
                draw->AddText(ImVec2(origin.x + 4, top.y + size + 4), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                showTooltip = hovered;
            }
            else
            {
                ImGui::Selectable((std::string(icon) + "  " + entry.mName + (entry.mUnsaved ? "  (unsaved)" : "")).c_str(),
                    selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, ImGui::GetFrameHeight() - ImGui::GetStyle().CellPadding.y * 2));
            }
            ImGui::PopStyleColor(selectionColors);
            if (ImGui::IsItemClicked())
                mSelectedPath = entry.mRelativePath;

            if (entry.mKind == ContentBrowserEntryKind::File && IsPrefabFile(entry.mName) &&
                ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kPrefabContentPayload, entry.mRelativePath.data(),
                    entry.mRelativePath.size());
                ImGui::TextUnformatted(entry.mName.c_str());
                ImGui::EndDragDropSource();
            }
            if (entry.mKind == ContentBrowserEntryKind::File && entry.mName.ends_with(".particle.json") &&
                ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kParticleContentPayload, entry.mRelativePath.data(),
                    entry.mRelativePath.size());
                ImGui::TextUnformatted(entry.mName.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (entry.mKind == ContentBrowserEntryKind::Directory)
                    mCurrentFolder = entry.mRelativePath;
                else if (entry.mName.ends_with(".particle.json"))
                {
                    action.mKind = ContentBrowserActionKind::OpenParticleSystem;
                    action.mTargetPath = entry.mRelativePath;
                }
                else if (IsPrefabFile(entry.mName))
                {
                    action.mKind = ContentBrowserActionKind::OpenPrefab;
                    action.mTargetPath = entry.mRelativePath;
                }
                else if (IsTextFile(entry.mName))
                {
                    action.mKind = ContentBrowserActionKind::OpenInIde;
                    action.mTargetPath = entry.mRelativePath;
                }
            }

            if (ImGui::BeginPopupContextItem())
            {
                mSelectedPath = entry.mRelativePath;
                if (ImGui::MenuItem("Rename"))
                {
                    mRenameTarget = entry.mRelativePath;
                    std::snprintf(mRenameBuffer, sizeof(mRenameBuffer), "%s", entry.mName.c_str());
                }
                if (ImGui::MenuItem("Delete"))
                {
                    mPendingDelete = entry.mRelativePath;
                    mPendingDeleteIsFolder = entry.mKind == ContentBrowserEntryKind::Directory;
                    mDeleteConfirmRequested = true;
                }
                ImGui::EndPopup();
            }
            if (showTooltip)
                ImGui::SetTooltip("%s%s", entry.mName.c_str(), entry.mUnsaved ? " (unsaved)" : "");
        }
        ImGui::PopID();
    }

    void ContentBrowserPanel::DrawDeleteConfirm(ContentBrowserAction &action)
    {
        constexpr const char *kPopupId = "Delete##ContentBrowserConfirm";

        // outside any row's ID stack: OpenPopup needs the same ID stack as BeginPopupModal
        if (mDeleteConfirmRequested)
        {
            ImGui::OpenPopup(kPopupId);
            mDeleteConfirmRequested = false;
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(
            kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::Text("Delete \"%s\"?", mPendingDelete.c_str());
        if (mPendingDeleteIsFolder)
            ImGui::TextDisabled("This folder and everything inside it will be deleted. This cannot be undone.");
        else
            ImGui::TextDisabled("This cannot be undone.");
        ImGui::Spacing();

        if (ImGui::Button("Delete"))
        {
            action.mKind = ContentBrowserActionKind::Delete;
            action.mTargetPath = mPendingDelete;
            mPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            mPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
} // namespace mir
