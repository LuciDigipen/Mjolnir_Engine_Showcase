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
        constexpr uint32_t kCollectionTag = 0x74746366u; // 'ttcf'

        uint32_t ReadU32(std::span<const uint8_t> bytes, size_t at)
        {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[at + 2]) << 8) | static_cast<uint32_t>(bytes[at + 3]);
        }

        // 1.0, 'OTTO', 'true'
        bool IsFontTag(uint32_t tag)
        {
            return tag == 0x00010000u || tag == 0x4F54544Fu || tag == 0x74727565u;
        }

        // a font's tag, then a table directory whose tables all lie inside the file: a backend reads them
        // without checking, so a cut-off file is refused here
        bool TablesFit(std::span<const uint8_t> bytes, uint64_t directory)
        {
            if (directory + 12 > bytes.size() || !IsFontTag(ReadU32(bytes, directory)))
                return false;
            const uint64_t count = (uint64_t{bytes[directory + 4]} << 8) | bytes[directory + 5];
            if (count == 0 || directory + 12 + count * 16 > bytes.size())
                return false;
            for (uint64_t table = 0; table < count; ++table)
            {
                const uint64_t record = directory + 12 + table * 16;
                if (uint64_t{ReadU32(bytes, record + 8)} + ReadU32(bytes, record + 12) > bytes.size())
                    return false;
            }
            return true;
        }

        // a TrueType or OpenType file, or a collection of them, which backends draw the first font of
        bool IsFontFile(std::span<const uint8_t> bytes)
        {
            if (bytes.size() <= 100)
                return false;
            if (ReadU32(bytes, 0) != kCollectionTag)
                return TablesFit(bytes, 0);
            return ReadU32(bytes, 8) > 0 && TablesFit(bytes, ReadU32(bytes, 12));
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
            MIR_LOG_ERROR("gameui: {} bytes are not a whole TrueType or OpenType font", bytes.size());
            return {};
        }

        Font &font = mFonts.emplace_back(Font{std::vector<uint8_t>(bytes.begin(), bytes.end())});
        Make(font);
        // the backend said why
        if (mBackend != nullptr && font.backend == nullptr)
        {
            mFonts.pop_back();
            return {};
        }
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
