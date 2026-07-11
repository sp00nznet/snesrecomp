cmake_minimum_required(VERSION 3.16)
project(snesrecomp C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SNESRECOMP_BUILD_EXAMPLES "Build example programs" ON)
option(SNESRECOMP_MENU "Build the Dear ImGui menu overlay (File/Graphics/Sound/Controller/Help)" ON)

# SDL2
find_package(SDL2 REQUIRED)

# --- Dear ImGui (static lib) — only when the menu overlay is enabled ---
if(SNESRECOMP_MENU)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR})
    target_link_libraries(imgui PUBLIC SDL2::SDL2)
    if(MSVC)
        target_compile_options(imgui PRIVATE /W0)
    else()
        target_compile_options(imgui PRIVATE -w)
    endif()
    set(SNESRECOMP_MENU_SRC src/menu_overlay.cpp)
else()
    set(SNESRECOMP_MENU_SRC src/menu_overlay_stub.c)
endif()

# --- LakeSnes hardware backend (static lib) ---
# We compile all of LakeSnes's snes/ directory. The CPU and input modules
# are included for completeness (DMA/etc. reference them), but in a recomp
# project the CPU is replaced by native recompiled code.
set(LAKESNES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/LakeSnes/snes)

add_library(lakesnes_hw STATIC
    ${LAKESNES_DIR}/apu.c
    ${LAKESNES_DIR}/cart.c
    ${LAKESNES_DIR}/cpu.c
    ${LAKESNES_DIR}/dma.c
    ${LAKESNES_DIR}/dsp.c
    ${LAKESNES_DIR}/dsp1.c
    ${LAKESNES_DIR}/gsu.c
    ${LAKESNES_DIR}/input.c
    ${LAKESNES_DIR}/ppu.c
    ${LAKESNES_DIR}/snes.c
    ${LAKESNES_DIR}/snes_other.c
    ${LAKESNES_DIR}/spc.c
    ${LAKESNES_DIR}/statehandler.c
)
target_include_directories(lakesnes_hw PUBLIC ${LAKESNES_DIR})
# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(lakesnes_hw PRIVATE /W0)
else()
    target_compile_options(lakesnes_hw PRIVATE -w)
endif()

# --- snesrecomp adapter library (static lib) ---
add_library(snesrecomp STATIC
    src/snesrecomp.c
    src/cpu.c
    src/bus.c
    src/func_table.c
    src/recomp_interp.c
    src/platform_sdl.c
    src/input.c
    src/mp_session.c
    ${SNESRECOMP_MENU_SRC}
)
target_include_directories(snesrecomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${LAKESNES_DIR}
)
target_link_libraries(snesrecomp
    PUBLIC  lakesnes_hw
    PUBLIC  SDL2::SDL2
)
if(WIN32)
    target_link_libraries(snesrecomp PUBLIC ws2_32)   # netplay sockets (mp_session)
endif()
if(SNESRECOMP_MENU)
    target_include_directories(snesrecomp PRIVATE ${IMGUI_DIR})
    target_link_libraries(snesrecomp PRIVATE imgui)
endif()

# --- Example: minimal recomp ---
if(SNESRECOMP_BUILD_EXAMPLES)
    add_executable(snesrecomp_minimal examples/minimal/main.c)
    target_link_libraries(snesrecomp_minimal PRIVATE snesrecomp SDL2::SDL2main)
endif()
