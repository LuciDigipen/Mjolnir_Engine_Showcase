/**
 * @file EditorImage.h
 * @author Jackey Cen
 * @brief An image file loaded into an ImGui texture, owned safely across ImGui shutdown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/vec2.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;
struct ImTextureData;
struct ImGuiContext;

namespace mir
{
    struct EditorImageTextureState;

    class EditorImage
    {
    public:
        EditorImage() = default;
        ~EditorImage();

        EditorImage(const EditorImage &) = delete;
        EditorImage &operator=(const EditorImage &) = delete;
        EditorImage(EditorImage &&) = delete;
        EditorImage &operator=(EditorImage &&) = delete;

        // Needs an ImGui context. On failure the current image stays and @p error says why.
        bool Load(const std::filesystem::path &path, std::string &error);
        void Clear();
        bool HasImage() const { return mTexture != nullptr; }

        // size in texels; zero without an image
        glm::vec2 Size() const;

        // frees textures the backend has released; once per frame
        void CollectRetired();

        // draws nothing without an image
        void AddTo(ImDrawList &drawList, glm::vec2 min, glm::vec2 max, glm::vec2 uvMin = { 0.0f, 0.0f },
            glm::vec2 uvMax = { 1.0f, 1.0f }) const;

    private:
        void Retire(ImTextureData *texture);
        bool EnsureCleanup();

        ImTextureData *mTexture = nullptr;
        std::vector<ImTextureData *> mRetired;
        ImGuiContext *mContext = nullptr;
        std::shared_ptr<EditorImageTextureState> mCleanup;
    };
}
