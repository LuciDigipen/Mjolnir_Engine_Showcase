/**
 * @file ShaderReload.cpp
 * @author Jackey Cen
 * @brief All-or-none shader reload stages and coverage validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ShaderReload.h>

#include <algorithm>

namespace mir
{
    ShaderCompatibility ValidateShaderOwnerCoverage(const std::vector<std::string> &managed,
        const std::map<std::string, uint64_t> &accepted, const std::map<std::string, uint64_t> &candidate)
    {
        ShaderCompatibility result;
        result.compatible = true;
        const auto reject = [&](const std::string &name)
        {
            result.compatible = false;
            result.restartRequired = true;
            result.differences.push_back("restart required: shader output has no transactional reload owner: " + name);
        };
        for (const auto &[name, hash] : candidate)
        {
            if (std::find(managed.begin(), managed.end(), name) != managed.end())
                continue;
            const auto old = accepted.find(name);
            if (old == accepted.end() || old->second != hash)
                reject(name);
        }
        for (const auto &[name, hash] : accepted)
            if (!candidate.contains(name) && std::find(managed.begin(), managed.end(), name) == managed.end())
                reject(name);
        return result;
    }

    void ShaderReloadTransaction::AddStage(std::string name, Prepare prepare, Publish publish, Discard discard)
    {
        if (prepare == nullptr || publish == nullptr)
            return;
        mNames.push_back(name);
        mStages.push_back(Stage{ std::move(name), std::move(prepare), std::move(publish), std::move(discard) });
    }

    bool ShaderReloadTransaction::PrepareAll(std::string *error)
    {
        DiscardAll();
        mPrepared = 0;
        mCommitted = false;
        for (Stage &stage : mStages)
        {
            std::string stageError;
            bool prepared = false;
            try
            {
                prepared = stage.prepare(stageError);
            }
            catch (const std::exception &exception)
            {
                stageError = exception.what();
            }
            catch (...)
            {
                stageError = "unknown prepare failure";
            }
            if (!prepared)
            {
                if (stage.discard != nullptr)
                {
                    try
                    {
                        stage.discard();
                    }
                    catch (...)
                    {
                    }
                }
                if (error != nullptr)
                    *error = stage.name + ": " + (stageError.empty() ? "prepare failed" : stageError);
                DiscardAll();
                return false;
            }
            ++mPrepared;
        }
        return true;
    }

    void ShaderReloadTransaction::CommitAll()
    {
        if (!IsPrepared() || mCommitted)
            return;
        for (Stage &stage : mStages)
            stage.publish();
        mCommitted = true;
        mStages.clear();
        mNames.clear();
        mPrepared = 0;
    }

    void ShaderReloadTransaction::DiscardAll()
    {
        for (std::size_t i = 0; i < mPrepared && i < mStages.size(); ++i)
        {
            if (mStages[i].discard == nullptr)
                continue;
            try
            {
                mStages[i].discard();
            }
            catch (...)
            {
            }
        }
        mPrepared = 0;
        mCommitted = false;
    }

    const std::vector<std::string> &ShaderReloadTransaction::StageNames() const
    {
        return mNames;
    }

    void ShaderReloadTransaction::Clear()
    {
        DiscardAll();
        mStages.clear();
        mNames.clear();
    }

    void ShaderLibrary::Register(std::string name, std::vector<std::string> outputs, std::vector<std::string> stages)
    {
        for (ShaderLibraryConsumer &consumer : mConsumers)
        {
            if (consumer.name != name)
                continue;
            for (std::string &output : outputs)
            {
                if (std::find(consumer.outputs.begin(), consumer.outputs.end(), output) == consumer.outputs.end())
                    consumer.outputs.push_back(std::move(output));
            }
            for (std::string &stage : stages)
            {
                if (std::find(consumer.stages.begin(), consumer.stages.end(), stage) == consumer.stages.end())
                    consumer.stages.push_back(std::move(stage));
            }
            return;
        }
        mConsumers.push_back(ShaderLibraryConsumer{ std::move(name), std::move(outputs), std::move(stages) });
    }

    bool ShaderLibrary::ValidateCoverage(const ShaderManifest &manifest, std::string *error) const
    {
        if (mConsumers.empty())
        {
            if (error != nullptr)
                *error = "shader library has no registered consumers";
            return false;
        }
        for (const ShaderLibraryConsumer &consumer : mConsumers)
        {
            if (consumer.outputs.empty())
            {
                if (error != nullptr)
                    *error = "shader consumer '" + consumer.name + "' names no output";
                return false;
            }
            for (const std::string &outputName : consumer.outputs)
            {
                const ShaderManifestOutput *output = manifest.Find(outputName);
                if (output == nullptr)
                {
                    if (error != nullptr)
                        *error = "shader consumer '" + consumer.name + "' is missing manifest output '" +
                            outputName + "'";
                    return false;
                }
                if (!consumer.stages.empty() &&
                    std::find(consumer.stages.begin(), consumer.stages.end(), output->stage) == consumer.stages.end())
                {
                    if (error != nullptr)
                        *error = "manifest output '" + outputName + "' has stage '" + output->stage +
                            "' but consumer '" + consumer.name + "' expects a different stage";
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<std::string> ShaderLibrary::Outputs() const
    {
        std::vector<std::string> outputs;
        for (const ShaderLibraryConsumer &consumer : mConsumers)
            for (const std::string &output : consumer.outputs)
                if (std::find(outputs.begin(), outputs.end(), output) == outputs.end())
                    outputs.push_back(output);
        return outputs;
    }

    void ShaderLibrary::Clear()
    {
        mConsumers.clear();
    }
}
