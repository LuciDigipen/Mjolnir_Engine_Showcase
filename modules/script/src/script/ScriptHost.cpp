/**
 * @file ScriptHost.cpp
 * @author Rahul Nair
 * @brief Lua-backed IScriptHost
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <script/ScriptHost.h>

#include "bindings/EntityBindings.h"
#include "bindings/WorldBindings.h"

#include <script/BindingApi.h>

#include <core/RegistrationList.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <sol/sol.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        constexpr const char *kRunningRegistryKey = "mir.scriptHost";
    }

    struct ScriptHostImpl
    {
        ScriptHostImpl()
        {
            // how a binding, which has only the Lua state, finds the entity whose callback is running
            mLua.registry()[kRunningRegistryKey] = static_cast<void *>(this);

            mLua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

            // Lua's own print writes to stdout, which nothing here reads -
            // route script output through the same log every other subsystem
            // uses instead.
            mLua.set_function("print",
                              [](sol::variadic_args args)
                              {
                                  sol::state_view lua(args.lua_state());
                                  const sol::function tostring = lua["tostring"];

                                  std::string line;
                                  bool first = true;
                                  for (const auto arg : args)
                                  {
                                      if (!first)
                                          line += '\t';
                                      first = false;
                                      line += tostring(arg.get<sol::object>()).get<std::string>();
                                  }
                                  MIR_LOG_INFO("[lua] {}", line);
                              });

            // core's bindings; every library's own come from its bridge through AddBindings
            RegisterEntityBindings(mLua);
            RegisterWorldBindings(mLua);
        }

        // The entity whose callback is running now, null between callbacks.
        Entity mRunning = kNullEntity;

        struct ScriptInstance
        {
            std::string scriptName;
            sol::table data;
        };

        int32_t CreateInstance(std::string_view scriptName)
        {
            const auto it = mLoadedScripts.find(std::string(scriptName));
            if (it == mLoadedScripts.end() || !it->second.is<sol::table>())
            {
                MIR_LOG_ERROR("script: CreateInstance: '{}' is not a loaded script table", scriptName);
                ++mErrors;
                return -1;
            }

            const int32_t ref = mNextInstanceRef++;
            mInstances.emplace(
                ref, ScriptInstance{std::string(scriptName), NewInstanceData(it->second.as<sol::table>(), scriptName)});
            return ref;
        }

        sol::table NewInstanceData(const sol::table &scriptClass, std::string_view scriptName)
        {
            const sol::object ctor = scriptClass["NewInstanceData"];
            if (!ctor.is<sol::protected_function>())
                return mLua.create_table();

            const sol::protected_function_result result = ctor.as<sol::protected_function>()();
            if (!result.valid())
            {
                const sol::error err = result;
                MIR_LOG_ERROR("script: '{}'.NewInstanceData() failed: {}; using an empty instance", scriptName,
                              err.what());
                ++mErrors;
                return mLua.create_table();
            }

            const sol::object data = result.get<sol::object>();
            if (!data.is<sol::table>())
            {
                MIR_LOG_ERROR("script: '{}'.NewInstanceData() did not return a table; using an empty instance",
                              scriptName);
                ++mErrors;
                return mLua.create_table();
            }

            return data.as<sol::table>();
        }

        void DestroyInstance(int32_t instanceRef)
        {
            mInstances.erase(instanceRef);
        }

        // false only when the callback ran and failed
        template <typename... Args>
        bool CallCallback(int32_t instanceRef, const char *callbackName, Args &&...args)
        {
            const auto it = mInstances.find(instanceRef);
            if (it == mInstances.end())
                return true; // destroyed or never created - routine, not an error

            const auto scriptIt = mLoadedScripts.find(it->second.scriptName);
            if (scriptIt == mLoadedScripts.end() || !scriptIt->second.is<sol::table>())
                return true; // unloaded out from under this instance - routine, not an error

            const sol::object callback = scriptIt->second.as<sol::table>()[callbackName];
            if (!callback.is<sol::protected_function>())
                return true;

            const sol::protected_function fn = callback;
            try
            {
                const sol::protected_function_result result = fn(it->second.data, std::forward<Args>(args)...);
                if (!result.valid())
                {
                    const sol::error err = result;
                    MIR_LOG_ERROR("script: {} failed: {}", callbackName, err.what());
                    ++mErrors;
                    return false;
                }
                return true;
            }
            catch (...)
            {
                // the caller reports a C++ exception from a binding, it is still a script failure
                ++mErrors;
                throw;
            }
        }

        sol::state mLua;
        // replayed into any Lua state built later; mLua is built once today
        RegistrationList<LuaBindings> mBindings;
        std::unordered_map<std::string, sol::object> mLoadedScripts;
        std::unordered_map<int32_t, ScriptInstance> mInstances;
        int32_t mNextInstanceRef = 0;
        std::size_t mErrors = 0;
    };

    ScriptHost::ScriptHost() : mImpl(std::make_unique<ScriptHostImpl>())
    {
    }

    ScriptHost::~ScriptHost() = default;

    std::size_t ScriptHost::ErrorCount() const { return mImpl->mErrors; }

    Registration AddBindings(ScriptHost &host, LuaBindings bindings)
    {
        ScriptHostImpl &impl = host.Impl();
        bindings(impl.mLua);

        return impl.mBindings.Add(std::move(bindings));
    }

    bool ScriptHost::LoadScriptSource(std::string_view name, std::string_view source)
    {
        const sol::protected_function_result result =
            mImpl->mLua.safe_script(source, sol::script_pass_on_error, std::string(name));

        if (!result.valid())
        {
            const sol::error err = result;
            MIR_LOG_ERROR("script: failed to load '{}': {}", name, err.what());
            ++mImpl->mErrors;
            return false;
        }

        mImpl->mLoadedScripts[std::string(name)] = result.get<sol::object>();
        return true;
    }

    bool ScriptHost::ReloadScriptSource(std::string_view name, std::string_view source)
    {
        return LoadScriptSource(name, source);
    }

    void ScriptHost::UnloadScript(std::string_view name)
    {
        mImpl->mLoadedScripts.erase(std::string(name));
    }

    int32_t ScriptHost::CreateInstance(std::string_view scriptName)
    {
        return mImpl->CreateInstance(scriptName);
    }

    void ScriptHost::DestroyInstance(int32_t instanceRef)
    {
        mImpl->DestroyInstance(instanceRef);
    }

    std::size_t ScriptHost::InstanceCount() const
    {
        return mImpl->mInstances.size();
    }

    namespace
    {
        // Set for the length of one callback and put back after, so a callback inside a callback is safe.
        class RunningScope
        {
        public:
            RunningScope(ScriptHostImpl &impl, World &world, Entity entity)
                : mImpl(impl), mPrevious(impl.mRunning),
                  mSceneScope(world, world.EntityScene(entity) == kOwnedBySession ?
                      (world.PrimaryScene() == kOwnedBySession ? world.ActiveScene() : world.PrimaryScene()) :
                      world.EntityScene(entity))
            {
                mImpl.mRunning = entity;
            }

            ~RunningScope() { mImpl.mRunning = mPrevious; }

            RunningScope(const RunningScope &) = delete;
            RunningScope &operator=(const RunningScope &) = delete;

        private:
            ScriptHostImpl &mImpl;
            Entity mPrevious;
            World::SceneScope mSceneScope;
        };
    }

    Entity RunningScriptEntity(sol::state_view lua)
    {
        const sol::optional<void *> impl = lua.registry()[kRunningRegistryKey];
        return impl ? static_cast<ScriptHostImpl *>(*impl)->mRunning : kNullEntity;
    }

    sol::state_view LuaState(ScriptHost &host) { return host.Impl().mLua; }

    bool CallScriptCallback(ScriptHost &host, World &world, Entity entity, int32_t instanceRef, const char *name,
        const std::vector<sol::object> &args)
    {
        ScriptHostImpl &impl = host.Impl();
        const RunningScope running(impl, world, entity);
        try
        {
            return impl.CallCallback(instanceRef, name, std::ref(world), Entity{ entity }, sol::as_args(args));
        }
        catch (const std::exception &e)
        {
            MIR_LOG_ERROR("script: entity {} threw in {}: {}", entity.mIndex, name, e.what());
            return false;
        }
    }

    void ScriptHost::CallOnStart(World &world, CommandBuffer &, Entity entity, int32_t instanceRef)
    {
        const RunningScope running(*mImpl, world, entity);
        mImpl->CallCallback(instanceRef, "OnStart", std::ref(world), Entity{ entity });
    }

    void ScriptHost::CallOnUpdate(World &world, CommandBuffer &, Entity entity, int32_t instanceRef, float dt)
    {
        const RunningScope running(*mImpl, world, entity);
        mImpl->CallCallback(instanceRef, "OnUpdate", std::ref(world), Entity{ entity }, dt);
    }

    void ScriptHost::CallOnFixedUpdate(World &world, CommandBuffer &, Entity entity, int32_t instanceRef, float dt)
    {
        const RunningScope running(*mImpl, world, entity);
        mImpl->CallCallback(instanceRef, "OnFixedUpdate", std::ref(world), Entity{ entity }, dt);
    }

    void ScriptHost::CallOnStop(World &world, CommandBuffer &, Entity entity, int32_t instanceRef)
    {
        const RunningScope running(*mImpl, world, entity);
        mImpl->CallCallback(instanceRef, "OnStop", std::ref(world), Entity{ entity });
    }
}
