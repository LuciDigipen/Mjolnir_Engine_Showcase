/**
 * @file AssetLease.h
 * @author sumin.park
 * @brief Independent consumer claims on shared decoded assets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>

namespace mir
{
    enum class AssetLoadState : uint8_t { Stale, Queued, Ready, Failed };

    namespace detail
    {
        // State is published on main, workers only return completion messages.
        struct AssetLoadRecord
        {
            explicit AssetLoadRecord(std::type_index valueType) : mType(valueType) {}
            std::type_index mType;
            AssetLoadState mState = AssetLoadState::Queued;
            std::shared_ptr<const void> mResult;
            std::string mError;
            std::size_t mConsumers = 0;
            bool mPinned = false;
            bool mDispatched = false;
            std::atomic<bool> mCancelled{ false };
            uint64_t mBytes = 0;
            uint64_t mLastUse = 0;
            // what the result is made from, zero when unknown, and the decoder that made it
            uint64_t mContent = 0;
            const void *mDescriptor = nullptr;
        };
    }

    class AsyncAssetService;

    // A main-thread consumer claim. Releasing it never waits or invalidates another consumer.
    template <class T>
    class AssetLease
    {
    public:
        AssetLease() = default;
        ~AssetLease() { Reset(); }
        AssetLease(const AssetLease &) = delete;
        AssetLease &operator=(const AssetLease &) = delete;
        AssetLease(AssetLease &&other) noexcept : mRecord(std::move(other.mRecord)) {}
        AssetLease &operator=(AssetLease &&other) noexcept
        {
            if (this != &other)
            {
                Reset();
                mRecord = std::move(other.mRecord);
            }
            return *this;
        }
        void Reset()
        {
            if (mRecord)
            {
                --mRecord->mConsumers;
                if (mRecord->mConsumers == 0 && !mRecord->mPinned && mRecord->mState == AssetLoadState::Queued)
                    mRecord->mCancelled = true;
                mRecord.reset();
            }
        }
        AssetLoadState State() const { return mRecord ? mRecord->mState : AssetLoadState::Stale; }
        std::shared_ptr<const T> Acquire() const
        {
            return State() == AssetLoadState::Ready && mRecord->mType == typeid(T)
                ? std::static_pointer_cast<const T>(mRecord->mResult) : nullptr;
        }
        const std::string &FailureReason() const
        {
            static const std::string empty;
            return State() == AssetLoadState::Failed ? mRecord->mError : empty;
        }

    private:
        friend class AsyncAssetService;
        explicit AssetLease(std::shared_ptr<detail::AssetLoadRecord> record) : mRecord(std::move(record))
        {
            ++mRecord->mConsumers;
        }
        std::shared_ptr<detail::AssetLoadRecord> mRecord;
    };
}

