/**
 * @file ParticleRenderCheck.cpp
 * @author sumin.park
 * @brief Per-view sort proofs and captured depth, lighting, fog, blend and mode changes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleRenderCheck.h"

#include <core/log/Log.h>
#include <particles/ParticleEffect.h>
#include <particles_gpu/ParticlesGpuModule.h>
#include <particles_gpu/ParticleRibbonGeometry.h>
#include <renderer/CameraMath.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void RequireRender(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
    mir::WorldPosition Position(double x,double y,double z)
    { return *mir::WorldPosition::FromMeters(mir::WorldVec3(x,y,z)); }
}
void RunParticleRenderCheck(mir::VulkanRenderer &renderer,mir::ParticlesGpuModule &particles,
    mir::Window &window,const mir::ParticleEmitterAsset &emitter,const std::filesystem::path &output,bool rt,bool rr)
{
    mir::RenderView view;
    auto matrix=[&](bool reverse,float aspect)
    {
        const auto camera=reverse?glm::rotate(glm::mat4(1),glm::radians(180.0f),glm::vec3(0,1,0)):glm::mat4(1);
        return mir::MakeViewProjection(camera,mir::Camera{60,0.1f,100},aspect);
    };
    bool tracing=rt,split=false;
    auto draw=[&]
    {
        window.PollEvents();
        RequireRender(!window.ShouldClose(),"Particle render check closed early");
        const auto extent=renderer.SwapchainExtent();
        const auto aspect=float(extent.width)/extent.height;
        view.viewProj=matrix(false,aspect);
        if(tracing) renderer.TraceFrame(std::span(&view,1));
        else if(split)
        {
            std::array<mir::RenderView,2> views{view,view};
            views[0].rect={{0,0},{extent.width/2,extent.height}};
            views[0].viewProj=matrix(false,aspect/2);
            views[1].rect={{int32_t(extent.width/2),0},{extent.width-extent.width/2,extent.height}};
            views[1].cameraPosition=Position(0,0,-14);
            std::vector<mir::DrawItem> reverseItems(view.items.begin(),view.items.end());
            for(auto &item:reverseItems) { item.model[3].z+=14;item.previousModel=item.model; }
            views[1].items=reverseItems;
            views[1].viewProj=matrix(true,aspect/2);
            renderer.DrawFrame(views);
        }
        else renderer.DrawFrame(std::span(&view,1));
    };
    auto frames=[&](int count=4) {for(int f=0;f<count;++f) draw();};
    auto capture=[&](const char *name)
    {
        const auto path=output.parent_path()/(output.stem().string()+"-"+name+".png");
        renderer.CaptureNextFrame(path);draw();
        RequireRender(std::filesystem::exists(path),"Particle composition capture missing");
    };
    particles.Clear();frames();
    mir::ParticleSubmission input;
    input.scriptKey=emitter.script.value;input.scriptCode=emitter.scriptCode;
    input.index=0;input.generation=900;input.capacity=5;input.spawns=3;
    input.seconds=0;input.lifetime=30;input.size=0.8f;input.velocity={0.3f,0.7f,0};input.color={1,0.4f,0.15f,0.7f};
    input.explicitTile=input.swept=true;
    input.emissionStart=Position(0,0,-4);input.anchor=Position(0,0,-10);
    input.births={{0,0,0,0},{0.5f,0,1,0},{1,0,2,0}};
    particles.Submit(std::span(&input,1));frames();
    auto stats=particles.Stats();
    RequireRender(stats.visible[0]==3 && stats.firstSortedParticle[0]==2,"GPU back-to-front sorting or non-power-of-two padding failed");
    RequireRender(stats.alive==3,"View preparation changed simulation particle count");
    if(!rt)
    {
        split=true;frames();stats=particles.Stats();
        RequireRender(stats.visible[0]==3 && stats.visible[1]==3,"Split-view culling did not retain each camera's visible particles");
        RequireRender(stats.firstSortedParticle[0]==2 && stats.firstSortedParticle[1]==0,"Second camera reused the first camera's particle order");
        RequireRender(stats.alive==3 && stats.spawned==0,"Extra views advanced particle simulation or replayed births");
        capture("two-cameras");split=false;
    }
    input.spawns=0;input.births.clear();input.renderer.sort=1;
    particles.Submit(std::span(&input,1));frames();
    RequireRender(particles.Stats().firstSortedParticle[0]==0,"Sort none did not preserve pool order");
    view.cameraPosition=Position(1000,0,0);frames();
    RequireRender(particles.Stats().visible[0]==0 && particles.Stats().alive==3,"Offscreen culling reset simulation or retained invisible particles");
    view.cameraPosition={};input.renderer.sort=0;
    input.renderer.maxDistance=5;
    particles.Submit(std::span(&input,1));frames();
    RequireRender(particles.Stats().visible[0]==1,"Per-view distance culling is incorrect");
    input.renderer.maxDistance=100;
    // An opaque wall and a blended quad expose depth edges and the deliberate glass ordering limit.
    const std::array<mir::Vertex,4> vertices{{
        {{-1,-1,0},{1,1,1},{0,0,1},{0,0}},{{1,-1,0},{1,1,1},{0,0,1},{1,0}},
        {{1,1,0},{1,1,1},{0,0,1},{1,1}},{{-1,1,0},{1,1,1},{0,0,1},{0,1}}}};
    const std::array<uint32_t,6> indices{0,1,2,0,2,3};
    const auto mesh=renderer.CreateMesh(vertices,indices);
    RequireRender(!mesh.IsNull(),"Particle composition wall mesh failed");
    mir::MaterialDesc opaque;opaque.cullMode=VK_CULL_MODE_NONE;
    const auto opaqueMaterial=renderer.CreateMaterial(opaque);
    opaque.shaderName="model";opaque.alphaMode=mir::MaterialAlphaMode::Blend;
    const auto glassMaterial=renderer.CreateMaterial(opaque);
    std::array<mir::DrawItem,2> items;
    items[0].mesh=items[1].mesh=mesh;
    items[0].material=opaqueMaterial;items[1].material=glassMaterial;items[1].transparent=true;
    items[0].model=glm::translate(glm::mat4(1),glm::vec3(-1,0,-6.2f));
    items[0].tint={0.15f,0.6f,0.4f,1};
    items[1].model=glm::translate(glm::mat4(1),glm::vec3(1,0,-3.5f));
    items[1].tint={0.2f,0.6f,1,0.35f};
    view.items=items;
    mir::RibbonSubmission ribbon;
    for(uint64_t i=0;i<5;++i)
        ribbon.points.push_back({Position(-2.0+double(i),0.4,-5.6-double(i)*0.2),double(i)*2,1,i,0.35f,{0.7f,0.85f,1,0.65f}});
    RequireRender(mir::BuildRibbonGeometry(std::span(&ribbon,1),{},{1,0,0}).size()==24,
        "Composition fixture ribbon lost expected segments");
    input.textureKey=ribbon.textureKey=551;
    input.textureWidth=input.textureHeight=ribbon.textureWidth=ribbon.textureHeight=2;
    input.texturePixels=ribbon.texturePixels={255,255,255,255,90,90,90,255,170,170,170,255,255,255,255,255};
    input.renderer.textured=ribbon.renderer.textured=true;
    input.renderer.softDistance=ribbon.renderer.softDistance=0;
    input.renderer.fog=ribbon.renderer.fog=false;
    auto submit=[&] {particles.Submit(std::span(&input,1),std::span(&ribbon,1));frames();};
    mir::PostProcessingState effects;
    effects.fog.density=0;
    effects.antiAliasing.mode=rr?mir::AntiAliasingMode::Dlss:mir::AntiAliasingMode::Off;
    renderer.SetPostProcessing(effects);
    renderer.SetDirectionalLight({0.2f,0.9f,0.4f},{0.9f,0.8f,0.6f});renderer.SetAmbientColor({0.15f,0.15f,0.15f});
    renderer.SetSkyVisible(true);
    renderer.SetAtmosphere(mir::Sky{});
    particles.Clear();frames();capture("opaque-sky-baseline");
    ++input.generation;input.spawns=3;input.births={{0,0,0,0},{0.5f,0,1,0},{1,0,2,0}};
    submit();input.spawns=0;input.births.clear();capture("hard-depth");
    input.renderer.softDistance=ribbon.renderer.softDistance=1;
    submit();capture("soft-depth");
    if(!rt) {split=true;frames();capture("two-cameras-depth");split=false;}
    input.renderer.lit=ribbon.renderer.lit=true;
    submit();capture("sun-ambient");
    input.renderer.fog=ribbon.renderer.fog=true;
    effects.fog={{0.5f,0.7f,0.9f},0.15f,0,100};renderer.SetPostProcessing(effects);
    submit();capture("fog");
    input.renderer.blend=1;input.renderer.alignment=1;input.renderer.facing=1;ribbon.renderer.facing=1;
    submit();capture("additive-facing");
    effects.depthOfFieldEnabled=effects.motionBlurEnabled=true;
    effects.depthOfField.focusDistance=8;effects.depthOfField.aperture=0.08f;
    effects.antiAliasing.mode=rr?mir::AntiAliasingMode::Dlss:(rt?mir::AntiAliasingMode::Msaa:mir::AntiAliasingMode::Fxaa);
    renderer.SetPostProcessing(effects);frames();capture("post-effects");
    effects.depthOfFieldEnabled=effects.motionBlurEnabled=false;
    effects.antiAliasing.mode=mir::AntiAliasingMode::Off;renderer.SetPostProcessing(effects);
    if(renderer.RayTracingSupported())
    {
        for(int change=0;change<4;++change)
        {
            tracing=!tracing;frames();
            RequireRender(particles.Stats().alive==3 && particles.Stats().spawned==0,"Backend switch reset simulation or replayed births");
        }
    }
    tracing=rt;
    glfwSetWindowSize(static_cast<GLFWwindow *>(window.NativeHandleForImGui()),960,540);
    frames(8);capture("resized");
    RequireRender(particles.Stats().visible[0]==3,"Camera return or resize lost particle state");
    particles.Clear();frames();
    RequireRender(particles.Stats().alive==0 && particles.Stats().visible[0]==0,"Scene clear retained render indices or particles");
    view.items={};renderer.DestroyMesh(mesh);
    MIR_LOG_INFO("Production composition: per-view order/culling, no double simulation, depth, lighting, fog, texture, blend, cuts, resize and backend changes passed");
}
