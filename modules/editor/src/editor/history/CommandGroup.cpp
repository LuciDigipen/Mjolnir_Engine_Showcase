/**
 * @file CommandGroup.cpp
 * @author Quy Dong Ta
 * @brief Several commands as one undo step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/CommandGroup.h>

#include <algorithm>
#include <utility>

namespace mir
{
    namespace
    {
        void AppendUnique(std::vector<DocumentId> &out, const std::vector<DocumentId> &documents)
        {
            for (const DocumentId document : documents)
            {
                if (std::find(out.begin(), out.end(), document) == out.end())
                    out.push_back(document);
            }
        }

        class CommandGroup final : public ICommand
        {
        public:
            CommandGroup(std::string name, std::vector<std::unique_ptr<ICommand>> commands)
                : mName(std::move(name)), mCommands(std::move(commands)), mApplied(mCommands.size(), false)
            {
            }

            bool Do(CommandContext &context) override
            {
                bool changed = false;
                for (std::size_t i = 0; i < mCommands.size(); ++i)
                {
                    mApplied[i] = mCommands[i]->Do(context);
                    changed |= mApplied[i];
                }
                return changed;
            }

            bool Undo(CommandContext &context) override
            {
                bool changed = false;
                for (std::size_t i = mCommands.size(); i-- > 0;)
                {
                    if (mApplied[i])
                        changed |= mCommands[i]->Undo(context);
                }
                return changed;
            }

            std::size_t Size() const override
            {
                std::size_t size = sizeof(*this) + mName.size();
                for (const auto &command : mCommands)
                    size += command->Size();
                return size;
            }

            std::string Name() const override { return mName; }

            //each applied command of next merges into one here, or joins the group after it
            bool TryMerge(ICommand &next) override
            {
                auto *other = dynamic_cast<CommandGroup *>(&next);
                if (other == nullptr || other->mName != mName)
                    return false;

                for (std::size_t i = 0; i < other->mCommands.size(); ++i)
                {
                    if (!other->mApplied[i])
                        continue;

                    bool merged = false;
                    for (std::size_t j = 0; j < mCommands.size() && !merged; ++j)
                        merged = mApplied[j] && mCommands[j]->TryMerge(*other->mCommands[i]);

                    if (!merged)
                    {
                        mCommands.push_back(std::move(other->mCommands[i]));
                        mApplied.push_back(true);
                    }
                }
                return true;
            }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                for (const auto &command : mCommands)
                    command->CollectIds(out);
            }

            void CollectDocuments(std::vector<DocumentId> &out) const override
            {
                for (const auto &command : mCommands)
                {
                    std::vector<DocumentId> documents;
                    command->CollectDocuments(documents);
                    AppendUnique(out, documents);
                }
            }

            void CollectMaintained(std::vector<DocumentId> &out) const override
            {
                for (std::size_t i = 0; i < mCommands.size(); ++i)
                {
                    if (!mApplied[i])
                        continue;
                    std::vector<DocumentId> documents;
                    mCommands[i]->CollectMaintained(documents);
                    AppendUnique(out, documents);
                }
            }

        private:
            std::string mName;
            std::vector<std::unique_ptr<ICommand>> mCommands;
            std::vector<bool> mApplied;
        };
    }

    std::unique_ptr<ICommand> MakeCommandGroup(std::string name, std::vector<std::unique_ptr<ICommand>> commands)
    {
        return std::make_unique<CommandGroup>(std::move(name), std::move(commands));
    }
}
