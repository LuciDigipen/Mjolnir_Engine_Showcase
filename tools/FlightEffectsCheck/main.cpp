/**
 * @file main.cpp
 * @author sumin.park
 * @brief Hidden-window flight acceptance and fixed-step production particle profiling.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetsModule.h>
#include <core/ecs/CoreModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <particles/ParticleWorld.h>
#include <particles/ParticlesModule.h>
#include <particles/components/ParticleSystem.h>
#include <particles_gpu/ParticlesGpuModule.h>
#include <particles_runtime/ParticlesRuntimeModule.h>
#include <particles_script/ParticleBindings.h>
#include <renderer/CameraMath.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/Shapes.h>
#include <renderer/VulkanRenderer.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>
#include <window/Window.h>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    constexpr const char *kRoot = "games/FlightEffectsCheck/assets";
    constexpr std::array<const char *,14> kMetrics{"cpuRuntimeMs","cpuSubmitMs","cpuRecordMs","frameWallMs",
        "gpuSimulationMs","gpuTextureUploadMs","gpuPrepareSortMs","gpuCompositionTransparentMs","cpuTotalManagementMs","gpuTotalFxMs",
        "cpuSimulationRecordMs","cpuTextureRecordMs","cpuPrepareRecordMs","cpuDrawRecordMs"};
    using Samples = std::array<std::vector<double>,kMetrics.size()>;
    void Require(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
    double Ms(Clock::time_point started) { return std::chrono::duration<double,std::milli>(Clock::now()-started).count(); }
    Json Summarize(const Samples &samples)
    {
        Json result;
        for(size_t i=0;i<samples.size();++i)
        {
            auto values=samples[i];
            if(values.empty()) continue;
            std::sort(values.begin(),values.end());
            result[kMetrics[i]]={{"samples",values.size()},{"median",values[(values.size()-1)/2]},
                {"p95",values[static_cast<size_t>(std::ceil(values.size()*0.95))-1]}};
        }
        return result;
    }
    const char *Phase(double seconds)
    {
        if(seconds<15) return "acceleration";
        if(seconds<60) return "turns-and-wind";
        if(seconds<70) return "offscreen";
        if(seconds<80) return "camera-return";
        if(seconds<126) return "shutdown-drain";
        return "retired";
    }
}

int main(int argc,char **argv)
{
    const std::filesystem::path output=argc>1?argv[1]:"flight-effects.json";
    Json report{{"status","failed"}};
    mir::LogConfig logging; logging.filePath=output.string()+".log"; mir::InitLog(logging);
    auto write=[&] {std::ofstream(output)<<report.dump(2)<<'\n';};
    try
    {
        Require(argc==8,"Usage: FlightEffectsCheck report.json cpu|raster|rt|dlss on|off 1|8 0|1000000 smoke|profile validation|timing");
        const std::string mode=argv[2],effects=argv[3],route=argv[6],checks=argv[7];
        Require(mode=="cpu" || mode=="raster" || mode=="rt" || mode=="dlss","Invalid rendering mode");
        Require(effects=="on" || effects=="off","Invalid effects choice");
        Require(std::string(argv[4])=="1" || std::string(argv[4])=="8","Aircraft count must be 1 or 8");
        Require(std::string(argv[5])=="0" || std::string(argv[5])=="1000000","Origin must be 0 or 1000000");
        Require(route=="smoke" || route=="profile","Invalid route");
        Require(checks=="validation" || checks=="timing","Invalid instrumentation mode");
        const bool enabled=effects=="on",profile=route=="profile",device=mode!="cpu";
        const int count=std::stoi(argv[4]),frames=profile?9000:240,warmup=profile?600:30;
        const double origin=std::stod(argv[5]);
        report.update({{"mode",mode},{"effects",enabled},{"aircraft",count},{"originMeters",origin},
            {"fixedStepSeconds",1.0/60},{"simulationSeconds",frames/60.0},{"warmupSeconds",warmup/60.0},
            {"validation",checks=="validation"},{"resolution",{1920,1080}},
            {"timingNote","GPU queries lag frame submission by two frame-slot cycles, capture frames and the next five frames are excluded. GPU uploads are texture copies, mapped birth/ribbon writes contribute CPU submit/record cost. frameWall includes GPU pacing and presentation, not an isolated CPU cost."}});
        mir::World world; mir::CommandBuffer commands; mir::SystemScheduler scheduler;
        mir::CoreModule core({world,scheduler},commands);
        mir::RegisterRendererComponents();
        mir::AssetsModule assets(mir::CookedAssetsDir());
        Require(assets.HadManifest(),"Missing cooked game assets");
        mir::ParticlesModule module({world,scheduler},assets,kRoot);
        auto &particles=module.Particles();
        mir::SetEngineMode(world,mir::EngineMode::Play);
        mir::ScriptHost scripts;
        const auto bindings=mir::AddBindings(scripts,&mir::RegisterParticleBindings);
        const auto *script=assets.Cache().Load(mir::MakeAssetId(std::string(kRoot)+"/scripts/aircraft.lua"));
        Require(script && scripts.LoadScriptSource("flight",mir::AsStringView(*script)),"Cannot load cooked flight route");
        std::vector<mir::Entity> owners;
        std::vector<int32_t> instances;
        for(int i=0;i<count+1;++i)
        {
            const auto owner=world.CreateEntity(); owners.push_back(owner);
            const double offset=i<count && count>1?(i%4-1.5)*6:0;
            mir::AddTransform(world,owner,mir::Transform(mir::WorldVec3(origin+offset,i<count?2+(i/4)*3:8,i<count?origin:origin+22)));
            if(i==count) world.AddComponent<mir::Camera>(owner,{60,0.1f,600});
            else if(enabled)
            {
                mir::ParticleSystem system; std::strcpy(system.systemName,"particles/aircraft.particle.json");
                system.playOnStart=true;world.AddComponent<mir::ParticleSystem>(owner,system);
            }
            instances.push_back(scripts.CreateInstance("flight"));
            Require(instances.back()!=-1,"Cannot create flight script instance");
            scripts.CallOnStart(world,commands,owner,instances.back());
        }
        commands.Flush(world);
        std::unique_ptr<mir::Window> window;
        std::unique_ptr<mir::VulkanRenderer> renderer;
        std::unique_ptr<mir::ParticlesGpuModule> gpu;
        mir::MeshHandle mesh;
        if(device)
        {
            window=mir::Window::Create({.mWidth=1920,.mHeight=1080,.mTitle="Flight effects acceptance",.mStartHidden=true});
            renderer=std::make_unique<mir::VulkanRenderer>();
            Require(renderer->Initialize({window.get(),"FlightEffectsCheck",checks=="validation"}),"Renderer initialization failed");
            if((mode=="rt" || mode=="dlss") && !renderer->RayTracingSupported() || mode=="dlss" && !renderer->DlssAvailable())
            { report["status"]="unsupported";write();renderer->Shutdown();return 77; }
            if(mode=="dlss")
            {
                mir::PostProcessingState post;post.antiAliasing.mode=mir::AntiAliasingMode::Dlss;
                renderer->SetPostProcessing(post);
            }
            renderer->SetDirectionalLight({0.3f,-1,0.2f},{1,0.9f,0.8f});
            renderer->SetAmbientColor({0.2f,0.25f,0.3f});
            const auto cube=mir::MakeCube(); mesh=renderer->CreateMesh(cube.vertices,cube.indices);
            Require(!mesh.IsNull(),"Cannot upload aircraft proxy");
            if(enabled) gpu=std::make_unique<mir::ParticlesGpuModule>(*renderer);
        }
        std::map<std::string,Samples> samples;
        std::vector<mir::WorldPose> previousPoses(count);
        Json timeline=Json::array();
        Json captures=Json::array();
        uint64_t cpuPeak=0,gpuPeak=0,compositionPeak=0,snapshotPeak=0,driverPeak=0,uploaded=0,admitted=0,poolDrops=0;
        uint32_t peakAlive=0,peakRibbon=0,peakInstances=0;
        uint64_t lastCompleted=0;
        int excludeThrough=-1;
        bool offscreenHistory=false,returnedHistory=false;
        bool historyDrift=false,offscreenCulled=false,returnedGpuVisible=false;
        std::optional<mir::ParticleRibbonPoint> oldPoint;
        uint32_t trailSlot=0;
        const auto started=Clock::now();
        for(int frame=0;frame<frames;++frame)
        {
            const auto frameStarted=Clock::now();
            const double seconds=(frame+1)/60.0;
            if(frame==warmup) report["warmupWallSeconds"]=Ms(started)/1000;
            for(size_t i=0;i<owners.size();++i)
                scripts.CallOnUpdate(world,commands,owners[i],instances[i],1.0f/60);
            commands.Flush(world);
            const auto camera=mir::ResolveWorld(world,owners.back());
            particles.Runtime().SetQualityOrigin(camera.position);
            const auto updating=Clock::now();
            for(int i=0;i<count && enabled;++i)
                particles.Synchronize(owners[i],*world.GetComponent<mir::ParticleSystem>(owners[i]),mir::ResolveWorld(world,owners[i]));
            particles.Runtime().Advance(1.0f/60);
            const double runtimeMs=Ms(updating);
            const auto cpu=particles.Runtime().Stats();
            for(const auto &emitter:particles.Runtime().EmitterSlots()) admitted+=emitter.spawns;
            cpuPeak=std::max(cpuPeak,cpu.cpuHighWaterBytes);peakInstances=std::max(peakInstances,cpu.active);
            peakRibbon=std::max(peakRibbon,cpu.ribbonPoints);
            if(seconds>62 && seconds<69) offscreenHistory|=cpu.ribbonPoints>0;
            if(seconds>71 && seconds<79) returnedHistory|=cpu.ribbonPoints>0;
            if(enabled && frame==3599)
                for(uint32_t i=0;i<particles.Runtime().EmitterSlots().size();++i)
                {
                    const auto &emitter=particles.Runtime().EmitterSlots()[i];
                    if(!emitter.ribbon.points.empty()) {oldPoint=emitter.ribbon.points.back();trailSlot=i;break;}
                }
            if(oldPoint && frame==4199)
                for(const auto &point:particles.Runtime().EmitterSlots()[trailSlot].ribbon.points)
                    if(point.ribbon==oldPoint->ribbon && point.link==oldPoint->link)
                    {
                        const auto drift=point.position.Meters()-oldPoint->position.Meters();
                        historyDrift=glm::length(drift-mir::WorldVec3(-30,5,10))<mir::WorldReal(0.02) && point.color.a<oldPoint->color.a;
                    }
            const auto submitting=Clock::now();
            if(gpu) gpu->Submit(mir::BuildParticleRenderData(particles.Runtime(),true),mir::BuildRibbonRenderData(particles.Runtime()));
            const double submitMs=Ms(submitting);
            if(renderer)
            {
                std::vector<mir::DrawItem> draws;
                for(int i=0;i<count;++i)
                {
                    const auto pose=mir::ResolveWorld(world,owners[i]);
                    const auto model=pose.TryMatrixRelativeTo(camera.position);
                    const auto previous=frame?previousPoses[i].TryMatrixRelativeTo(camera.position):model;
                    Require(model.has_value(),"Proxy is outside camera-relative range");
                    Require(previous.has_value(),"Previous proxy pose is outside camera-relative range");
                    mir::DrawItem body;body.mesh=mesh;body.model=glm::scale(*model,{0.6f,0.4f,3});body.previousModel=glm::scale(*previous,{0.6f,0.4f,3});
                    body.normalMatrix=glm::transpose(glm::inverse(glm::mat3(body.model)));
                    body.tint={0.3f,0.4f,0.6f,1};draws.push_back(body);
                    body.model=glm::scale(glm::translate(*model,{0,0,0.3f}),{3.5f,0.12f,0.7f});
                    body.previousModel=glm::scale(glm::translate(*previous,{0,0,0.3f}),{3.5f,0.12f,0.7f});
                    body.normalMatrix=glm::transpose(glm::inverse(glm::mat3(body.model)));draws.push_back(body);
                    previousPoses[i]=pose;
                }
                mir::RenderView view;view.items=draws;view.cameraPosition=camera.position;view.nearZ=0.1f;view.farZ=600;
                view.viewProj=mir::MakeViewProjection(glm::mat4(camera.linear),{60,0.1f,600},1920.0f/1080);
                if(profile && (frame==1199 || frame==3899 || frame==4499 || frame==5999 || frame==8399))
                {
                    const auto capture=output.parent_path()/(output.stem().string()+"-"+std::to_string(frame+1)+".png");
                    renderer->CaptureNextFrame(capture);excludeThrough=frame+5;
                    captures.push_back(capture.string());
                }
                window->PollEvents();
                if(frame==3599 || frame==4199) renderer->ResetHistory();
                Require(!window->ShouldClose(),"Hidden fixture window closed unexpectedly");
                if(mode=="raster") renderer->DrawFrame(std::span(&view,1));
                else renderer->TraceFrame(std::span(&view,1));
                if(frame%60==0) {driverPeak=std::max(driverPeak,renderer->GetGpuMemoryUsage().mUsageBytes);compositionPeak=std::max(compositionPeak,renderer->FxCompositionMemoryBytes());}
            }
            const auto stats=gpu?gpu->Stats():mir::ParticleGpuStats{};
            if(gpu && frame>=warmup) Require(stats.gpuTimingAvailable,"Asynchronous GPU timing sample did not complete");
            snapshotPeak=std::max(snapshotPeak,stats.snapshotBytes);
            gpuPeak=std::max(gpuPeak,stats.gpuHighWaterBytes);uploaded+=stats.uploadBytes;peakAlive=std::max(peakAlive,stats.alive);
            if(gpu && frame==3899) offscreenCulled=stats.visible[0]==0;
            if(gpu && seconds>71 && seconds<79) returnedGpuVisible|=stats.visible[0]>0;
            if(stats.completedFrame!=lastCompleted) {poolDrops+=stats.dropped;lastCompleted=stats.completedFrame;}
            if(frame>=warmup && frame>excludeThrough)
            {
                const std::array<double,14> values{runtimeMs,submitMs,stats.cpuRecordMs,Ms(frameStarted),
                    stats.simulationGpuMs,stats.uploadGpuMs,stats.prepareGpuMs,stats.drawGpuMs,runtimeMs+submitMs+stats.cpuRecordMs,
                    stats.simulationGpuMs+stats.uploadGpuMs+stats.prepareGpuMs+stats.drawGpuMs,
                    stats.cpuSimulationRecordMs,stats.cpuTextureRecordMs,stats.cpuPrepareRecordMs,stats.cpuDrawRecordMs};
                auto add=[&](Samples &rows) {for(size_t i=0;i<values.size();++i) {Require(std::isfinite(values[i]),"Nonfinite timing");rows[i].push_back(values[i]);}};
                add(samples["all"]);add(samples[Phase(seconds)]);
                if(seconds<80) add(samples["active"]);
            }
            else if(frame<warmup && frame>excludeThrough)
            {
                const std::array<double,14> values{runtimeMs,submitMs,stats.cpuRecordMs,Ms(frameStarted),
                    stats.simulationGpuMs,stats.uploadGpuMs,stats.prepareGpuMs,stats.drawGpuMs,runtimeMs+submitMs+stats.cpuRecordMs,
                    stats.simulationGpuMs+stats.uploadGpuMs+stats.prepareGpuMs+stats.drawGpuMs,
                    stats.cpuSimulationRecordMs,stats.cpuTextureRecordMs,stats.cpuPrepareRecordMs,stats.cpuDrawRecordMs};
                for(size_t i=0;i<values.size();++i) samples["warmup"][i].push_back(values[i]);
            }
            if(frame%600==599)
            {
                timeline.push_back({{"seconds",seconds},{"instances",cpu.active},{"ribbonPoints",cpu.ribbonPoints},
                    {"gpuAlive",stats.alive},{"cpuReservedBytes",cpu.cpuBytes},{"gpuOwnedBytes",stats.gpuBytes}});
                report["status"]="running";report["framesComplete"]=frame+1;report["elapsedWallSeconds"]=Ms(started)/1000;
                write();
                MIR_LOG_INFO("Flight route {}/{} frames, {} systems, {} ribbon points, {} GPU particles",frame+1,frames,cpu.active,cpu.ribbonPoints,stats.alive);
            }
        }
        const auto cpu=particles.Runtime().Stats();
        const auto stats=gpu?gpu->Stats():mir::ParticleGpuStats{};
        Require(!enabled || peakInstances==uint32_t(count),"Flight systems were not admitted");
        Require(!enabled || peakRibbon>0,"Flight route produced no contrails");
        Require(!gpu || peakAlive>0,"Flight route produced no GPU exhaust");
        Require(cpu.rejectedStarts==0,"Unexpected system admission rejection");
        Require(!gpu || stats.rejectedSubmissions==0,"Unexpected GPU submission rejection");
        if(profile && enabled)
        {
            Require(offscreenHistory && returnedHistory && historyDrift,"Offscreen return lost, froze or replayed CPU ribbon history");
            Require(!gpu || offscreenCulled && returnedGpuVisible,"GPU view did not cull particles offscreen and restore visibility on return");
            Require(cpu.active==0 && cpu.activeEmitters==0 && cpu.ribbonPoints==0,"Instances/history did not retire after trail lifetime");
            Require(!gpu || stats.alive==0,"GPU particles did not retire after shutdown");
            Require(cpu.cpuBytes<cpuPeak,"Retired CPU reservations did not release");
        }
        if(gpu) Require(stats.gpuTimingAvailable,"GPU timestamp timing unsupported, measured acceptance incomplete");
        if(mode=="dlss") Require(renderer->DlssAvailable(),"DLSS/RR stopped being available during the route");
        for(const auto &capture:captures)
            Require(std::filesystem::file_size(capture.get<std::string>())>0,"Acceptance capture is missing or empty");
        Json measurements;
        for(const auto &[phase,rows]:samples) measurements[phase]=Summarize(rows);
        if(profile && enabled && count==1 && device)
        {
            report["budget"]={{"cpuP95LimitMs",0.5},{"gpuP95LimitMs",2.0},{"memoryLimitBytes",64*1024*1024},
                {"cpuPass",measurements["active"]["cpuTotalManagementMs"]["p95"].get<double>()<=0.5},
                {"gpuPass",measurements["active"]["gpuTotalFxMs"]["p95"].get<double>()<=2.0},
                {"memoryPass",cpuPeak+gpuPeak+compositionPeak+snapshotPeak*3<=64ull*1024*1024}};
        }
        report.update({{"wallSeconds",Ms(started)/1000},{"measurements",measurements},{"timeline",timeline},
            {"cpuHighWaterBytes",cpuPeak},{"gpuHighWaterBytes",gpuPeak},{"compositionHighWaterBytes",compositionPeak},{"snapshotHighWaterBytes",snapshotPeak},{"accountedEffectHighWaterBytes",cpuPeak+gpuPeak+compositionPeak+snapshotPeak*3},{"driverUsageHighWaterBytes",driverPeak},
            {"peakInstances",peakInstances},{"peakGpuAlive",peakAlive},{"peakRibbonPoints",peakRibbon},
            {"admittedBirths",admitted},{"droppedCpuBirths",cpu.droppedBudgetSpawns},{"evictedRibbonPoints",cpu.evictedRibbonPoints},
            {"droppedUploadBirths",stats.droppedSubmitSpawns},{"observedCompletedPoolDrops",poolDrops},
            {"deferredTextureFrames",stats.deferredTextures},{"droppedRibbonSegments",stats.droppedRibbonSegments},
            {"uploadedBytes",uploaded},{"finalInstances",cpu.active},{"finalGpuAlive",stats.alive},
            {"finalCpuReservedBytes",cpu.cpuBytes},{"finalGpuOwnedBytes",stats.gpuBytes},
            {"offscreenHistory",offscreenHistory},{"returnedHistory",returnedHistory},
            {"historyDriftVerified",historyDrift},{"offscreenCullingVerified",offscreenCulled},
            {"returnedGpuVisible",returnedGpuVisible},{"captures",captures}});
        particles.Clear();
        for(size_t i=0;i<owners.size();++i) scripts.DestroyInstance(instances[i]);
        Require(scripts.InstanceCount()==0,"Flight Lua instances leaked");
        gpu.reset();
        if(renderer) renderer->Shutdown();
        Require(mir::ErrorCount()==0,"Acceptance logged errors, inspect validation log");
        report["status"]="passed";write();
        return 0;
    }
    catch(const std::exception &error)
    {
        report["status"]="failed";report["error"]=error.what();write();std::fprintf(stderr,"%s\n",error.what());return 1;
    }
}
