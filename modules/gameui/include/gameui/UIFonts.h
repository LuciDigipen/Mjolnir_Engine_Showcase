/**
 * @file UIFonts.h
 * @author Quy Dong Ta
 * @brief The game UI's fonts: TrueType and OpenType files from the game's
 *        assets, kept as bytes and copied into fonts the backend draws with.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <assets/AssetId.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetCache;
    class IUIBackend;

    class UIFonts
    {
    public:
        UIFonts();
        ~UIFonts();

        UIFonts(const UIFonts &) = delete;
        UIFonts &operator=(const UIFonts &) = delete;
        UIFonts(UIFonts &&) = delete;
        UIFonts &operator=(UIFonts &&) = delete;

        // a font file the cooker kept as raw bytes, once per id, one that does not load is reported once and
        // is the default font
        UIFont Load(AssetCache &assets, AssetId id);

        // the default font, logged, when the bytes are not a whole TrueType or OpenType file, or the installed
        // backend cannot make a font of them. Added before a backend, a font it cannot make draws in the default
        UIFont Add(std::span<const uint8_t> bytes);

        size_t Count() const { return mFonts.size(); }

        // the backend's font, or null: the default font, no backend, or it could not make one
        void *Find(UIFont font) const;

        // as UITextures::SetBackend
        void SetBackend(IUIBackend *backend);

    private:
        struct Font
        {
            std::vector<uint8_t> bytes;
            void *backend = nullptr;
            void (*release)(void *) = nullptr;
        };

        void Make(Font &font);

        // [id - 1]
        std::vector<Font> mFonts;
        IUIBackend *mBackend = nullptr;

        std::unordered_map<uint64_t, UIFont> mLoaded;
    };
} // namespace mir
