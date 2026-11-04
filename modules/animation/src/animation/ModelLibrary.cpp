/**
 * @file ModelLibrary.cpp
 * @author sumin.park
 * @brief Shared model entries, advanced once for every World that holds them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "animation/ModelLibrary.h"

#include <renderer/AssetUploadCache.h>

#include <tuple>

namespace mir
{
    ModelLibrary::Claim ModelLibrary::Acquire(AssetId id, AsyncAssetService *assets, const std::string &label)
    {
        std::erase_if(mEntries, [](const auto &item) { return item.second.expired(); });

        // Texture changes must also create a new prepared model.
        const uint64_t content = UploadContent(assets, id, ModelIdentityTags());
        const Key key{ id.value, content, assets };
        const uint32_t revision = assets != nullptr ? assets->Revision() : 0;
        if (const auto found = mEntries.find(key); found != mEntries.end())
            if (Claim claim = found->second.lock())
            {
                // A failure, or a model that shows an error appearance, is remembered while the catalog stays the
                // same so nothing retries in a loop. A reload allows another attempt, the files may be the same.
                const bool unsettled = !claim->failure.empty() || (claim->ready && claim->ready->degraded);
                if (!unsettled || claim->revision == revision)
                    return claim;
            }

        auto entry = std::make_shared<Entry>();
        entry->asset = id;
        entry->content = content;
        entry->revision = revision;
        entry->pending = std::make_unique<ModelPreparation>(mHost, mTextures, mErrorTexture, assets, id, label);
        mEntries[key] = entry;
        return entry;
    }

    void ModelLibrary::Advance()
    {
        for (auto &[key, weak] : mEntries)
        {
            const Claim entry = weak.lock();
            if (!entry || !entry->pending)
                continue;
            const ModelPreparation::State state = entry->pending->Advance();
            if (state == ModelPreparation::State::Ready)
            {
                entry->ready = std::shared_ptr<const PreparedModel>(entry->pending->Take());
                entry->pending.reset();
            }
            else if (state == ModelPreparation::State::Failed || state == ModelPreparation::State::Superseded)
            {
                // Reload hooks request a replacement for each World still using this model.
                entry->failure = entry->pending->Error();
                entry->pending.reset();
            }
        }
    }

    size_t ModelLibrary::LiveEntries() const
    {
        size_t count = 0;
        for (const auto &[key, weak] : mEntries)
            count += weak.expired() ? 0 : 1;
        return count;
    }

    void ModelLibrary::ReleaseAll()
    {
        for (auto &[key, weak] : mEntries)
            if (const Claim entry = weak.lock())
            {
                entry->pending.reset();
                entry->ready.reset();
                entry->failure = "the renderer released its resources";
            }
        mEntries.clear();
    }
}
