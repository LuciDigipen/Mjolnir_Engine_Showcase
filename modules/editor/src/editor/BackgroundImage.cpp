/**
 * @file BackgroundImage.cpp
 * @author Jackey Cen
 * @brief The editor's background image, drawn under the docked panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BackgroundImage.h>

#include <core/log/Log.h>

#include <imgui.h>

#include <glm/common.hpp>

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace mir
{
    void BackgroundImage::Draw(BackgroundFit fit, glm::vec2 areaMin, glm::vec2 areaMax, glm::vec2 excludeMin,
        glm::vec2 excludeMax) const
    {
        if (!mImage.HasImage())
            return;

        const glm::vec2 areaSize = areaMax - areaMin;
        if (areaSize.x <= 0.0f || areaSize.y <= 0.0f)
            return;

        glm::vec2 imageMin = areaMin;
        glm::vec2 imageSize = areaSize;
        if (fit != BackgroundFit::Stretch)
        {
            const glm::vec2 scales = areaSize / mImage.Size();
            const float scale = fit == BackgroundFit::Cover ? std::max(scales.x, scales.y) : std::min(scales.x, scales.y);
            imageSize = mImage.Size() * scale;
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
            mImage.AddTo(*drawList, a, b, (a - imageMin) / imageSize, (b - imageMin) / imageSize);
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
