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

#include <flight/FlightInput.h>
#include <flight/components/Aircraft.h>

#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputState.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string>
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
    }

    FlightDebugModule::FlightDebugModule(ModuleContext context, ImGuiModule &imgui)
        : mAuthoringWorld(&context.world), mActiveWorld(&context.world)
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

    void FlightDebugModule::Draw()
    {
        if (const InputState *input = mActiveWorld->TryResource<InputState>())
        {
            if (input->JustPressed(kFlightDebugToggleAction))
                mVisible = !mVisible;
        }

        if (!FlightDebugWindowVisible(MIR_WITH_EDITOR != 0, mPlayWorldActive, mVisible))
            return;

        std::vector<Entity> aircraftEntities;
        mActiveWorld->ForEach<Aircraft>([&aircraftEntities](Entity entity, Aircraft &)
            { aircraftEntities.push_back(entity); });

        if (mSelectedAircraft.IsNull() ||
            std::find(aircraftEntities.begin(), aircraftEntities.end(), mSelectedAircraft) == aircraftEntities.end())
            mSelectedAircraft = aircraftEntities.empty() ? kNullEntity : aircraftEntities.front();

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Flight Debug", &open))
        {
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

                    ImGui::Text("Config: %s", aircraft->config[0] != '\0' ? aircraft->config : "(none)");
                    ImGui::Text("Load state: %s", config != nullptr ? "loaded" : "not loaded");

                    if (config != nullptr)
                    {
                        DrawTierCombo(*aircraft, *config);
                        ImGui::SeparatorText("Configuration");
                        DrawRows(BuildFlightDebugConfigRows(*config));
                    }

                    ImGui::SeparatorText("Telemetry");
                    DrawRows(BuildFlightDebugTelemetryRows(aircraft->runtime));

                    ImGui::SeparatorText("Forces and moments (body frame)");
                    ImGui::TextUnformatted("X nose, Y right, Z down");
                    DrawRows(BuildFlightDebugForceRows(aircraft->runtime));
                }
            }
        }
        ImGui::End();

        if (!open)
            mVisible = false;
    }
}
