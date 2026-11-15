cmake_minimum_required(VERSION 3.20)
project(mxb_physics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "mxbikes.exe is x64 - configure a 64-bit generator")
endif()

# The reference menu ships its own ImGui fork (1.70 WIP) with the custom widgets
# the layout depends on, so it is built from there rather than from a package.
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/skrt menu clone/imgui")
set(MENU_DIR  "${CMAKE_SOURCE_DIR}/skrt menu clone")

find_path(MINHOOK_INCLUDE_DIR MinHook.h)
find_library(MINHOOK_LIBRARY NAMES minhook minhook.x64)
if(NOT MINHOOK_INCLUDE_DIR OR NOT MINHOOK_LIBRARY)
    message(FATAL_ERROR
        "MinHook not found. Install it with:\n"
        "  vcpkg install minhook:x64-windows\n"
        "and configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

add_library(mxb_physics SHARED
    src/dllmain.cpp

    src/mxb/Process.cpp
    src/mxb/Session.cpp
    src/mxb/Diagnostics.cpp
    src/mxb/CrashPatch.cpp
    src/mxb/Modifiers.cpp
    src/mxb/Spinny.cpp
    src/mxb/NetworkRam.cpp
    src/mxb/GuidSpoofer.cpp

    src/ui/ModMenu.cpp
    src/ui/Watermark.cpp
    src/ui/Overlays.cpp
    src/mxb/Visuals.cpp
    src/mxb/CleanLens.cpp
    src/mxb/World.cpp
    src/mxb/Probe.cpp
    src/mxb/CarMode.cpp
    src/mxb/Chams.cpp

    src/host/Hooks.cpp
    src/host/Log.cpp
    src/host/Overlay.cpp
    src/host/imgui_impl_gl2.cpp

    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_impl_win32.cpp"
)

target_include_directories(mxb_physics PRIVATE
    src
    src/compat          # stub for the ImGui fork's leftover #include "Globals.h"
    "${IMGUI_DIR}"
    "${MENU_DIR}"
    "${MINHOOK_INCLUDE_DIR}"
)

target_link_libraries(mxb_physics PRIVATE
    "${MINHOOK_LIBRARY}"
    opengl32
    gdi32
    user32
    ole32
    windowscodecs
)

target_compile_definitions(mxb_physics PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(mxb_physics PRIVATE /W3 /permissive-)
    # Static CRT, so the DLL does not need a redistributable present in the
    # injected process.
    set_property(TARGET mxb_physics PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(mxb_physics PRIVATE $<$<CONFIG:Debug>:/MTd> $<$<NOT:$<CONFIG:Debug>>:/MT>)
endif()

set_target_properties(mxb_physics PROPERTIES
    OUTPUT_NAME "mxb_physics"
    PREFIX ""
)

# Standalone read-only capture/file test; never loads or injects the game DLL.
add_executable(mxb_diagnostics_smoke EXCLUDE_FROM_ALL
    tests/DiagnosticsSmoke.cpp
    src/mxb/Diagnostics.cpp
    src/host/Log.cpp
)
target_include_directories(mxb_diagnostics_smoke PRIVATE src)
target_compile_definitions(mxb_diagnostics_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(mxb_stock_view_smoke EXCLUDE_FROM_ALL
    tests/StockBikeViewSmoke.cpp
    src/mxb/Modifiers.cpp
)
target_include_directories(mxb_stock_view_smoke PRIVATE src)
target_compile_definitions(mxb_stock_view_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(mxb_spinny_smoke EXCLUDE_FROM_ALL tests/SpinnySmoke.cpp src/mxb/Spinny.cpp)
target_include_directories(mxb_spinny_smoke PRIVATE src)
target_compile_definitions(mxb_spinny_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(mxb_local_bike_smoke EXCLUDE_FROM_ALL tests/LocalBikeSmoke.cpp)
target_include_directories(mxb_local_bike_smoke PRIVATE src)
target_compile_definitions(mxb_local_bike_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(mxb_guid_smoke EXCLUDE_FROM_ALL tests/GuidSmoke.cpp src/mxb/GuidSpoofer.cpp)
target_include_directories(mxb_guid_smoke PRIVATE src)
target_compile_definitions(mxb_guid_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mxb_guid_smoke PRIVATE ole32)

add_executable(mxb_clean_lens_smoke EXCLUDE_FROM_ALL tests/CleanLensSmoke.cpp src/mxb/CleanLens.cpp)
target_include_directories(mxb_clean_lens_smoke PRIVATE src)
target_compile_definitions(mxb_clean_lens_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(mxb_network_ram_smoke EXCLUDE_FROM_ALL tests/NetworkRamSmoke.cpp src/mxb/NetworkRam.cpp)
target_include_directories(mxb_network_ram_smoke PRIVATE src)
target_compile_definitions(mxb_network_ram_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
