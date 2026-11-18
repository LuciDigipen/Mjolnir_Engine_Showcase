/**
 * @file FlightDebugModule.cpp
 * @author Jackey Cen
 * @brief The Flight Debug window: a Tools drawer over the Play world's aircraft.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_debug/FlightDebugModule.h>

#include <flight_debug/FlightDebugRows.h>
#include <flight_debug/FlightDebugSaving.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftConfigStore.h>
#include <flight/FlightInput.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/log/Log.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputState.h>

#include <imgui.h>

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        std::string AircraftLabel(World &world, Entity entity)
        {
            const Name *name = world.GetComponent<Name>(entity);
            if (name != nullptr && name->mValue[0] != '\0')
                return name->mValue;
            return std::format("Entity {}", entity.mIndex);
        }

        void DrawRows(const std::vector<FlightDebugRow> &rows)
        {
            for (const FlightDebugRow &row : rows)
            {
                ImGui::TextUnformatted(row.label.c_str());
                ImGui::SameLine(190.0f);
                if (row.warning)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f), "%s", row.value.c_str());
                else
                    ImGui::TextUnformatted(row.value.c_str());
            }
        }

        void DrawAircraftList(World &world, std::vector<Entity> &aircraft, Entity &selected)
        {
            const std::string preview = selected.IsNull() ? "(none)" : AircraftLabel(world, selected);
            if (!ImGui::BeginCombo("Aircraft", preview.c_str()))
                return;

            for (const Entity entity : aircraft)
            {
                const std::string label = AircraftLabel(world, entity);
                if (ImGui::Selectable(label.c_str(), entity == selected))
                    selected = entity;
            }
            ImGui::EndCombo();
        }

        void DrawTierCombo(Aircraft &aircraft, const AircraftConfig &config)
        {
            std::array<std::string, std::size(kAerodynamicTierNames)> labels;
            std::array<const char *, std::size(kAerodynamicTierNames)> items;
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                const AerodynamicTier tier = static_cast<AerodynamicTier>(i);
                labels[i] = std::string(kAerodynamicTierNames[i]);
                if (!FlightDebugTierAvailable(config, tier))
                    labels[i] += " (missing)";
                items[i] = labels[i].c_str();
            }

            int tier = static_cast<int>(aircraft.tier);
            if (ImGui::Combo("Tier", &tier, items.data(), static_cast<int>(items.size())))
                SetFlightDebugTier(aircraft, &config, static_cast<AerodynamicTier>(tier));
        }

        // Field labels repeat across engines and gear, so each widget gets its own ID.
        void DrawEditFields(AircraftConfig &config, AircraftConfigStore &store, std::string_view path)
        {
            std::string group;
            for (const FlightDebugField &field : BuildFlightDebugEditableFields(config))
            {
                if (field.group != group)
                {
                    group = field.group;
                    ImGui::SeparatorText(group.c_str());
                }

                float value = field.get(config);
                if (field.degrees)
                    value = FlightDebugDegrees(value);

                // Groups carry the engine or gear index, so the same label stays unique across them.
                const std::string id = field.group + '/' + field.label;
                ImGui::PushID(id.c_str());
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::DragFloat(field.label.c_str(), &value, 0.01f))
                {
                    field.set(config, field.degrees ? glm::radians(value) : value);
                    store.MarkEdited(path);
                }
                ImGui::PopID();
            }
        }

        void DrawSaveControls(const AircraftConfig &config, AircraftConfigStore &store, std::string_view path,
            const std::string &sourceAssetsRoot, std::string &error)
        {
            const bool dirty = store.IsDirty(path);
            ImGui::BeginDisabled(!dirty || sourceAssetsRoot.empty());
            if (ImGui::Button("Save to source"))
            {
                if (SaveAircraftConfigToSource(config, sourceAssetsRoot, path, error))
                    store.ClearDirty(path);
                else
                    MIR_LOG_ERROR("Flight Debug: {}", error);
            }
            ImGui::EndDisabled();

            if (!dirty)
            {
                ImGui::SameLine();
                ImGui::TextUnformatted("no unsaved edits");
            }
            if (!error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", error.c_str());
        }
    }

    FlightDebugModule::FlightDebugModule(ModuleContext context, ImGuiModule &imgui, std::string sourceAssetsRoot)
        : mAuthoringWorld(&context.world), mActiveWorld(&context.world),
          mSourceAssetsRoot(std::move(sourceAssetsRoot))
    {
        mDrawer = imgui.AddDrawer(DrawerGroup::Tools, [this] { Draw(); });
    }

    FlightDebugModule::~FlightDebugModule() { mDrawer.Reset(); }

    void FlightDebugModule::ActivateWorld(World *world)
    {
        mActiveWorld = world != nullptr ? world : mAuthoringWorld;
        mPlayWorldActive = world != nullptr;
        mSelectedAircraft = kNullEntity;
    }

    void FlightDebugModule::CollectOverwrittenEdits()
    {
        const AircraftConfigStoreRef *ref = mActiveWorld->TryResource<AircraftConfigStoreRef>();
        if (ref == nullptr || ref->store == nullptr)
            return;
        for (std::string &path : ref->store->TakeOverwrittenEdits())
            mOverwrittenEdits.push_back(std::move(path));
    }

    void FlightDebugModule::Draw()
    {
        if (const InputState *input = mActiveWorld->TryResource<InputState>())
        {
            if (input->JustPressed(kFlightDebugToggleAction))
                mVisible = !mVisible;
        }

        CollectOverwrittenEdits();

        if (!FlightDebugWindowVisible(MIR_WITH_EDITOR != 0, mPlayWorldActive, mVisible))
            return;

        std::vector<Entity> aircraftEntities;
        mActiveWorld->ForEach<Aircraft>([&aircraftEntities](Entity entity, Aircraft &)
            { aircraftEntities.push_back(entity); });

        if (mSelectedAircraft.IsNull() ||
            std::find(aircraftEntities.begin(), aircraftEntities.end(), mSelectedAircraft) == aircraftEntities.end())
            mSelectedAircraft = aircraftEntities.empty() ? kNullEntity : aircraftEntities.front();

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(460.0f, 760.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Flight Debug", &open))
        {
            for (const std::string &path : mOverwrittenEdits)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Reload overwrote unsaved edits in '%s'.",
                    path.c_str());
            if (!mOverwrittenEdits.empty() && ImGui::SmallButton("Dismiss"))
                mOverwrittenEdits.clear();

            if (aircraftEntities.empty())
            {
                ImGui::TextUnformatted("No aircraft in this world.");
            }
            else
            {
                DrawAircraftList(*mActiveWorld, aircraftEntities, mSelectedAircraft);

                Aircraft *aircraft = mActiveWorld->GetComponent<Aircraft>(mSelectedAircraft);
                if (aircraft != nullptr)
                {
                    const AircraftConfig *config = aircraft->runtime.config;
                    const AircraftConfigStoreRef *ref = mActiveWorld->TryResource<AircraftConfigStoreRef>();
                    AircraftConfigStore *store = ref != nullptr ? ref->store : nullptr;
                    const std::string_view path(aircraft->config);
                    const bool dirty = store != nullptr && !path.empty() && store->IsDirty(path);

                    ImGui::Text("Config: %s%s", aircraft->config[0] != '\0' ? aircraft->config : "(none)",
                        dirty ? " * unsaved" : "");
                    ImGui::Text("Load state: %s", config != nullptr ? "loaded" : "not loaded");

                    if (config != nullptr)
                    {
                        DrawTierCombo(*aircraft, *config);
                        ImGui::SeparatorText("Configuration");
                        DrawRows(BuildFlightDebugConfigRows(*config));
                    }

                    AircraftConfig *editable = store != nullptr && !path.empty() ? store->Edit(path) : nullptr;
                    if (editable != nullptr)
                    {
                        ImGui::SeparatorText("Edit");
                        DrawEditFields(*editable, *store, path);

                        ImGui::SeparatorText("Aero L3 generation");
                        ImGui::BeginDisabled(!editable->aero.l2.has_value());
                        if (ImGui::Button("Generate L3 from L2") && editable->aero.l2.has_value())
                        {
                            editable->aero.l3 = GenerateL3FromL2(*editable->aero.l2, *editable,
                                DefaultL3Grid(*editable), DefaultL3Deflections(*editable));
                            store->MarkEdited(path);
                        }
                        ImGui::EndDisabled();
                        if (!editable->aero.l2.has_value())
                            ImGui::TextUnformatted("needs aero.L2");

                        DrawSaveControls(*editable, *store, path, mSourceAssetsRoot, mSaveError);

                        ImGui::SeparatorText("Curves and tables (read-only)");
                        DrawRows(BuildFlightDebugTableRows(*editable));
                    }

                    ImGui::SeparatorText("Telemetry");
                    DrawRows(BuildFlightDebugTelemetryRows(aircraft->runtime));

                    ImGui::SeparatorText("Forces and moments (body frame)");
                    ImGui::TextUnformatted("X nose, Y right, Z down");
                    DrawRows(BuildFlightDebugForceRows(aircraft->runtime));

                    if (config != nullptr && config->aero.l3.has_value())
                    {
                        ImGui::SeparatorText("Aero L3");
                        DrawRows(BuildFlightDebugL3Rows(*config, aircraft->runtime));
                    }
                }
            }
        }
        ImGui::End();

        if (!open)
            mVisible = false;
    }
}
