/**
 * @file AuthoredCommand.cpp
 * @author Dong Ta
 * @brief A change to authored entries across documents, written as one batch through App's backend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/Authoring.h>

#include <core/log/Log.h>

#include <algorithm>
#include <utility>

namespace mir
{
    namespace
    {
        bool SameKeys(std::span<const AuthoredEntry> a, std::span<const AuthoredEntry> b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                [](const AuthoredEntry &x, const AuthoredEntry &y) { return x.mKey == y.mKey; });
        }

        //presence counts as well as the value
        bool SameState(std::span<const AuthoredEntry> a, std::span<const AuthoredEntry> b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const AuthoredEntry &x, const AuthoredEntry &y)
                { return x.mKey == y.mKey && x.mValue == y.mValue; });
        }

        void AddDocument(std::vector<DocumentId> &out, DocumentId document)
        {
            if (std::find(out.begin(), out.end(), document) == out.end())
                out.push_back(document);
        }

        std::size_t SizeOf(const std::vector<AuthoredEntry> &entries)
        {
            std::size_t size = entries.size() * sizeof(AuthoredEntry);
            for (const AuthoredEntry &entry : entries)
            {
                size += entry.mKey.mObject.mAddress.size() * sizeof(uint64_t) + entry.mKey.mComponent.size() +
                    entry.mKey.mField.size() + (entry.mValue ? entry.mValue->size() : 0);
            }
            return size;
        }

        class AuthoredCommand final : public ICommand
        {
        public:
            AuthoredCommand(std::string name, std::vector<AuthoredEntry> changes,
                std::vector<AuthoredEntry> maintenance)
                : mName(std::move(name)), mAfter(std::move(changes)), mMaintenance(std::move(maintenance))
            {
            }

            bool Do(CommandContext &context) override
            {
                IAuthoringBackend *backend = context.mAuthoring;
                if (backend == nullptr)
                {
                    MIR_LOG_WARN("History: '{}' needs an authoring backend, and App has none", mName);
                    return false;
                }

                if (!mCaptured && !Capture(*backend))
                    return false;

                std::vector<AuthoredEntry> batch = mAfter;
                batch.insert(batch.end(), mMaintenance.begin(), mMaintenance.end());

                const AuthoringWrite write = backend->Write(batch);
                if (!write.mApplied)
                    return false;

                mMaintained.clear();
                for (const AuthoredEntry &entry : mMaintenance)
                    AddDocument(mMaintained, entry.mKey.mObject.mDocument);
                for (const DocumentId document : write.mMaintained)
                    AddDocument(mMaintained, document);
                return true;
            }

            //only this command's own entries, maintenance stays as Do left it
            bool Undo(CommandContext &context) override
            {
                return context.mAuthoring != nullptr && context.mAuthoring->Write(mBefore).mApplied;
            }

            std::size_t Size() const override
            {
                return sizeof(*this) + SizeOf(mBefore) + SizeOf(mAfter) + SizeOf(mMaintenance) + mName.size() +
                    mMaintained.size() * sizeof(DocumentId);
            }

            std::string Name() const override { return mName; }
            void CollectIds(std::vector<HistoryId> &) const override {}

            void CollectDocuments(std::vector<DocumentId> &out) const override
            {
                for (const AuthoredEntry &entry : mAfter)
                    AddDocument(out, entry.mKey.mObject.mDocument);
            }

            void CollectMaintained(std::vector<DocumentId> &out) const override
            {
                out.insert(out.end(), mMaintained.begin(), mMaintained.end());
            }

            
            bool TryMerge(const ICommand &next) override
            {
                const auto *other = dynamic_cast<const AuthoredCommand *>(&next);
                if (other == nullptr || !mMaintenance.empty() || !other->mMaintenance.empty() ||
                    !SameKeys(mAfter, other->mAfter))
                    return false;

                mAfter = other->mAfter;
                return true;
            }

        private:
            bool Capture(IAuthoringBackend &backend)
            {
                std::vector<AuthoredKey> keys;
                keys.reserve(mAfter.size());
                for (const AuthoredEntry &entry : mAfter)
                    keys.push_back(entry.mKey);

                //a missing target refuses the whole batch
                std::optional<std::vector<AuthoredEntry>> before = backend.Read(keys);
                if (!before || !SameKeys(*before, mAfter))
                    return false;

                //an edit that changes no presence and no value doesnt count
                if (mMaintenance.empty() && SameState(*before, mAfter))
                    return false;

                mBefore = std::move(*before);
                mCaptured = true;
                return true;
            }

            std::string mName;
            std::vector<AuthoredEntry> mBefore;
            std::vector<AuthoredEntry> mAfter;
            std::vector<AuthoredEntry> mMaintenance;
            std::vector<DocumentId> mMaintained;
            bool mCaptured = false;
        };
    }

    std::unique_ptr<ICommand> MakeAuthoredCommand(std::string name, std::vector<AuthoredEntry> changes,
        std::vector<AuthoredEntry> maintenance)
    {
        return std::make_unique<AuthoredCommand>(std::move(name), std::move(changes), std::move(maintenance));
    }
}
