/**
 * @file EditorImage.cpp
 * @author Jackey Cen
 * @brief An image file loaded into an ImGui texture, owned safely across ImGui shutdown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorImage.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <imgui.h>
// RegisterUserTexture and UnregisterUserTexture
#include <imgui_internal.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

namespace mir
{
    struct EditorImageTextureState
    {
        std::vector<ImTextureData *> mTextures;
        bool mHandled = false;
    };

    namespace
    {
        // keeps a mistaken pick from allocating hundreds of MB on the GPU
        constexpr int kMaxSide = 8192;

        using TextureStateHolder = std::shared_ptr<EditorImageTextureState>;

        void FreeImageTextures(ImGuiContext *ctx, ImGuiContextHook *hook)
        {
            auto *holder = static_cast<TextureStateHolder *>(hook->UserData);
            if (holder == nullptr)
                return;

            const TextureStateHolder state = *holder;
            delete holder;

            for (ImTextureData *texture : state->mTextures)
            {
                if (texture->TexID != ImTextureID_Invalid || texture->BackendUserData != nullptr)
                    MIR_LOG_ERROR("Editor image texture still had renderer data at ImGui shutdown");
                ImGui::UnregisterUserTexture(texture);
                if (ctx != nullptr)
                    ctx->PlatformIO.Textures.find_erase(texture);
                delete texture;
            }
            state->mTextures.clear();
            state->mHandled = true;
        }
    }

    EditorImage::~EditorImage()
    {
        if (mCleanup != nullptr && mCleanup->mHandled)
        {
            mTexture = nullptr;
            mRetired.clear();
            return;
        }

        if (mCleanup != nullptr)
        {
            const auto remember = [this](ImTextureData *texture)
            {
                if (std::find(mCleanup->mTextures.begin(), mCleanup->mTextures.end(), texture) ==
                    mCleanup->mTextures.end())
                    mCleanup->mTextures.push_back(texture);
            };
            if (mTexture != nullptr)
                remember(mTexture);
            for (ImTextureData *texture : mRetired)
                remember(texture);

            mTexture = nullptr;
            mRetired.clear();
            return;
        }

        std::vector<ImTextureData *> textures = std::move(mRetired);
        if (mTexture != nullptr)
            textures.push_back(mTexture);
        mTexture = nullptr;
        mRetired.clear();

        for (ImTextureData *texture : textures)
        {
            if (texture->TexID == ImTextureID_Invalid && texture->BackendUserData == nullptr)
            {
                delete texture;
                continue;
            }
            MIR_LOG_ERROR("Editor image texture still had renderer data without a live ImGui context");
        }
    }

    bool EditorImage::EnsureCleanup()
    {
        if (mCleanup != nullptr)
            return true;
        if (mContext == nullptr)
            return false;

        mCleanup = std::make_shared<EditorImageTextureState>();
        auto *holder = new TextureStateHolder(mCleanup);
        ImGuiContextHook hook{};
        hook.Type = ImGuiContextHookType_Shutdown;
        hook.Callback = FreeImageTextures;
        hook.UserData = holder;
        ImGui::AddContextHook(mContext, &hook);
        return true;
    }

    bool EditorImage::Load(const std::filesystem::path &path, std::string &error)
    {
        ImGuiContext *const context = ImGui::GetCurrentContext();
        if (context == nullptr)
        {
            error = "no ImGui context";
            return false;
        }
        if (mContext != nullptr && mContext != context)
        {
            error = "ImGui context changed";
            return false;
        }
        mContext = context;
        if (!EnsureCleanup())
        {
            error = "cannot register image cleanup";
            return false;
        }

        const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        if (!bytes || bytes->empty() || bytes->size() > static_cast<size_t>(INT_MAX))
        {
            error = "cannot read the file";
            return false;
        }

        const auto *data = reinterpret_cast<const stbi_uc *>(bytes->data());
        const int size = static_cast<int>(bytes->size());
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info_from_memory(data, size, &width, &height, &channels) == 0 || width <= 0 || height <= 0)
        {
            error = std::string("not a supported image (") + stbi_failure_reason() + ")";
            return false;
        }
        if (width > kMaxSide || height > kMaxSide)
        {
            error = std::to_string(width) + "x" + std::to_string(height) + " is over " + std::to_string(kMaxSide) +
                    " px per side";
            return false;
        }

        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load_from_memory(data, size, &width, &height, &channels, 4), stbi_image_free);
        if (pixels == nullptr)
        {
            error = std::string("cannot decode the image (") + stbi_failure_reason() + ")";
            return false;
        }

        auto *texture = new ImTextureData;
        texture->Create(ImTextureFormat_RGBA32, width, height);
        std::memcpy(texture->GetPixels(), pixels.get(), static_cast<size_t>(width) * height * 4);
        texture->UseColors = true;
        texture->UsedRect = ImTextureRect{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
        ImGui::RegisterUserTexture(texture);
        mCleanup->mTextures.push_back(texture);

        if (mTexture != nullptr)
            Retire(mTexture);
        mTexture = texture;
        return true;
    }

    void EditorImage::Clear()
    {
        if (mTexture == nullptr)
            return;
        Retire(mTexture);
        mTexture = nullptr;
    }

    void EditorImage::Retire(ImTextureData *texture)
    {
        // The backend frees it once no frame in flight uses it, then marks it Destroyed.
        texture->WantDestroyNextFrame = true;
        mRetired.push_back(texture);
    }

    glm::vec2 EditorImage::Size() const
    {
        if (mTexture == nullptr)
            return { 0.0f, 0.0f };
        return { mTexture->Width, mTexture->Height };
    }

    void EditorImage::CollectRetired()
    {
        std::erase_if(mRetired, [this](ImTextureData *texture)
            {
                if (texture->Status != ImTextureStatus_Destroyed)
                    return false;
                ImGui::UnregisterUserTexture(texture);
                ImGui::GetPlatformIO().Textures.find_erase(texture);
                if (mCleanup != nullptr)
                    std::erase(mCleanup->mTextures, texture);
                delete texture;
                return true;
            });
    }

    void EditorImage::AddTo(ImDrawList &drawList, glm::vec2 min, glm::vec2 max, glm::vec2 uvMin, glm::vec2 uvMax) const
    {
        if (mTexture == nullptr)
            return;
        drawList.AddImage(mTexture->GetTexRef(), min, max, uvMin, uvMax);
    }
}
