/**
 * @file Registration.h
 * @author sumin.park
 * @brief Move-only token that undoes a registration when it dies
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/log/Assert.h"

#include <functional>
#include <memory>
#include <utility>

namespace mir
{
    // Returned by every Add* that registers a callback. A module keeps its tokens as its
    // last-declared members: they die first, so nothing calls into members already destroyed
    class Registration
    {
    public:
        Registration() = default;

        // owner expires when the registry dies; release undoes the registration
        Registration(std::weak_ptr<const void> owner, std::function<void()> release)
            : mOwner(std::move(owner)), mRelease(std::move(release))
        {
        }

        ~Registration() { Reset(); }

        Registration(const Registration &) = delete;
        Registration &operator=(const Registration &) = delete;

        Registration(Registration &&other) noexcept
            : mOwner(std::move(other.mOwner)), mRelease(std::exchange(other.mRelease, nullptr))
        {
        }

        Registration &operator=(Registration &&other) noexcept
        {
            if (this != &other)
            {
                Reset();
                mOwner = std::move(other.mOwner);
                mRelease = std::exchange(other.mRelease, nullptr);
            }
            return *this;
        }

        // unregisters now; a moved-from or reset token does nothing
        void Reset()
        {
            if (!mRelease)
                return;

            const bool ownerAlive = !mOwner.expired();
            MIR_ASSERT(ownerAlive, "Registration: the registry died before this token");
            if (ownerAlive)
                mRelease();
            mRelease = nullptr;
        }

        bool Active() const { return static_cast<bool>(mRelease); }

    private:
        std::weak_ptr<const void> mOwner;
        std::function<void()> mRelease;
    };
}
