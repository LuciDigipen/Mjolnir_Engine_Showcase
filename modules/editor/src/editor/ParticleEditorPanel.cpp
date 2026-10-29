/**
 * @file ParticleEditorPanel.cpp
 * @author sumin.park
 * @brief Edits source stacks and previews cooked systems through the production runtime.
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
#include <core/fs/FileIO.h>
#include <particles/ParticleWorld.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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
        bool Choice(const char *label, uint32_t &value, const char *choices)
        {
            int index = static_cast<int>(value);
            if (!ImGui::Combo(label, &index, choices)) return false;
            value = static_cast<uint32_t>(index);
            return true;
        }
    }

    ParticleEditorPanel::ParticleEditorPanel(std::filesystem::path sourceRoot, std::string idRoot)
        : mSourceRoot(std::move(sourceRoot)), mIdRoot(std::move(idRoot)) {}

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
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(mSourceRoot, std::filesystem::directory_options::skip_permission_denied, error), end;
            !error && it != end; it.increment(error))
        {
            if (it->is_regular_file() && it->path().filename().string().ends_with(".particle.json"))
                mSystems.push_back(std::filesystem::relative(it->path(), mSourceRoot).generic_string());
            if (mSystems.size() == 4096) break;
        }
        std::sort(mSystems.begin(), mSystems.end());
        mScanned = true;
    }

    void ParticleEditorPanel::LoadSystem(const std::string &path)
    {
        const auto text = ReadText(SourceFile(path));
        if (!text) throw std::runtime_error("cannot read particle system source");
        auto result = ReadParticleEffectSource(*text, mIdRoot);
        if (!result.mEffect) throw std::invalid_argument(result.mError);
        mSystem = *result.mEffect; mReferences = result.mEmitterPaths; mSystemPath = path;
        mDirtySystem = false; mEmitter.reset(); mSelected = 0;
        LoadEmitter(0);
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
    }

    void ParticleEditorPanel::DrawStack(bool spawn)
    {
        auto &definition = *mEmitter;
        auto &stack = spawn ? definition.spawnModules : definition.updateModules;
        ImGui::PushID(spawn ? "spawn" : "update");
        ImGui::TextUnformatted(spawn ? "Spawn stack" : "Update stack");
        for (size_t i = 0; i < stack.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            auto &module = stack[i];
            const auto infos = ParticleModules();
            const auto info = std::find_if(infos.begin(), infos.end(), [&](const auto &entry)
                { return entry.name == module.name && entry.spawn == spawn && entry.target == definition.simulationTarget; });
            const bool expanded = ImGui::TreeNodeEx(module.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (expanded)
            {
                if (module.name == "ColorGradient" || module.name == "RibbonColorGradient")
                {
                    mDirtyEmitter |= ImGui::ColorEdit4("Start color", module.parameters.data(), ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                    mDirtyEmitter |= ImGui::ColorEdit4("End color", module.parameters.data()+4, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                }
                else
                {
                    for (size_t p = 0; p < module.parameters.size(); ++p)
                    {
                        const std::string label = info != infos.end() && p < info->parameters.size() ?
                            std::string(*(info->parameters.begin()+p)) : "Parameter " + std::to_string(p);
                        mDirtyEmitter |= ImGui::DragFloat(label.c_str(), &module.parameters[p], 0.02f);
                    }
                    if (module.name == "SizeCurve" || module.name == "RibbonWidthCurve")
                    {
                        float samples[64];
                        for (int p = 0; p < 64; ++p) samples[p] = EvaluateParticleCurve(module.parameters[0], module.parameters[1], module.parameters[2], p/63.0f);
                        ImGui::PlotLines("Normalized life", samples, 64, 0, nullptr, 0, *std::max_element(samples,samples+64)+0.1f, ImVec2(0,65));
                    }
                }
                ImGui::TreePop();
            }
            if (definition.simulationTarget == ParticleSimulationTarget::GPU)
            {
                bool changed = false;
                if (i && ImGui::SmallButton("Up")) { std::swap(stack[i], stack[i-1]); changed = true; }
                ImGui::SameLine();
                if (i+1 < stack.size() && ImGui::SmallButton("Down")) { std::swap(stack[i], stack[i+1]); changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) { stack.erase(stack.begin()+i); changed = true; }
                if (changed) { mDirtyEmitter = true; ImGui::PopID(); break; }
            }
            ImGui::PopID();
        }
        if (definition.simulationTarget == ParticleSimulationTarget::GPU && stack.size() < 32 && ImGui::BeginCombo("Add module", "Select operation"))
        {
            for (const auto &info : ParticleModules())
                if (info.target == definition.simulationTarget && info.spawn == spawn && ImGui::Selectable(std::string(info.name).c_str()))
                {
                    ParticleModule module{std::string(info.name), std::vector<float>(info.parameters.size(),0)};
                    size_t index = 0;
                    for (const auto parameter : info.parameters)
                    {
                        if (parameter == "columns" || parameter == "rows" || parameter == "endScale" || parameter == "start" || parameter == "middle" || parameter == "end")
                            module.parameters[index] = 1;
                        ++index;
                    }
                    if (info.name == "ColorGradient") module.parameters = {1,1,1,1,1,1,1,0};
                    stack.push_back(std::move(module)); mDirtyEmitter = true;
                }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    void ParticleEditorPanel::DrawEmitter()
    {
        if (!mEmitter) return;
        auto &e = *mEmitter;
        ImGui::Separator(); ImGui::Text("Emitter: %s%s",mEmitterPath.c_str(),mDirtyEmitter ? " *" : "");
        ImGui::TextUnformatted(e.simulationTarget == ParticleSimulationTarget::GPU ? "GPU sprite emitter" : "CPU ribbon emitter");
        mDirtyEmitter |= Text("Texture",mTexturePath);
        mDirtyEmitter |= ImGui::DragFloat("Lifetime",&e.lifetime,0.05f,0.01f,120, "%.2f s",ImGuiSliderFlags_AlwaysClamp);
        int capacity = static_cast<int>(e.capacity);
        if (ImGui::DragInt("Capacity",&capacity,1,1,e.simulationTarget == ParticleSimulationTarget::CPU ? 1024 : 65536,"%d",ImGuiSliderFlags_AlwaysClamp))
        { e.capacity = static_cast<uint32_t>(capacity); mDirtyEmitter = true; }
        if (e.simulationTarget == ParticleSimulationTarget::GPU)
        {
            mDirtyEmitter |= ImGui::DragFloat("Births per second",&e.spawnRate,1,0,65536);
            int burst = static_cast<int>(e.burst);
            if (ImGui::DragInt("Initial burst",&burst,1,0,65536,"%d",ImGuiSliderFlags_AlwaysClamp)) { e.burst=burst;mDirtyEmitter=true; }
        }
        mDirtyEmitter |= ImGui::DragFloat("Births per metre",&e.spawnPerMeter,0.05f,0,65536);
        mDirtyEmitter |= ImGui::DragFloat3("Attachment offset",&e.localOffset.x,0.05f);
        mDirtyEmitter |= ImGui::DragFloat3("Local velocity",&e.velocity.x,0.05f);
        mDirtyEmitter |= ImGui::DragFloat("Inherited velocity",&e.inheritVelocity,0.02f,0,16);
        mDirtyEmitter |= ImGui::DragFloat("Teleport distance",&e.teleportDistance,1,0.01f,65536);
        mDirtyEmitter |= ImGui::InputScalar("Seed",ImGuiDataType_U32,&e.seed);
        mDirtyEmitter |= ImGui::DragFloat("Size",&e.size,0.01f,0.001f,64);
        mDirtyEmitter |= ImGui::ColorEdit4("Base color",&e.color.x,ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float);
        if (e.simulationTarget == ParticleSimulationTarget::CPU)
        {
            bool curve = e.updateModules[2].name == "RibbonWidthCurve";
            if (ImGui::Checkbox("Width curve",&curve))
            { e.updateModules[2] = curve ? ParticleModule{"RibbonWidthCurve",{1,2,4}} : ParticleModule{"RibbonWidth",{4}};mDirtyEmitter=true; }
            bool gradient = e.updateModules.size()==5;
            if (ImGui::Checkbox("Color gradient",&gradient))
            {
                if (gradient) e.updateModules.insert(e.updateModules.end()-1,{"RibbonColorGradient",{1,1,1,1,1,1,1,1}});
                else e.updateModules.erase(e.updateModules.begin()+3);
                mDirtyEmitter=true;
            }
        }
        DrawStack(true); DrawStack(false);
        if (ImGui::CollapsingHeader("Renderer",ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &r=e.renderer;
            mDirtyEmitter |= Choice("Alignment",r.alignment,"Screen\0Velocity\0");
            mDirtyEmitter |= Choice("Facing",r.facing,"Camera plane\0Camera position\0");
            mDirtyEmitter |= Choice("Blend",r.blend,"Alpha\0Additive\0");
            mDirtyEmitter |= Choice("Sort",r.sort,"Back to front\0None\0");
            mDirtyEmitter |= ImGui::DragFloat("Soft intersection",&r.softDistance,0.05f,0,1024);
            mDirtyEmitter |= ImGui::DragFloat("Draw distance",&r.maxDistance,10,1,10000000);
            mDirtyEmitter |= ImGui::Checkbox("Sun and ambient lighting",&r.lit);
            mDirtyEmitter |= ImGui::Checkbox("Distance fog",&r.fog);
            mDirtyEmitter |= ImGui::Checkbox("Sample texture",&r.textured);
        }
        if (ImGui::CollapsingHeader("Importance and distance quality",ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &q=e.quality;
            mDirtyEmitter |= ImGui::SliderFloat("Importance",&q.importance,0,1);
            mDirtyEmitter |= ImGui::DragFloat("Full quality distance",&q.nearDistance,10,0,10000000);
            mDirtyEmitter |= ImGui::DragFloat("Minimum quality distance",&q.farDistance,10,1,10000000);
            mDirtyEmitter |= ImGui::SliderFloat("Minimum birth scale",&q.minSpawnScale,0,1);
            ImGui::TextWrapped("Quality changes births. Existing particles and offscreen trails keep simulating.");
        }
    }

    void ParticleEditorPanel::OnHidden(World &world)
    {
        if (auto *particles=world.TryResource<ParticleWorld>(); particles && mPreviewing) particles->StopPreview();
        mPreviewing=false;
    }

    void ParticleEditorPanel::DrawDiagnostics(World &world)
    {
        auto *particles=world.TryResource<ParticleWorld>();
        if (!particles) return;
        const auto &s=particles->Runtime().Stats();
        const auto &limits=particles->Runtime().Budgets();
        if (!mBudgetsKnown) {mBudgetDraft=limits;mBudgetsKnown=true;}
        if (ImGui::CollapsingHeader("Session resource limits"))
        {
            ImGui::TextWrapped("Apply while no effects are alive. These session limits also apply during Play.");
            ImGui::BeginDisabled(s.active!=0 || !IsEditing(world));
            ImGui::InputScalar("Emitter limit",ImGuiDataType_U32,&mBudgetDraft.emitters);
            ImGui::InputScalar("GPU particle reservation limit",ImGuiDataType_U32,&mBudgetDraft.gpuParticles);
            ImGui::InputScalar("Ribbon reservation limit",ImGuiDataType_U32,&mBudgetDraft.ribbonPoints);
            ImGui::InputScalar("Birth work limit",ImGuiDataType_U32,&mBudgetDraft.births);
            float mib=mBudgetDraft.cpuBytes/1048576.0f;
            if(ImGui::SliderFloat("CPU reservation limit (MiB)",&mib,0.0625f,64)) mBudgetDraft.cpuBytes=uint64_t(mib*1048576);
            if(ImGui::Button("Apply limits") && !particles->Runtime().ConfigureBudgets(mBudgetDraft))
                mError="Limits must be positive and within the compiled resource ceilings.";
            ImGui::SameLine();
            if(ImGui::Button("Default limits")) {mBudgetDraft={};particles->Runtime().ConfigureBudgets(mBudgetDraft);}
            ImGui::EndDisabled();
        }
        ImGui::TextWrapped("Systems %u/%u, emitters %u/%u",s.active,particles->Runtime().Capacity(),s.activeEmitters,limits.emitters);
        ImGui::TextWrapped("GPU capacity %u/%u, ribbon samples %u (reserved %u/%u)",s.gpuCapacity,limits.gpuParticles,s.ribbonPoints,s.ribbonCapacity,limits.ribbonPoints);
        ImGui::TextWrapped("CPU reserved %.2f MiB (peak %.2f), limit %.2f MiB",s.cpuBytes/1048576.0,s.cpuHighWaterBytes/1048576.0,limits.cpuBytes/1048576.0);
        ImGui::TextWrapped("Birth budget %u, reduced emitters %u",limits.births,s.qualityReducedEmitters);
        ImGui::TextWrapped("Rejected starts %llu, memory %llu, dropped births %llu, evicted trail points %llu",
            s.rejectedStarts,s.rejectedMemoryStarts,s.droppedBudgetSpawns,s.evictedRibbonPoints);
        if (const auto *d=world.TryResource<ParticleDiagnostics>())
        {
            ImGui::TextWrapped("GPU alive %u, completed frame %llu",d->gpuAlive,d->gpuFrame);
            ImGui::TextWrapped("GPU owned %.2f MiB (peak %.2f), snapshot %.2f MiB, last uploads %.1f KiB",
                d->gpuBytes/1048576.0,d->gpuHighWaterBytes/1048576.0,d->snapshotBytes/1048576.0,d->uploadBytes/1024.0);
            ImGui::TextWrapped("Rejected GPU packets %llu, dropped GPU upload births %llu, last completed pool drops %llu",
                d->rejectedGpu,d->droppedGpuBirths,d->completedPoolDrops);
            ImGui::TextWrapped("Deferred textures %llu, dropped segments %llu",d->deferredTextures,d->droppedRibbonSegments);
            ImGui::TextWrapped("CPU simulation %.3f ms, submit %.3f ms, recording %.3f ms",s.cpuMs,d->cpuSubmitMs,d->cpuRecordMs);
            if (d->gpuTimingAvailable) ImGui::TextWrapped("GPU simulation %.3f ms, texture uploads %.3f ms, prepare %.3f ms, draw %.3f ms (asynchronous)",
                d->simulationGpuMs,d->uploadGpuMs,d->prepareGpuMs,d->drawGpuMs);
            else ImGui::TextUnformatted("GPU timing unavailable on this queue");
        }
    }

    void ParticleEditorPanel::Draw(bool &open, World &world, AssetCache &, AssetImportManager &imports,
        const EditorCamera &camera, float dt)
    {
        if (!open) {OnHidden(world);return;}
        ImGui::SetNextWindowSize(ImVec2(580,std::min(750.0f,ImGui::GetMainViewport()->WorkSize.y-80)),ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Particles",&open)) {ImGui::End();return;}
        try
        {
            if (!mScanned || ImGui::Button("Refresh systems")) Scan();
            if ((mDirtySystem || mDirtyEmitter) && ImGui::Button("Discard drafts and reload")) LoadSystem(mSystemPath);
            std::string requestedSystem;
            ImGui::BeginDisabled(mDirtySystem || mDirtyEmitter);
            if (ImGui::BeginCombo("System",mSystemPath.empty()?"Select a particle system":mSystemPath.c_str()))
            {
                for (const auto &path:mSystems) if (ImGui::Selectable(path.c_str(),path==mSystemPath)) requestedSystem=path;
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if(!requestedSystem.empty()) { OnHidden(world); LoadSystem(requestedSystem); }
            auto *particles=world.TryResource<ParticleWorld>();
            const bool editing=IsEditing(world);
            if (!editing) OnHidden(world);
            ImGui::BeginDisabled(!particles || !editing || mSystemPath.empty());
            if (ImGui::Button("Preview"))
            {
                OnHidden(world);mPreviewing=true;mPreviewPaused=false;mPreviewTime=0;
                mPreviewAnchor=camera.Pose();
                mPreviewAnchor.linear=WorldMat3(1);
                mPreviewAnchor.position=*WorldPosition::FromMeters(camera.Position().Meters()+WorldVec3(camera.Forward())*WorldReal(6));
            }
            ImGui::SameLine();
            if (ImGui::Button(mPreviewPaused?"Resume preview":"Pause preview"))
            { mPreviewPaused=!mPreviewPaused;if(particles) particles->Runtime().Pause(particles->PreviewHandle(),mPreviewPaused); }
            ImGui::SameLine(); if (ImGui::Button("Clear preview")) OnHidden(world);
            ImGui::Checkbox("Move preview attachment",&mMovePreview);
            ImGui::EndDisabled();
            if (particles && mPreviewing && editing)
            {
                if (!particles->Runtime().Find(particles->PreviewHandle()))
                {
                    const auto logical=MakeLogicalAssetPath(mIdRoot,mSystemPath);
                    const auto handle=particles->StartPreview(MakeAssetId(logical.path),mPreviewAnchor);
                    if(handle.IsNull()) {mPreviewing=false;mError="Cook this system and its dependencies before previewing.";}
                    else particles->Runtime().Pause(handle,mPreviewPaused);
                }
                if (!mPreviewPaused)
                {
                    mPreviewTime+=std::min(dt,0.067f);
                    auto pose=mPreviewAnchor;
                    if(mMovePreview) pose.position=*WorldPosition::FromMeters(pose.position.Meters()+WorldVec3(std::sin(mPreviewTime)*2,0,0));
                    particles->Runtime().SetPose(particles->PreviewHandle(),pose);
                }
            }
            ImGui::TextWrapped("Preview uses cooked assets. Save and recook applies in Edit, Play waits until Stop. Scene and Play changes clear preview.");
            ImGui::Separator(); DrawDiagnostics(world);
            if (!mSystemPath.empty())
            {
                ImGui::Separator(); ImGui::Text("System asset%s",mDirtySystem?" *":"");
                mDirtySystem |= ImGui::DragFloat("System birth scale",&mSystem.parameters.spawnRateScale,0.05f,0,16);
                mDirtySystem |= ImGui::DragFloat("System size scale",&mSystem.parameters.sizeScale,0.05f,0,16);
                mDirtySystem |= ImGui::DragFloat3("World wind",&mSystem.parameters.wind.x,0.05f);
                std::optional<size_t> requestedEmitter;
                for(size_t i=0;i<mReferences.size();++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    mDirtySystem |= Text("Emitter reference",mReferences[i]);
                    ImGui::SameLine(); ImGui::BeginDisabled(mDirtyEmitter);
                    if(ImGui::SmallButton("Edit")) requestedEmitter=i;
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                if(requestedEmitter) LoadEmitter(*requestedEmitter);
                if (mReferences.size()<8 && ImGui::Button("Add emitter reference"))
                {mReferences.push_back("particles/new.emitter.json");mDirtySystem=true;}
                ImGui::SameLine(); if(mReferences.size()>1 && ImGui::Button("Remove last reference"))
                {mReferences.pop_back();mDirtySystem=true;}
                if(ImGui::Button("Save system + recook"))
                {
                    mSystem.emitters.clear();
                    for(const auto &path:mReferences) mSystem.emitters.push_back(MakeAssetId(MakeLogicalAssetPath(mIdRoot,path).path));
                    const auto source=WriteParticleEffectSource(mSystem,mReferences);
                    const auto path=SourceFile(mSystemPath);
                    if(!WriteTextAtomic(path,source)) throw std::runtime_error("could not save system source");
                    imports.QueueChanged(path);mDirtySystem=false;mError.clear();
                }
            }
            DrawEmitter();
            if (mEmitter && ImGui::Button("Save emitter + recook"))
            {
                const auto source=WriteParticleEmitterSource(*mEmitter,mTexturePath);
                const auto path=SourceFile(mEmitterPath);
                if(!WriteTextAtomic(path,source)) throw std::runtime_error("could not save emitter source");
                imports.QueueChanged(path);mDirtyEmitter=false;mError.clear();
            }
            if(mEmitter)
            {
                std::string error;
                if(!ValidateParticleEmitterAsset(*mEmitter,error)) ImGui::TextWrapped("Validation: %s",error.c_str());
            }
        }
        catch (const std::bad_alloc &) {throw;}
        catch (const std::exception &error) {mError=error.what();}
        if(!mError.empty()) ImGui::TextWrapped("%s",mError.c_str());
        if(!imports.View().mLastError.empty()) ImGui::TextWrapped("Cook/reload: %s",imports.View().mLastError.c_str());
        if(imports.View().mProcessRunning) ImGui::TextUnformatted("Cooking source assets...");
        if(imports.PendingApply()) ImGui::TextUnformatted("Cook complete, waiting for the next Edit reload.");
        ImGui::End();
        if(!open) OnHidden(world);
    }
}
