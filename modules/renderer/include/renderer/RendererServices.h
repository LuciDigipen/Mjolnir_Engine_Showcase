/**
 * @file RendererServices.h
 * @author sumin.park
 * @brief Renderer-wide objects that modules above the renderer share between Worlds.
 *
 * The renderer owns the slot and knows nothing of the type. A service releases what it holds when the
 * renderer destroys the resources it refers to, so no handle outlives its resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>

namespace mir
{
    class IRendererService
    {
    public:
        virtual ~IRendererService() = default;
        // Drops every claim and handle, the renderer is about to destroy the resources they name.
        virtual void ReleaseAll() = 0;
    };

    class RendererServices
    {
    public:
        // The one service of type T, made by make on first use. make returns a shared_ptr<T>.
        template <class T, class Make>
        std::shared_ptr<T> GetOrCreate(Make &&make)
        {
            std::shared_ptr<IRendererService> &slot = mServices[typeid(T)];
            if (!slot)
                slot = std::forward<Make>(make)();
            return std::static_pointer_cast<T>(slot);
        }

        void ReleaseAll()
        {
            for (auto &[type, service] : mServices)
                service->ReleaseAll();
        }

        // Releases and forgets every service, for the renderer's shutdown.
        void Clear()
        {
            ReleaseAll();
            mServices.clear();
        }

    private:
        std::unordered_map<std::type_index, std::shared_ptr<IRendererService>> mServices;
    };
}
