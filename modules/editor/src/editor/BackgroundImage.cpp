/**
 * @file BackgroundImage.cpp
 * @author Jackey Cen
 * @brief The editor's background image, drawn under the docked panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BackgroundImage.h>
#include <editor/ImageDecode.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <imgui.h>
// RegisterUserTexture and UnregisterUserTexture
#include <imgui_internal.h>


#include <glm/common.hpp>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace mir
{
    struct BackgroundTextureState
    {
        std::vector<ImTextureData *> mTextures;
        bool mHandled = false;
    };

    namespace
    {
        // keeps a mistaken pick from allocating hundreds of MB on the GPU
        constexpr int kMaxSide = 8192;

        using TextureStateHolder = std::shared_ptr<BackgroundTextureState>;

        void FreeBackgroundTextures(ImGuiContext *ctx, ImGuiContextHook *hook)
        {
            auto *holder = static_cast<TextureStateHolder *>(hook->UserData);
            if (holder == nullptr)
                return;

            const TextureStateHolder state = *holder;
            delete holder;

            for (ImTextureData *texture : state->mTextures)
            {
                if (texture->TexID != ImTextureID_Invalid || texture->BackendUserData != nullptr)
                    MIR_LOG_ERROR("Editor background texture still had renderer data at ImGui shutdown");
                ImGui::UnregisterUserTexture(texture);
                if (ctx != nullptr)
                    ctx->PlatformIO.Textures.find_erase(texture);
                delete texture;
            }
            state->mTextures.clear();
            state->mHandled = true;
        }
    }

    BackgroundImage::~BackgroundImage()
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
            MIR_LOG_ERROR("Editor background texture still had renderer data without a live ImGui context");
        }
    }

    bool BackgroundImage::EnsureCleanup()
    {
        if (mCleanup != nullptr)
            return true;
        if (mContext == nullptr)
            return false;

        mCleanup = std::make_shared<BackgroundTextureState>();
        auto *holder = new TextureStateHolder(mCleanup);
        ImGuiContextHook hook{};
        hook.Type = ImGuiContextHookType_Shutdown;
        hook.Callback = FreeBackgroundTextures;
        hook.UserData = holder;
        ImGui::AddContextHook(mContext, &hook);
        return true;
    }

    bool BackgroundImage::Load(const std::filesystem::path &path, std::string &error)
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
            error = "cannot register background cleanup";
            return false;
        }

        DecodedImage image;
        if (!ReadImage(path, kMaxSide, image, error))
            return false;
        const int width = image.mWidth, height = image.mHeight;

        auto *texture = new ImTextureData;
        texture->Create(ImTextureFormat_RGBA32, width, height);
        std::memcpy(texture->GetPixels(), image.mPixels.get(), static_cast<size_t>(width) * height * 4);
        texture->UseColors = true;
        texture->UsedRect = ImTextureRect{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
        ImGui::RegisterUserTexture(texture);
        mCleanup->mTextures.push_back(texture);

        if (mTexture != nullptr)
            Retire(mTexture);
        mTexture = texture;
        return true;
    }

    void BackgroundImage::Clear()
    {
        if (mTexture == nullptr)
            return;
        Retire(mTexture);
        mTexture = nullptr;
    }

    void BackgroundImage::Retire(ImTextureData *texture)
    {
        // The backend frees it once no frame in flight uses it, then marks it Destroyed.
        texture->WantDestroyNextFrame = true;
        mRetired.push_back(texture);
    }

    void BackgroundImage::CollectRetired()
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

    void BackgroundImage::Draw(BackgroundFit fit, glm::vec2 areaMin, glm::vec2 areaMax, glm::vec2 excludeMin,
        glm::vec2 excludeMax) const
    {
        if (mTexture == nullptr)
            return;

        const glm::vec2 areaSize = areaMax - areaMin;
        if (areaSize.x <= 0.0f || areaSize.y <= 0.0f)
            return;

        glm::vec2 imageMin = areaMin;
        glm::vec2 imageSize = areaSize;
        if (fit != BackgroundFit::Stretch)
        {
            const glm::vec2 scales = areaSize / glm::vec2(mTexture->Width, mTexture->Height);
            const float scale = fit == BackgroundFit::Cover ? std::max(scales.x, scales.y) : std::min(scales.x, scales.y);
            imageSize = glm::vec2(mTexture->Width, mTexture->Height) * scale;
            imageMin = areaMin + (areaSize - imageSize) * 0.5f;
        }

        const glm::vec2 lo = glm::max(imageMin, areaMin);
        const glm::vec2 hi = glm::min(imageMin + imageSize, areaMax);

        ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
        const auto band = [&](glm::vec2 a, glm::vec2 b)
        {
            a = glm::max(a, lo);
            b = glm::min(b, hi);
            if (a.x >= b.x || a.y >= b.y)
                return;
            drawList->AddImage(mTexture->GetTexRef(), a, b, (a - imageMin) / imageSize, (b - imageMin) / imageSize);
        };

        if (excludeMin.x >= excludeMax.x || excludeMin.y >= excludeMax.y)
        {
            band(lo, hi);
            return;
        }
        band({lo.x, lo.y}, {hi.x, excludeMin.y});
        band({lo.x, excludeMax.y}, {hi.x, hi.y});
        band({lo.x, excludeMin.y}, {excludeMin.x, excludeMax.y});
        band({excludeMax.x, excludeMin.y}, {hi.x, excludeMax.y});
    }

    std::optional<std::filesystem::path> PickImageFile()
    {
#if defined(_WIN32)
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        {
            MIR_LOG_ERROR("PickImageFile: CoInitializeEx failed ({:#x})", static_cast<unsigned long>(init));
            return std::nullopt;
        }

        std::optional<std::filesystem::path> picked;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            const COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
            dialog->SetFileTypes(1, filters);
            dialog->SetTitle(L"Choose a background image");
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options)))
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

            // modal: the editor stops rendering until it closes
            if (SUCCEEDED(dialog->Show(GetActiveWindow())))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                        picked = std::filesystem::path(name);
                    CoTaskMemFree(name);
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(init))
            CoUninitialize();
        return picked;
#else
        return std::nullopt;
#endif
    }
}
