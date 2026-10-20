/**
 * @file PostEffectRegistry.h
 * @author Rahul Nair
 * @brief Name -> params-UI callback, for the editor's Post-Processing panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace mir
{
    struct PostEffectContext
    {
        bool rayTracing = false;
        bool ngxSupported = false;
    };

    struct PostEffectDesc
    {
        std::string_view mName;

        void (*mDrawParams)(nlohmann::json &params, const PostEffectContext &context) = nullptr;
    };

    class PostEffectRegistry
    {
    public:
        static PostEffectRegistry &Instance();

        void Register(std::string_view name,
            void (*drawParams)(nlohmann::json &, const PostEffectContext &) = nullptr);

        std::span<const PostEffectDesc> All() const { return mEffects; }

    private:
        std::vector<PostEffectDesc> mEffects;
    };
}
