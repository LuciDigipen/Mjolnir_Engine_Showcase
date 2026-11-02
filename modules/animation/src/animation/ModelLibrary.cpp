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

        const uint64_t content = UploadContent(assets, id, ModelResourcesDecoder().dependencyTags);
        const Key key{ id.value, content, assets };
        if (const auto found = mEntries.find(key); found != mEntries.end())
            if (Claim claim = found->second.lock(); claim && claim->failure.empty())
                return claim;

        auto entry = std::make_shared<Entry>();
        entry->asset = id;
        entry->content = content;
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
            else if (state == ModelPreparation::State::Failed)
            {
                entry->failure = entry->pending->Error();
                entry->pending.reset();
            }
        }
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
