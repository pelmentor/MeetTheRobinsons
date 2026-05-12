cmake_minimum_required(VERSION 3.20)
project(mtr-asi
    VERSION 0.2.0
    DESCRIPTION "ASI mod for Meet the Robinsons (2007 PC)"
    LANGUAGES CXX
)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "mtr-asi must be built as 32-bit. Configure with: cmake -B build -A Win32")
endif()

if (NOT WIN32)
    message(FATAL_ERROR "mtr-asi only builds for Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MTR_ASI_OUTPUT_NAME "mtr-asi")

set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
if (NOT EXISTS "${MINHOOK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "MinHook not found. Run: git submodule update --init --recursive")
endif()
add_subdirectory("${MINHOOK_DIR}" minhook EXCLUDE_FROM_ALL)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
if (NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui not found. Run: git submodule update --init --recursive")
endif()
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx9.cpp"
)
target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_link_libraries(imgui PUBLIC d3d9)

add_library(mtr-asi SHARED
    src/dllmain.cpp
    src/log.cpp
    src/menu.cpp
    src/menu/menu_helpers.cpp
    src/menu/menu_fps_overlay.cpp
    src/menu/tab_camera.cpp
    src/menu/tab_picture.cpp
    src/menu/tab_world.cpp
    src/menu/tab_performance.cpp
    src/menu/tab_tools.cpp
    src/menu/tab_debug.cpp
    src/d3d9_hook.cpp
    src/d3d9_hook/camera_hooks.cpp
    src/d3d9_hook/sprite_hooks.cpp
    src/d3d9_hook/diag_hooks.cpp
    src/cmdline_hook.cpp
    src/screenshot.cpp
    src/aspect_patch.cpp
    src/console.cpp
    src/screen_push.cpp
    src/level_select.cpp
    src/freecam.cpp
    src/input_hook.cpp
    src/dinput_hook.cpp
    src/cvar_dump.cpp
    src/ui_aspect_rules.cpp
    src/vis_test_probe.cpp
    src/scene_vis_log.cpp
    src/peripheral_cull_probe.cpp
    src/fps_limit.cpp
    src/sprite_probe.cpp
    src/sprite_split.cpp
    src/sprite_xform.cpp
    src/sprite_picking.cpp
    src/state_key_probe.cpp
    src/widget_probe.cpp
    src/sim_decouple.cpp
    src/sim_decouple/sim_decouple_throttle.cpp
    src/sim_decouple/sim_decouple_telemetry.cpp
    src/interp.cpp
    src/interp/interp_view.cpp
    src/interp/interp_player.cpp
    src/interp/interp_npc.cpp
    src/interp/interp_halo.cpp
    src/dt_correctness.cpp
    src/windowmode.cpp
    src/test_harness.cpp
    src/crash_handler.cpp
    src/msaa.cpp
    src/trigger_overlay.cpp
    src/npc_overlay.cpp
    src/prop_overlay.cpp
    src/entity_kv.cpp
    src/coop/coop_spawn_probe.cpp
    src/coop/protagonist_registry.cpp
    src/coop/coop_vibrate_route.cpp
    src/coop/coop_registry_mirror.cpp
    src/coop/engine_player.cpp
    src/coop/remote_player.cpp
    src/coop/remote_player_manager.cpp
    src/coop/net/remote_pose_packet.cpp
    src/coop/net/net_session.cpp
    src/coop/input/controlmapper_dev.cpp
    src/coop/input/per_entity_tick_hook.cpp
    src/coop/coop_dual_launch.cpp
    src/cmdline_utils.cpp
    src/save_system.cpp
)

target_include_directories(mtr-asi PRIVATE
    include
    "${MINHOOK_DIR}/include"
)

target_link_libraries(mtr-asi PRIVATE
    minhook
    imgui
    user32
    gdi32
    d3d9
    dinput8
    dxguid
    ws2_32
)

target_compile_definitions(mtr-asi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    MTR_ASI_VERSION="${PROJECT_VERSION}"
)

if (MSVC)
    target_compile_options(mtr-asi PRIVATE
        /W4 /permissive- /Zc:__cplusplus
        $<$<CONFIG:Release>:/O2 /GS- /Gy>
    )
    target_link_options(mtr-asi PRIVATE
        /INCREMENTAL:NO
        # /MAP emits mtr-asi.map next to the .asi — section + symbol table for
        # offline RVA→symbol lookup without needing dbghelp at runtime.
        # /MAPINFO:EXPORTS includes exported symbols (cheap, small impact).
        /MAP /MAPINFO:EXPORTS
        $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>
    )
endif()

set_target_properties(mtr-asi PROPERTIES
    OUTPUT_NAME "${MTR_ASI_OUTPUT_NAME}"
    SUFFIX ".asi"
    PREFIX ""
)

message(STATUS "mtr-asi v${PROJECT_VERSION}: building 32-bit Win32 .asi")
