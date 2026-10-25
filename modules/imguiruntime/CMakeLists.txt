engine_add_module(imguiruntime)

target_link_libraries(engine_imguiruntime
    # PUBLIC core: ImGuiModule.h holds a ModuleContext and hands out Registrations
    PUBLIC mir::core
           mir::renderer
           mir::window
    PRIVATE mir::input
            imgui::imgui
            iconfontcppheaders::iconfontcppheaders
)

target_sources(engine_imguiruntime PRIVATE src/imguiruntime/ImGuiModule.cpp src/imguiruntime/ImGuiRuntime.cpp)
