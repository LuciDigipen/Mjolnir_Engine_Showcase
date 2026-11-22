/**
 * @file UIFonts.cpp
 * @author Quy Dong Ta
 * @brief The game UI's fonts: TrueType and OpenType files from the game's
 *        assets, kept as bytes and copied into fonts the backend draws with.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIFonts.h>

#include <gameui/UIBackend.h>

#include <assets/AssetCache.h>
#include <core/log/Log.h>

namespace mir
{
    namespace
    {
        // the tag a TrueType, OpenType or collection file starts with, a backend may assert on anything else
        bool IsFontFile(std::span<const uint8_t> bytes)
        {
            if (bytes.size() <= 100)
                return false;
            const uint32_t tag = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
                                 (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
            // 1.0, 'OTTO', 'true', 'ttcf'
            return tag == 0x00010000u || tag == 0x4F54544Fu || tag == 0x74727565u || tag == 0x74746366u;
        }
    } // namespace

    UIFonts::UIFonts() = default;

    UIFonts::~UIFonts()
    {
        for (Font &font : mFonts)
        {
            if (font.release != nullptr)
                font.release(font.backend);
        }
    }

    UIFont UIFonts::Load(AssetCache &assets, AssetId id)
    {
        if (const auto loaded = mLoaded.find(id.value); loaded != mLoaded.end())
            return loaded->second;

        UIFont font;
        if (const AssetBlobView *blob = assets.Load(id); blob == nullptr)
            MIR_LOG_ERROR("gameui: font {:016x} did not load", id.value);
        else
            font = Add(std::span(reinterpret_cast<const uint8_t *>(blob->content.data()), blob->content.size()));

        mLoaded.emplace(id.value, font);
        return font;
    }

    UIFont UIFonts::Add(std::span<const uint8_t> bytes)
    {
        if (!IsFontFile(bytes))
        {
            MIR_LOG_ERROR("gameui: {} bytes are not a TrueType or OpenType font", bytes.size());
            return {};
        }

        Make(mFonts.emplace_back(Font{std::vector<uint8_t>(bytes.begin(), bytes.end())}));
        return UIFont{static_cast<uint32_t>(mFonts.size())};
    }

    void *UIFonts::Find(UIFont font) const
    {
        if (!font.IsValid() || font.mId > mFonts.size())
            return nullptr;
        return mFonts[font.mId - 1].backend;
    }

    void UIFonts::SetBackend(IUIBackend *backend)
    {
        mBackend = backend;
        for (Font &font : mFonts)
            Make(font);
    }

    void UIFonts::Make(Font &font)
    {
        if (mBackend == nullptr || font.backend != nullptr)
            return;

        const UIBackendFont made = mBackend->MakeFont(font.bytes);
        font.backend = made.font;
        font.release = made.release;
    }
} // namespace mir
