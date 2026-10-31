/**
 * @file ParticleEditorPanel.cpp
 * @author sumin.park
 * @brief Edits particle assets and previews cooked systems through the production runtime.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleEditorPanel.h"

#include <editor/AssetImportManager.h>
#include <editor/EditorCamera.h>

#include <assets/AssetPath.h>
#include <assets/AssetCache.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/FileIO.h>
#include <editor/Selection.h>
#include <particles/ParticleStackEdit.h>
#include <particles/ParticleWorld.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <unordered_set>

namespace mir
{
    namespace
    {
        bool Text(const char *label, std::string &value)
        {
            char text[512]{};
            std::snprintf(text, sizeof(text), "%s", value.c_str());
            if (!ImGui::InputText(label, text, sizeof(text))) return false;
            value = text;
            return true;
        }

        Entity SelectedSystemEntity(World &world, const std::string &systemPath)
        {
            const auto *selection = world.TryResource<Selection>();
            if (selection == nullptr || selection->mSelected.size() != 1) return kNullEntity;
            const Entity entity = selection->mSelected.front();
            const auto *component = world.GetComponent<ParticleSystem>(entity);
            return component != nullptr && systemPath == component->systemName ? entity : kNullEntity;
        }
        bool Choice(const char *label, uint32_t &value, const char *choices)
        {
            int index = static_cast<int>(value);
            if (!ImGui::Combo(label, &index, choices)) return false;
            value = static_cast<uint32_t>(index);
            return true;
        }
        ParticleEmitterAsset EmitterTemplate(int type, AssetId texture)
        {
            ParticleEmitterAsset emitter;
            emitter.texture = texture;
            if (type == 2 || type == 4)
            {
                emitter.simulationTarget = ParticleSimulationTarget::CPU;
                emitter.spawnRate = 0;
                emitter.spawnPerMeter = 2;
                emitter.spawnBehaviors = { { "RibbonSpawn",{ 0.25f } } };
                emitter.updateBehaviors = { { "RibbonAge",{} },{ "RibbonWind",{} },
                    { "RibbonWidthCurve",{ 1, 1, 0 } },{ "RibbonFade",{} } };
                if (type == 4)
                {
                    emitter.spawnPerMeter = 1;
                    emitter.lifetime = 3;
                    emitter.size = 0.35f;
                    emitter.color = { 0.7f, 0.8f, 1.0f, 0.65f };
                }
            }
            else if (type == 3)
            {
                emitter.spawnRate = 90;
                emitter.lifetime = 0.65f;
                emitter.size = 0.12f;
                emitter.velocity = { 0, 0, -3 };
                emitter.updateBehaviors.push_back({ "FadeOverLife",{ 0 } });
            }
            else if (type == 0) emitter.spawnRate = 0;
            return emitter;
        }
    }

    ParticleEditorPanel::ParticleEditorPanel(std::filesystem::path sourceRoot, std::string idRoot)
        : mSourceRoot(std::move(sourceRoot)), mIdRoot(std::move(idRoot))
    {}

    bool ParticleEditorPanel::OpenSystem(const std::string &path, World &world)
    {
        if (mDirtySystem || mDirtyEmitter)
        {
            mError = "Save or discard particle drafts before opening another system.";
            return false;
        }
        try
        {
            OnHidden(world);
            LoadSystem(path);
            return true;
        }
        catch (const std::exception &error) { mError = error.what(); return false; }
    }

    void ParticleEditorPanel::PrepareNewSystem(const std::string &folder)
    {
        mNewSystemPath = folder.empty() ? "particles/new.particle.json" : folder + "/new.particle.json";
    }

    std::filesystem::path ParticleEditorPanel::SourceFile(std::string_view relative) const
    {
        const auto logical = MakeLogicalAssetPath(mIdRoot, relative);
        if (!logical.Ok() || relative.empty()) throw std::invalid_argument("asset path must stay inside this project's assets");
        const auto root = std::filesystem::weakly_canonical(mSourceRoot);
        const auto path = std::filesystem::weakly_canonical(root / std::filesystem::path(relative));
        auto a = root.begin(), b = path.begin();
        for (; a != root.end() && b != path.end() && *a == *b; ++a, ++b) {}
        if (a != root.end() || b == path.end()) throw std::invalid_argument("asset path resolves outside this project's assets");
        return path;
    }

    void ParticleEditorPanel::Scan()
    {
        mSystems.clear();
        mEmitters.clear();
        mTextures.clear();
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(mSourceRoot, std::filesystem::directory_options::skip_permission_denied, error), end;
            !error && it != end; it.increment(error))
        {
            if (it->is_regular_file() && it->path().filename().string().ends_with(".particle.json"))
                mSystems.push_back(std::filesystem::relative(it->path(), mSourceRoot).generic_string());
            if (it->is_regular_file() && it->path().filename().string().ends_with(".emitter.json"))
                mEmitters.push_back(std::filesystem::relative(it->path(), mSourceRoot).generic_string());
            if (it->is_regular_file())
            {
                const auto extension = it->path().extension().string();
                if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga")
                    mTextures.push_back(std::filesystem::relative(it->path(), mSourceRoot).generic_string());
            }
            if (mSystems.size() == 4096 || mEmitters.size() == 4096) break;
        }
        std::sort(mSystems.begin(), mSystems.end());
        std::sort(mEmitters.begin(), mEmitters.end());
        std::sort(mTextures.begin(), mTextures.end());
        mEmitterUsage.clear();
        for (const auto &path : mSystems)
            try
            {
                if (const auto source = ReadText(SourceFile(path)))
                {
                    const auto parsed = ReadParticleEffectSource(*source, mIdRoot);
                    if (!parsed.mEffect) continue;
                    std::unordered_set<std::string> seen;
                    for (const auto &reference : parsed.mEmitterPaths)
                        if (seen.insert(reference).second) ++mEmitterUsage[reference];
                }
            }
            catch (const std::exception &) { continue; }
            mScanned = true;
    }

    void ParticleEditorPanel::LoadSystem(const std::string &path)
    {
        const auto text = ReadText(SourceFile(path));
        if (!text) throw std::runtime_error("cannot read particle system source");
        auto result = ReadParticleEffectSource(*text, mIdRoot);
        if (!result.mEffect) throw std::invalid_argument(result.mError);
        if (result.mEmitterPaths.empty()) throw std::invalid_argument("particle system has no emitter placements");
        const auto emitterText = ReadText(SourceFile(result.mEmitterPaths.front()));
        if (!emitterText) throw std::runtime_error("cannot read the first emitter source");
        auto emitter = ReadParticleEmitterSource(*emitterText, mIdRoot);
        if (!emitter.mEmitter) throw std::invalid_argument(emitter.mError);
        mSystem = *result.mEffect; mReferences = result.mEmitterPaths; mSystemPath = path;
        mDirtySystem = false; mSelected = 0;
        mEmitter = std::move(*emitter.mEmitter);
        mEmitterPath = mReferences.front();
        mTexturePath = std::move(emitter.mTexturePath);
        mDirtyEmitter = false; mError.clear();
        mSavedEmitterSources[mEmitterPath] = WriteParticleEmitterSource(*mEmitter, mTexturePath);
        mSavedSystemSource = WriteParticleEffectSource(mSystem, mReferences);
        mUndoDrafts.clear(); mRedoDrafts.clear(); mGestureStart.reset();
    }

    void ParticleEditorPanel::LoadEmitter(size_t index)
    {
        if (index >= mReferences.size()) return;
        const auto text = ReadText(SourceFile(mReferences[index]));
        if (!text) throw std::runtime_error("cannot read emitter source, create the referenced emitter before loading it");
        auto result = ReadParticleEmitterSource(*text, mIdRoot);
        if (!result.mEmitter) throw std::invalid_argument(result.mError);
        mEmitter = *result.mEmitter; mTexturePath = result.mTexturePath;
        mSelected = index; mEmitterPath = mReferences[index]; mDirtyEmitter = false; mError.clear();
        mSavedEmitterSources[mEmitterPath] = WriteParticleEmitterSource(*mEmitter, mTexturePath);
    }

    ParticleEditorPanel::DraftSnapshot ParticleEditorPanel::CaptureDraft() const
    {
        DraftSnapshot draft;
        draft.system = mSystem;
        draft.references = mReferences;
        draft.emitter = mEmitter;
        draft.systemPath = mSystemPath;
        draft.emitterPath = mEmitterPath;
        draft.texturePath = mTexturePath;
        draft.selected = mSelected;
        if (!mSystemPath.empty()) draft.systemSource = WriteParticleEffectSource(mSystem, mReferences);
        if (mEmitter) draft.emitterSource = WriteParticleEmitterSource(*mEmitter, mTexturePath);
        return draft;
    }

    void ParticleEditorPanel::RestoreDraft(const DraftSnapshot &draft)
    {
        mSystem = draft.system;
        mReferences = draft.references;
        mEmitter = draft.emitter;
        mSystemPath = draft.systemPath;
        mEmitterPath = draft.emitterPath;
        mTexturePath = draft.texturePath;
        mSelected = draft.selected;
        mDirtySystem = draft.systemSource != mSavedSystemSource;
        const auto saved = mSavedEmitterSources.find(mEmitterPath);
        mDirtyEmitter = mEmitter && (saved == mSavedEmitterSources.end() ||
            draft.emitterSource != saved->second);
    }

    void ParticleEditorPanel::CommitDraftGesture()
    {
        if (!mGestureStart) return;
        const auto after = CaptureDraft();
        if (mGestureStart->systemSource != after.systemSource ||
            mGestureStart->emitterSource != after.emitterSource)
        {
            if (mUndoDrafts.size() == 128) mUndoDrafts.erase(mUndoDrafts.begin());
            mUndoDrafts.push_back(std::move(*mGestureStart));
            mRedoDrafts.clear();
        }
        mGestureStart.reset();
    }

    void ParticleEditorPanel::UndoDraft()
    {
        CommitDraftGesture();
        if (mUndoDrafts.empty()) return;
        mRedoDrafts.push_back(CaptureDraft());
        const auto previous = std::move(mUndoDrafts.back());
        mUndoDrafts.pop_back();
        RestoreDraft(previous);
        mHistoryApplied = true;
    }

    void ParticleEditorPanel::RedoDraft()
    {
        if (mRedoDrafts.empty()) return;
        mUndoDrafts.push_back(CaptureDraft());
        const auto next = std::move(mRedoDrafts.back());
        mRedoDrafts.pop_back();
        RestoreDraft(next);
        mHistoryApplied = true;
    }

    std::string ParticleEditorPanel::EnsureDefaultTexture(AssetImportManager &imports)
    {
        constexpr std::string_view relative = "particles/white.png";
        const auto path = SourceFile(relative);
        if (!std::filesystem::exists(path))
        {
            constexpr std::array<uint8_t, 70> white{
                0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
                0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,
                0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xff,0xff,0xff,0x7f,0x00,
                0x09,0xfb,0x03,0xfd,0x2a,0x86,0xe3,0x8a,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,
                0x42,0x60,0x82 };
            std::filesystem::create_directories(path.parent_path());
            if (!WriteBytesAtomic(path, std::as_bytes(std::span(white))))
                throw std::runtime_error("could not create the default particle texture");
            imports.QueueChanged(path);
        }
        return std::string(relative);
    }

    void ParticleEditorPanel::AttachEmitter(const std::string &path)
    {
        if (mReferences.size() >= kMaxSystemEmitters || !std::filesystem::is_regular_file(SourceFile(path)))
            throw std::invalid_argument("choose an existing emitter asset, maximum eight placements");
        auto draft = mSystem;
        const uint32_t last = *std::max_element(draft.placementIds.begin(), draft.placementIds.end());
        if (last == UINT32_MAX) throw std::runtime_error("particle placement IDs are exhausted");
        const uint32_t id = last + 1;
        draft.emitters.push_back(MakeAssetId(MakeLogicalAssetPath(mIdRoot, path).path));
        draft.placementIds.push_back(id);
        draft.placementOffsets.emplace_back(0.0f);
        std::string error;
        if (!ValidateParticleEffect(draft, error)) throw std::invalid_argument(error);
        mSystem = std::move(draft);
        mReferences.push_back(path);
        mDirtySystem = true;
    }

    void ParticleEditorPanel::CreateEmitter(AssetImportManager &imports)
    {
        if (mSystemPath.empty()) throw std::runtime_error("open a particle system before creating an emitter");
        if (!mNewEmitterPath.ends_with(".emitter.json") || std::filesystem::exists(SourceFile(mNewEmitterPath)))
            throw std::invalid_argument("choose an unused .emitter.json path");
        const std::string texture = EnsureDefaultTexture(imports);
        const auto emitter = EmitterTemplate(mNewEmitterTarget, MakeAssetId(MakeLogicalAssetPath(mIdRoot, texture).path));
        const auto source = WriteParticleEmitterSource(emitter, texture);
        const auto path = SourceFile(mNewEmitterPath);
        std::filesystem::create_directories(path.parent_path());
        if (!WriteTextAtomic(path, source)) throw std::runtime_error("could not create emitter source");
        try { AttachEmitter(mNewEmitterPath); }
        catch (...) { std::error_code ignored; std::filesystem::remove(path, ignored); throw; }
        imports.QueueChanged(path);
        Scan();
        LoadEmitter(mReferences.size() - 1);
    }

    void ParticleEditorPanel::CreateSystem(AssetImportManager &imports)
    {
        if (mNewSystemPath.size() >= kFieldStringCapacity)
            throw std::invalid_argument("system path is too long for a scene ParticleSystem component");
        if (!mNewSystemPath.ends_with(".particle.json") || std::filesystem::exists(SourceFile(mNewSystemPath)))
            throw std::invalid_argument("choose an unused .particle.json path");
        const auto emitterPath = mNewSystemPath.substr(0, mNewSystemPath.size() - std::string(".particle.json").size()) +
            ".emitter.json";
        if (std::filesystem::exists(SourceFile(emitterPath)))
            throw std::invalid_argument("the matching emitter asset already exists");
        const std::string texture = EnsureDefaultTexture(imports);
        const auto emitter = EmitterTemplate(mNewEmitterTarget, MakeAssetId(MakeLogicalAssetPath(mIdRoot, texture).path));
        ParticleEffect effect;
        effect.emitters = { MakeAssetId(MakeLogicalAssetPath(mIdRoot, emitterPath).path) };
        effect.placementIds = { 1 };
        effect.placementOffsets = { { 0, 0, 0 } };
        const std::vector<std::string> paths{ emitterPath };
        const auto emitterSource = WriteParticleEmitterSource(emitter, texture);
        const auto systemSource = WriteParticleEffectSource(effect, paths);
        const auto emitterFile = SourceFile(emitterPath), systemFile = SourceFile(mNewSystemPath);
        std::filesystem::create_directories(systemFile.parent_path());
        if (!WriteTextAtomic(emitterFile, emitterSource)) throw std::runtime_error("could not create emitter source");
        if (!WriteTextAtomic(systemFile, systemSource))
        {
            std::error_code ignored;
            std::filesystem::remove(emitterFile, ignored);
            throw std::runtime_error("could not create particle system source");
        }
        imports.QueueChanged(emitterFile);
        imports.QueueChanged(systemFile);
        Scan();
        LoadSystem(mNewSystemPath);
    }

    void ParticleEditorPanel::SaveSystem(AssetImportManager &imports)
    {
        mSystem.emitters.clear();
        for (const auto &path : mReferences)
        {
            if (!std::filesystem::is_regular_file(SourceFile(path)))
                throw std::invalid_argument("system references a missing emitter: " + path);
            mSystem.emitters.push_back(MakeAssetId(MakeLogicalAssetPath(mIdRoot, path).path));
        }
        std::string error;
        if (!ValidateParticleEffect(mSystem, error)) throw std::invalid_argument(error);
        const auto source = WriteParticleEffectSource(mSystem, mReferences);
        const auto path = SourceFile(mSystemPath);
        if (!WriteTextAtomic(path, source)) throw std::runtime_error("could not save system source");
        imports.QueueChanged(path);
        mSavedSystemSource = source;
        mDirtySystem = false;
        mError.clear();
    }

    void ParticleEditorPanel::SaveEmitter(AssetImportManager &imports)
    {
        if (!std::filesystem::is_regular_file(SourceFile(mTexturePath)))
            throw std::invalid_argument("choose an existing texture asset");
        mEmitter->texture = MakeAssetId(MakeLogicalAssetPath(mIdRoot, mTexturePath).path);
        std::string error;
        if (!ValidateParticleEmitterAsset(*mEmitter, error)) throw std::invalid_argument(error);
        const auto source = WriteParticleEmitterSource(*mEmitter, mTexturePath);
        const auto path = SourceFile(mEmitterPath);
        if (!WriteTextAtomic(path, source)) throw std::runtime_error("could not save emitter source");
        imports.QueueChanged(path);
        mSavedEmitterSources[mEmitterPath] = source;
        mDirtyEmitter = false;
        mError.clear();
    }

    void ParticleEditorPanel::DrawBehaviorControls(ParticleBehavior &behavior, const ParticleBehaviorInfo &info)
    {
        if (info.widget == ParticleBehaviorWidget::Gradient)
        {
            mDirtyEmitter |= ImGui::ColorEdit4("Start color", behavior.parameters.data(),
                ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            mDirtyEmitter |= ImGui::ColorEdit4("End color", behavior.parameters.data() + 4,
                ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 16));
            const ImU32 start = ImGui::ColorConvertFloat4ToU32(ImVec4(behavior.parameters[0],
                behavior.parameters[1], behavior.parameters[2], behavior.parameters[3]));
            const ImU32 end = ImGui::ColorConvertFloat4ToU32(ImVec4(behavior.parameters[4],
                behavior.parameters[5], behavior.parameters[6], behavior.parameters[7]));
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax(), start, end, end, start);
            return;
        }
        for (size_t p = 0; p < behavior.parameters.size(); ++p)
        {
            const auto &parameter = info.parameters.begin()[p];
            const std::string label(parameter.name);
            if (parameter.integer)
            {
                int value = static_cast<int>(behavior.parameters[p]);
                if (ImGui::DragInt(label.c_str(), &value, 1, static_cast<int>(parameter.min),
                    static_cast<int>(parameter.max), "%d", ImGuiSliderFlags_AlwaysClamp))
                {
                    behavior.parameters[p] = static_cast<float>(value);
                    mDirtyEmitter = true;
                }
            }
            else mDirtyEmitter |= ImGui::DragFloat(label.c_str(), &behavior.parameters[p], 0.02f,
                parameter.min, parameter.max, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (info.widget == ParticleBehaviorWidget::Curve)
        {
            float samples[64];
            for (int i = 0; i < 64; ++i)
                samples[i] = EvaluateParticleCurve(behavior.parameters[0], behavior.parameters[1],
                    behavior.parameters[2], i / 63.0f);
            ImGui::PlotLines("Normalized life", samples, 64, 0, nullptr, 0,
                *std::max_element(samples, samples + 64) + 0.1f, ImVec2(0, 65));
        }
    }

    void ParticleEditorPanel::DrawBehaviors()
    {
        auto &emitter = *mEmitter;
        const auto infos = ParticleBehaviors();
        const auto find = [&](const ParticleBehaviorInfo &info) -> std::pair<std::vector<ParticleBehavior> &, size_t>
            {
                auto &stack = info.spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
                const auto found = std::find_if(stack.begin(), stack.end(), [&](const ParticleBehavior &behavior)
                    {
                        return behavior.name == info.name;
                    });
                return { stack, static_cast<size_t>(found - stack.begin()) };
            };
        for (const char *section :{ "Emission", "Simulation", "Motion", "Appearance" })
        {
            if (!ImGui::CollapsingHeader(section, ImGuiTreeNodeFlags_DefaultOpen)) continue;
            for (const auto &info : infos)
            {
                if (info.target != emitter.simulationTarget || info.section != section) continue;
                ImGui::PushID(info.name.data());
                if (!info.group.empty())
                {
                    const auto prior = std::find_if(infos.begin(), infos.end(), [&](const ParticleBehaviorInfo &entry)
                        {
                            return entry.target == info.target && entry.group == info.group;
                        });
                    if (&*prior != &info) { ImGui::PopID(); continue; }
                    const ParticleBehaviorInfo *selected = nullptr;
                    for (const auto &choice : infos)
                        if (choice.target == info.target && choice.group == info.group)
                        {
                            const auto [stack, index] = find(choice);
                            if (index < stack.size() && stack[index].enabled) selected = &choice;
                        }
                    const std::string selectedLabel = selected ?
                        std::string(selected->label.empty() ? selected->name : selected->label) : "Off";
                    if (ImGui::BeginCombo(std::string(info.group).c_str(), selectedLabel.c_str()))
                    {
                        if (ImGui::Selectable("Off", !selected) && selected)
                        {
                            auto [stack, index] = find(*selected);
                            mDirtyEmitter |= SetParticleBehaviorEnabled(emitter, selected->spawn, index, false);
                        }
                        for (const auto &choice : infos)
                            if (choice.target == info.target && choice.group == info.group)
                            {
                                const std::string label(choice.label.empty() ? choice.name : choice.label);
                                if (ImGui::Selectable(label.c_str(), selected == &choice))
                                {
                                    auto [stack, index] = find(choice);
                                    mDirtyEmitter |= index < stack.size() ?
                                        SetParticleBehaviorEnabled(emitter, choice.spawn, index, true) :
                                        AddParticleBehavior(emitter, choice.spawn, choice.name);
                                }
                            }
                        ImGui::EndCombo();
                    }
                    if (selected)
                    {
                        auto [stack, index] = find(*selected);
                        if (index < stack.size() && stack[index].enabled) DrawBehaviorControls(stack[index], *selected);
                    }
                }
                else
                {
                    auto [stack, index] = find(info);
                    const std::string label(info.label.empty() ? info.name : info.label);
                    if (info.required) ImGui::TextDisabled("%s (required)", label.c_str());
                    else
                    {
                        bool enabled = index < stack.size() && stack[index].enabled;
                        if (ImGui::Checkbox(label.c_str(), &enabled))
                            mDirtyEmitter |= index < stack.size() ?
                            SetParticleBehaviorEnabled(emitter, info.spawn, index, enabled) :
                            AddParticleBehavior(emitter, info.spawn, info.name);
                    }
                    auto [current, currentIndex] = find(info);
                    if (currentIndex < current.size() && current[currentIndex].enabled)
                        DrawBehaviorControls(current[currentIndex], info);
                }
                ImGui::PopID();
            }
        }
    }

    void ParticleEditorPanel::DrawEmitter()
    {
        if (!mEmitter) return;
        auto &e = *mEmitter;
        ImGui::Separator(); ImGui::Text("Emitter: %s%s", mEmitterPath.c_str(), mDirtyEmitter ? " *" : "");
        if (const auto found = mEmitterUsage.find(mEmitterPath); found != mEmitterUsage.end() && found->second > 1)
            ImGui::TextDisabled("Shared by %zu systems. Changes to this asset affect all of them.", found->second);
        ImGui::TextUnformatted(e.simulationTarget == ParticleSimulationTarget::GPU ? "GPU sprite emitter" : "CPU ribbon emitter");
        if (ImGui::BeginCombo("Texture", mTexturePath.empty() ? "Select texture" : mTexturePath.c_str()))
        {
            for (const auto &path : mTextures)
                if (ImGui::Selectable(path.c_str(), path == mTexturePath))
                {
                    mTexturePath = path;
                    mDirtyEmitter = true;
                }
            ImGui::EndCombo();
        }
        mDirtyEmitter |= ImGui::DragFloat("Lifetime", &e.lifetime, 0.05f, 0.01f, 120, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
        int capacity = static_cast<int>(e.capacity);
        if (ImGui::DragInt("Capacity", &capacity, 1, 1, e.simulationTarget == ParticleSimulationTarget::CPU ? 1024 : 65536, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            e.capacity = static_cast<uint32_t>(capacity); mDirtyEmitter = true;
        }
        if (e.simulationTarget == ParticleSimulationTarget::GPU)
        {
            mDirtyEmitter |= ImGui::DragFloat("Births per second", &e.spawnRate, 1, 0, 65536);
            int burst = static_cast<int>(e.burst);
            if (ImGui::DragInt("Initial burst", &burst, 1, 0, 65536, "%d", ImGuiSliderFlags_AlwaysClamp)) { e.burst = burst;mDirtyEmitter = true; }
        }
        mDirtyEmitter |= ImGui::DragFloat("Births per metre", &e.spawnPerMeter, 0.05f, 0, 65536);
        mDirtyEmitter |= ImGui::DragFloat3("Attachment offset", &e.localOffset.x, 0.05f);
        mDirtyEmitter |= ImGui::DragFloat3("Local velocity", &e.velocity.x, 0.05f);
        mDirtyEmitter |= ImGui::DragFloat("Inherited velocity", &e.inheritVelocity, 0.02f, 0, 16);
        mDirtyEmitter |= ImGui::DragFloat("Teleport distance", &e.teleportDistance, 1, 0.01f, 65536);
        mDirtyEmitter |= ImGui::InputScalar("Seed", ImGuiDataType_U32, &e.seed);
        mDirtyEmitter |= ImGui::DragFloat("Size", &e.size, 0.01f, 0.001f, 64);
        mDirtyEmitter |= ImGui::ColorEdit4("Base color", &e.color.x, ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float);
        DrawBehaviors();
        if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &r = e.renderer;
            mDirtyEmitter |= Choice("Alignment", r.alignment, "Screen\0Velocity\0");
            mDirtyEmitter |= Choice("Facing", r.facing, "Camera plane\0Camera position\0");
            mDirtyEmitter |= Choice("Blend", r.blend, "Alpha\0Additive\0");
            mDirtyEmitter |= Choice("Sort", r.sort, "Back to front\0None\0");
            mDirtyEmitter |= ImGui::DragFloat("Soft intersection", &r.softDistance, 0.05f, 0, 1024);
            mDirtyEmitter |= ImGui::DragFloat("Draw distance", &r.maxDistance, 10, 1, 10000000);
            mDirtyEmitter |= ImGui::Checkbox("Sun and ambient lighting", &r.lit);
            mDirtyEmitter |= ImGui::Checkbox("Distance fog", &r.fog);
            mDirtyEmitter |= ImGui::Checkbox("Sample texture", &r.textured);
        }
        if (ImGui::CollapsingHeader("Importance and distance quality", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &q = e.quality;
            mDirtyEmitter |= ImGui::SliderFloat("Importance", &q.importance, 0, 1);
            mDirtyEmitter |= ImGui::DragFloat("Full quality distance", &q.nearDistance, 10, 0, 10000000);
            mDirtyEmitter |= ImGui::DragFloat("Minimum quality distance", &q.farDistance, 10, 1, 10000000);
            mDirtyEmitter |= ImGui::SliderFloat("Minimum birth scale", &q.minSpawnScale, 0, 1);
            ImGui::TextWrapped("Quality changes births. Existing particles and offscreen trails keep simulating.");
        }
    }

    void ParticleEditorPanel::OnHidden(World &world)
    {
        if (auto *particles = world.TryResource<ParticleWorld>(); particles && mPreviewing) particles->StopPreview();
        mPreviewing = false;
        mPreviewEntity = kNullEntity;
    }

    void ParticleEditorPanel::DrawDiagnostics(World &world)
    {
        auto *particles = world.TryResource<ParticleWorld>();
        if (!particles) return;
        const auto &s = particles->Runtime().Stats();
        const auto &limits = particles->Runtime().Budgets();
        if (!mBudgetsKnown) { mBudgetDraft = limits;mBudgetsKnown = true; }
        if (ImGui::CollapsingHeader("Session resource limits"))
        {
            ImGui::TextWrapped("Apply while no effects are alive. These session limits also apply during Play.");
            ImGui::BeginDisabled(s.active!=0 || !IsEditing(world));
            ImGui::InputScalar("Emitter limit", ImGuiDataType_U32, &mBudgetDraft.emitters);
            ImGui::InputScalar("GPU particle reservation limit", ImGuiDataType_U32, &mBudgetDraft.gpuParticles);
            ImGui::InputScalar("Ribbon reservation limit", ImGuiDataType_U32, &mBudgetDraft.ribbonPoints);
            ImGui::InputScalar("Birth work limit", ImGuiDataType_U32, &mBudgetDraft.births);
            float mib = mBudgetDraft.cpuBytes/1048576.0f;
            if (ImGui::SliderFloat("CPU reservation limit (MiB)", &mib, 0.0625f, 64)) mBudgetDraft.cpuBytes = uint64_t(mib*1048576);
            if (ImGui::Button("Apply limits") && !particles->Runtime().ConfigureBudgets(mBudgetDraft))
                mError = "Limits must be positive and within the compiled resource ceilings.";
            ImGui::SameLine();
            if (ImGui::Button("Default limits")) { mBudgetDraft = {};particles->Runtime().ConfigureBudgets(mBudgetDraft); }
            ImGui::EndDisabled();
        }
        ImGui::TextWrapped("Systems %u/%u, emitters %u/%u", s.active, particles->Runtime().Capacity(), s.activeEmitters, limits.emitters);
        ImGui::TextWrapped("GPU capacity %u/%u, ribbon samples %u (reserved %u/%u)", s.gpuCapacity, limits.gpuParticles, s.ribbonPoints, s.ribbonCapacity, limits.ribbonPoints);
        ImGui::TextWrapped("CPU reserved %.2f MiB (peak %.2f), limit %.2f MiB", s.cpuBytes/1048576.0, s.cpuHighWaterBytes/1048576.0, limits.cpuBytes/1048576.0);
        ImGui::TextWrapped("Birth budget %u, reduced emitters %u", limits.births, s.qualityReducedEmitters);
        ImGui::TextWrapped("Rejected starts %llu, memory %llu, dropped births %llu, evicted trail points %llu",
            s.rejectedStarts, s.rejectedMemoryStarts, s.droppedBudgetSpawns, s.evictedRibbonPoints);
        if (const auto *d = world.TryResource<ParticleDiagnostics>())
        {
            ImGui::TextWrapped("GPU alive %u, completed frame %llu", d->gpuAlive, d->gpuFrame);
            ImGui::TextWrapped("GPU owned %.2f MiB (peak %.2f), snapshot %.2f MiB, last uploads %.1f KiB",
                d->gpuBytes/1048576.0, d->gpuHighWaterBytes/1048576.0, d->snapshotBytes/1048576.0, d->uploadBytes/1024.0);
            ImGui::TextWrapped("Rejected GPU packets %llu, dropped GPU upload births %llu, last completed pool drops %llu",
                d->rejectedGpu, d->droppedGpuBirths, d->completedPoolDrops);
            ImGui::TextWrapped("Deferred textures %llu, dropped segments %llu", d->deferredTextures, d->droppedRibbonSegments);
            ImGui::TextWrapped("CPU simulation %.3f ms, submit %.3f ms, recording %.3f ms", s.cpuMs, d->cpuSubmitMs, d->cpuRecordMs);
            if (d->gpuTimingAvailable) ImGui::TextWrapped("GPU simulation %.3f ms, texture uploads %.3f ms, prepare %.3f ms, draw %.3f ms (asynchronous)",
                d->simulationGpuMs, d->uploadGpuMs, d->prepareGpuMs, d->drawGpuMs);
            else ImGui::TextUnformatted("GPU timing unavailable on this queue");
        }
    }

    void ParticleEditorPanel::SaveAll(AssetImportManager &imports)
    {
        CommitDraftGesture();
        if (mDirtyEmitter) SaveEmitter(imports);
        if (mDirtySystem) SaveSystem(imports);
    }

    void ParticleEditorPanel::DrawSelectBar(World &world, AssetImportManager &imports)
    {
        if (!mScanned) Scan();
        const ImGuiStyle &style = ImGui::GetStyle();
        const float buttons = ImGui::CalcTextSize("Refresh").x + ImGui::CalcTextSize("+ New").x +
            style.FramePadding.x * 4 + style.ItemSpacing.x * 2;
        std::string requestedSystem;
        ImGui::BeginDisabled(mDirtySystem || mDirtyEmitter);
        ImGui::SetNextItemWidth(-buttons);
        if (ImGui::BeginCombo("##System", mSystemPath.empty() ? "Select a particle system" : mSystemPath.c_str()))
        {
            for (const auto &path : mSystems)
                if (ImGui::Selectable(path.c_str(), path == mSystemPath)) requestedSystem = path;
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) Scan();
        ImGui::SameLine();
        if (ImGui::Button("+ New")) ImGui::OpenPopup("New particle system");
        if (ImGui::BeginPopup("New particle system"))
        {
            Text("Path", mNewSystemPath);
            ImGui::Combo("Template", &mNewEmitterTarget, "Blank\0Sprite\0Ribbon\0Exhaust\0Contrail\0");
            if (ImGui::Button("Create"))
            {
                try
                {
                    if (mDirtySystem || mDirtyEmitter) throw std::runtime_error("save or discard drafts before creating a system");
                    OnHidden(world);
                    CreateSystem(imports);
                    ImGui::CloseCurrentPopup();
                }
                catch (const std::bad_alloc &) { throw; }
                catch (const std::exception &error) { mError = error.what(); }
            }
            ImGui::EndPopup();
        }
        if (!requestedSystem.empty()) { OnHidden(world); LoadSystem(requestedSystem); }
    }

    void ParticleEditorPanel::DrawEditBar(AssetImportManager &imports)
    {
        ImGui::BeginDisabled(mUndoDrafts.empty() && !mGestureStart);
        if (ImGui::Button("Undo")) UndoDraft();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(mRedoDrafts.empty());
        if (ImGui::Button("Redo")) RedoDraft();
        ImGui::EndDisabled();
        const ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) UndoDraft();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y)) RedoDraft();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!mDirtySystem && !mDirtyEmitter);
        const bool save = ImGui::Button("Save");
        ImGui::SetItemTooltip("Writes the system and emitter sources and queues a recook.");
        ImGui::SameLine();
        const bool discard = ImGui::Button("Discard");
        ImGui::EndDisabled();
        if (save) SaveAll(imports);
        else if (discard && !mSystemPath.empty()) LoadSystem(mSystemPath);
    }

    void ParticleEditorPanel::DrawPreviewStrip(World &world, const EditorCamera &camera, float dt)
    {
        auto *particles = world.TryResource<ParticleWorld>();
        const bool editing = IsEditing(world);
        if (!editing) OnHidden(world);
        ImGui::BeginDisabled(!particles || !editing || mSystemPath.empty());
        if (ImGui::Button("Preview"))
        {
            OnHidden(world);mPreviewing = true;mPreviewPaused = false;mPreviewTime = 0;
            mPreviewAnchor = camera.Pose();
            mPreviewAnchor.linear = WorldMat3(1);
            mPreviewAnchor.position = *WorldPosition::FromMeters(camera.Position().Meters()+WorldVec3(camera.Forward())*WorldReal(6));
            if (const Entity entity = SelectedSystemEntity(world, mSystemPath); !entity.IsNull())
            {
                mPreviewEntity = entity;
                mPreviewAnchor = ResolveWorld(world, entity);
            }
        }
        ImGui::SetItemTooltip("Preview uses cooked assets, at the selected entity when it uses this system.\n"
            "Save recooks in Edit, Play waits until Stop.");
        ImGui::SameLine();
        if (ImGui::Button(mPreviewPaused ? "Resume" : "Pause"))
        {
            mPreviewPaused = !mPreviewPaused;if (particles) particles->Runtime().Pause(particles->PreviewHandle(), mPreviewPaused);
        }
        ImGui::SameLine(); if (ImGui::Button("Clear")) OnHidden(world);
        ImGui::SameLine(); ImGui::Checkbox("Move", &mMovePreview);
        ImGui::SetItemTooltip("Sweeps the preview attachment side to side.");
        ImGui::EndDisabled();
        if (!particles || !mPreviewing || !editing) return;
        if (!particles->Runtime().Find(particles->PreviewHandle()))
        {
            const auto logical = MakeLogicalAssetPath(mIdRoot, mSystemPath);
            const auto handle = particles->StartPreview(MakeAssetId(logical.path), mPreviewAnchor);
            if (handle.IsNull()) { mPreviewing = false;mError = "Cook this system and its dependencies before previewing."; }
            else particles->Runtime().Pause(handle, mPreviewPaused);
        }
        if (!mPreviewPaused)
        {
            mPreviewTime += std::min(dt, 0.067f);
            auto pose = mPreviewAnchor;
            if (!mPreviewEntity.IsNull() && world.IsAlive(mPreviewEntity))
                pose = ResolveWorld(world, mPreviewEntity);
            else if (mMovePreview)
                pose.position = *WorldPosition::FromMeters(pose.position.Meters()+WorldVec3(std::sin(mPreviewTime)*2, 0, 0));
            particles->Runtime().SetPose(particles->PreviewHandle(), pose);
        }
    }

    // Every action targets the selected emitter, so the strip needs no per-row buttons.
    void ParticleEditorPanel::DrawEmitterStrip(World &world, AssetImportManager &imports)
    {
        if (mSystemPath.empty()) return;
        enum class Action { None, Duplicate, Remove, Up, Down };
        const size_t count = mReferences.size();
        const bool hasSelection = mSelected < count;
        // Unmatched placement arrays can only be browsed, not restructured.
        const bool placed = mSystem.emitters.size() == count && mSystem.placementIds.size() == count &&
            mSystem.placementOffsets.size() == count;
        const bool full = count >= kMaxSystemEmitters;
        Action action = Action::None;
        std::optional<size_t> picked;
        ImGui::Separator();
        ImGui::Text("Emitters %zu/%u", count, kMaxSystemEmitters);
        ImGui::BeginDisabled(!placed || full);
        if (ImGui::SmallButton("New")) ImGui::OpenPopup("New emitter");
        ImGui::SetItemTooltip("Create a new emitter asset and add it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add")) ImGui::OpenPopup("Add existing emitter");
        ImGui::SetItemTooltip("Add an existing emitter asset.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::SmallButton("Copy")) action = Action::Duplicate;
        ImGui::SetItemTooltip("Place the selected emitter again, one metre along X.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!placed || !hasSelection || count < 2);
        if (ImGui::SmallButton("Remove")) action = Action::Remove;
        ImGui::SetItemTooltip("Remove the selected emitter from this system. The emitter asset stays.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!placed || !hasSelection || mSelected == 0);
        if (ImGui::SmallButton("Up")) action = Action::Up;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!placed || !hasSelection || mSelected + 1 >= count);
        if (ImGui::SmallButton("Down")) action = Action::Down;
        ImGui::EndDisabled();

        const ImGuiStyle &style = ImGui::GetStyle();
        const float rows = static_cast<float>(std::clamp<size_t>(count, 3, kMaxSystemEmitters));
        if (ImGui::BeginChild("##Emitters", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * rows + style.WindowPadding.y * 2),
            ImGuiChildFlags_Borders))
        {
            for (size_t i = 0; i < count; ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = std::to_string(i + 1) + "  " +
                    std::filesystem::path(mReferences[i]).filename().string() +
                    (i == mSelected && mDirtyEmitter ? " *" : "");
                if (ImGui::Selectable(label.c_str(), i == mSelected)) picked = i;
                ImGui::SetItemTooltip("%s", mReferences[i].c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (placed && hasSelection)
        {
            ImGui::TextDisabled("Origin relative to the scene entity");
            ImGui::SetNextItemWidth(-1);
            mDirtySystem |= ImGui::DragFloat3("##Origin", &mSystem.placementOffsets[mSelected].x, 0.05f,
                -4096, 4096, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        // Per-instance offsets live on the scene entity, so they only show for the selected entity playing this system.
        if (const Entity target = IsEditing(world) ? SelectedSystemEntity(world, mSystemPath) : kNullEntity;
            hasSelection && !target.IsNull())
        {
            glm::vec3 offset = world.GetComponent<ParticleSystem>(target)->emitterOffsets[mSelected];
            ImGui::TextDisabled("Offset on the selected entity");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##InstanceOffset", &offset.x, 0.05f, -4096, 4096, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                mOffsetEdit = InstanceOffsetEdit{ target, static_cast<uint32_t>(mSelected), offset };
        }

        // A throw between Begin and End would unbalance the popup stack, so failures are caught here.
        const auto attempt = [this](auto &&work)
            {
                try { work(); }
                catch (const std::bad_alloc &) { throw; }
                catch (const std::exception &error) { mError = error.what(); }
            };
        if (ImGui::BeginPopup("New emitter"))
        {
            Text("Path", mNewEmitterPath);
            ImGui::Combo("Template", &mNewEmitterTarget, "Blank\0Sprite\0Ribbon\0Exhaust\0Contrail\0");
            if (ImGui::Button("Create"))
                attempt([&] { CreateEmitter(imports); ImGui::CloseCurrentPopup(); });
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("Add existing emitter"))
        {
            for (const auto &path : mEmitters)
                if (ImGui::Selectable(path.c_str()))
                    attempt([&] { AttachEmitter(path); if (!mDirtyEmitter) LoadEmitter(mReferences.size() - 1); });
            ImGui::EndPopup();
        }

        if (action != Action::None && hasSelection)
        {
            const size_t i = mSelected;
            if (action == Action::Duplicate)
            {
                const auto offset = mSystem.placementOffsets[i];
                AttachEmitter(mReferences[i]);
                mSystem.placementOffsets.back() = offset + glm::vec3(1.0f, 0.0f, 0.0f);
                mSelected = mReferences.size() - 1;
            }
            else if (action == Action::Remove)
            {
                if (mDirtyEmitter) mError = "Save the emitter before removing it from the system.";
                else
                {
                    mReferences.erase(mReferences.begin() + i);
                    mSystem.emitters.erase(mSystem.emitters.begin() + i);
                    mSystem.placementIds.erase(mSystem.placementIds.begin() + i);
                    mSystem.placementOffsets.erase(mSystem.placementOffsets.begin() + i);
                    mDirtySystem = true;
                    LoadEmitter(std::min(i, mReferences.size() - 1));
                }
            }
            else
            {
                const size_t other = action == Action::Up ? i - 1 : i + 1;
                std::swap(mReferences[i], mReferences[other]);
                std::swap(mSystem.emitters[i], mSystem.emitters[other]);
                std::swap(mSystem.placementIds[i], mSystem.placementIds[other]);
                std::swap(mSystem.placementOffsets[i], mSystem.placementOffsets[other]);
                mSelected = other;
                mDirtySystem = true;
            }
        }
        else if (picked && *picked != mSelected)
        {
            if (mDirtyEmitter) mError = "Save the current emitter before selecting another one.";
            else { CommitDraftGesture(); LoadEmitter(*picked); }
        }
    }

    void ParticleEditorPanel::DrawStatus(AssetImportManager &imports)
    {
        if (!mError.empty()) ImGui::TextWrapped("%s", mError.c_str());
        if (mEmitter)
        {
            std::string error;
            if (!ValidateParticleEmitterAsset(*mEmitter, error)) ImGui::TextWrapped("Validation: %s", error.c_str());
        }
        if (!imports.View().mLastError.empty()) ImGui::TextWrapped("Cook/reload: %s", imports.View().mLastError.c_str());
        if (imports.View().mProcessRunning) ImGui::TextUnformatted("Cooking source assets...");
        if (imports.PendingApply()) ImGui::TextUnformatted("Cook complete, waiting for the next Edit reload.");
    }

    // The settings scroll on their own so the select bar, preview strip and emitter list stay pinned.
    void ParticleEditorPanel::DrawSettings(World &world)
    {
        const bool visible = ImGui::BeginChild("##Settings", ImVec2(0, 0));
        if (visible)
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            try
            {
                if (mSystemPath.empty()) ImGui::TextDisabled("Select or create a particle system.");
                else
                {
                    if (ImGui::CollapsingHeader(mDirtySystem ? "System settings *" : "System settings"))
                    {
                        mDirtySystem |= ImGui::DragFloat("Birth scale", &mSystem.parameters.spawnRateScale, 0.05f, 0, 16);
                        mDirtySystem |= ImGui::DragFloat("Size scale", &mSystem.parameters.sizeScale, 0.05f, 0, 16);
                        mDirtySystem |= ImGui::DragFloat3("World wind", &mSystem.parameters.wind.x, 0.05f);
                    }
                    DrawEmitter();
                }
                if (ImGui::CollapsingHeader("Profiling and limits")) DrawDiagnostics(world);
            }
            catch (const std::bad_alloc &) { throw; }
            catch (const std::exception &error) { mError = error.what(); }
            ImGui::PopItemWidth();
        }
        ImGui::EndChild();
    }

    void ParticleEditorPanel::Draw(bool &open, World &world, AssetCache &, AssetImportManager &imports,
        const EditorCamera &camera, float dt)
    {
        if (!open && (mDirtySystem || mDirtyEmitter))
        {
            open = true;
            mCloseRequested = true;
        }
        if (!open) { OnHidden(world);return; }
        ImGui::SetNextWindowSize(ImVec2(380, std::min(800.0f, ImGui::GetMainViewport()->WorkSize.y-80)), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Particle Editor", &open)) { ImGui::End();return; }
        mHistoryApplied = false;
        const DraftSnapshot before = CaptureDraft();
        try
        {
            if (mPendingHistoryAction < 0) UndoDraft();
            else if (mPendingHistoryAction > 0) RedoDraft();
            mPendingHistoryAction = 0;
            DrawSelectBar(world, imports);
            DrawEditBar(imports);
            DrawPreviewStrip(world, camera, dt);
            DrawEmitterStrip(world, imports);
            DrawStatus(imports);
            DrawSettings(world);
        }
        catch (const std::bad_alloc &) { throw; }
        catch (const std::exception &error) { mError = error.what(); }
        if (!mHistoryApplied && before.systemPath == mSystemPath && before.emitterPath == mEmitterPath)
        {
            const DraftSnapshot after = CaptureDraft();
            if (before.systemSource != after.systemSource || before.emitterSource != after.emitterSource)
                if (!mGestureStart) mGestureStart = before;
            if (mGestureStart && !ImGui::IsAnyItemActive()) CommitDraftGesture();
        }
        if (!mSystemPath.empty()) mDirtySystem = CaptureDraft().systemSource != mSavedSystemSource;
        if (mEmitter)
        {
            const auto saved = mSavedEmitterSources.find(mEmitterPath);
            mDirtyEmitter = saved == mSavedEmitterSources.end() ||
                CaptureDraft().emitterSource != saved->second;
        }
        if (!open && (mDirtySystem || mDirtyEmitter))
        {
            open = true;
            mCloseRequested = true;
        }
        if (mCloseRequested) ImGui::OpenPopup("Unsaved particle edits");
        if (ImGui::BeginPopupModal("Unsaved particle edits", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Save changes to the particle system and emitter before closing?");
            if (ImGui::Button("Save and close"))
            {
                try
                {
                    SaveAll(imports);
                    mCloseRequested = false;
                    open = false;
                    ImGui::CloseCurrentPopup();
                }
                catch (const std::exception &error) { mError = error.what(); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard and close"))
            {
                mError.clear();
                if (!mSystemPath.empty())
                    try { LoadSystem(mSystemPath); }
                catch (const std::exception &error) { mError = error.what(); }
                if (mError.empty())
                {
                    mCloseRequested = false;
                    open = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                mCloseRequested = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
        if (!open) OnHidden(world);
    }
}
