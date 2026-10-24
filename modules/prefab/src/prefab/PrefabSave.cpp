/**
 * @file PrefabSave.cpp
 * @author Jackey Cen
 * @brief Plans and writes a Prefab or a scene's unsaved Prefab dependencies in order.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabSave.h"

#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        void AddError(std::vector<PrefabDiagnostic> &diagnostics, std::string message)
        {
            diagnostics.push_back({ PrefabDiagnosticSeverity::Error, {}, std::move(message) });
        }

        class PlanBuilder
        {
        public:
            PlanBuilder(const PrefabRegistry &registry, const PrefabUnsavedPredicate &unsaved)
                : mRegistry(registry), mUnsaved(unsaved)
            {
            }

            void Visit(PrefabId id, bool always)
            {
                if (mVisiting.contains(id))
                {
                    AddError(mPlan.mDiagnostics, "prefab " + ToString(id) + " is its own dependency");
                    return;
                }
                if (!mVisited.insert(id).second)
                    return;

                const PrefabDocument *document = mRegistry.Find(id);
                if (document == nullptr)
                {
                    if (always || mUnsaved(id))
                        AddError(mPlan.mDiagnostics, "prefab " + ToString(id) + " is not in the registry");
                    return;
                }

                mVisiting.insert(id);
                for (const PrefabNode &node : document->mNodes)
                {
                    if (node.mKind == PrefabNodeKind::Prefab)
                        Visit(node.mPrefab, false);
                }
                mVisiting.erase(id);

                if (!always && !mUnsaved(id))
                    return;

                const std::optional<std::filesystem::path> path = mRegistry.PathOf(id);
                if (!path)
                {
                    AddError(mPlan.mDiagnostics, "prefab " + ToString(id) + " has no file path yet");
                    return;
                }
                mPlan.mSteps.push_back(PrefabSaveStep{ id, *path });
            }

            PrefabSavePlan Take() { return std::move(mPlan); }

        private:
            const PrefabRegistry &mRegistry;
            const PrefabUnsavedPredicate &mUnsaved;
            std::set<PrefabId> mVisited;
            std::set<PrefabId> mVisiting;
            PrefabSavePlan mPlan;
        };
    }

    PrefabSavePlan PlanSavePrefab(const PrefabRegistry &registry, PrefabId id,
        const PrefabUnsavedPredicate &unsaved)
    {
        PlanBuilder builder(registry, unsaved);
        builder.Visit(id, true);
        return builder.Take();
    }

    PrefabSavePlan PlanSaveScene(const PrefabRegistry &registry, const LoadedScene &scene,
        const PrefabUnsavedPredicate &unsaved)
    {
        PlanBuilder builder(registry, unsaved);
        for (const auto &[root, instance] : scene.mInstances)
        {
            (void)root;
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (id)
                builder.Visit(*id, false);
        }
        return builder.Take();
    }

    PrefabSaveResult ExecuteSavePlan(PrefabRegistry &registry, const PrefabSavePlan &plan)
    {
        PrefabSaveResult result;
        result.mDiagnostics = plan.mDiagnostics;
        if (HasPrefabErrors(plan.mDiagnostics))
            return result;

        for (const PrefabSaveStep &step : plan.mSteps)
        {
            const PrefabDocument *document = registry.Find(step.mPrefab);
            if (document == nullptr)
            {
                AddError(result.mDiagnostics, "prefab " + ToString(step.mPrefab) + " is not in the registry");
                return result;
            }

            std::vector<PrefabDiagnostic> diagnostics;
            if (!WritePrefabFile(step.mPath, *document, &diagnostics))
            {
                if (diagnostics.empty())
                    AddError(result.mDiagnostics, "prefab " + ToString(step.mPrefab) + " could not be written");
                result.mDiagnostics.insert(result.mDiagnostics.end(),
                    std::make_move_iterator(diagnostics.begin()), std::make_move_iterator(diagnostics.end()));
                return result;
            }

            registry.MarkSaved(*document, step.mPath);
            result.mWritten.push_back(step.mPrefab);
        }

        result.mSucceeded = true;
        return result;
    }
}
