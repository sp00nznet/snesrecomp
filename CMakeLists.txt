cmake_minimum_required(VERSION 3.16)
project(snesrecomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(SNESRECOMP_BUILD_EXAMPLES "Build example programs" ON)

# SDL2
find_package(SDL2 REQUIRED)

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
    src/platform_sdl.c
    src/input.c
)
target_include_directories(snesrecomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${LAKESNES_DIR}
)
target_link_libraries(snesrecomp
    PUBLIC  lakesnes_hw
    PUBLIC  SDL2::SDL2
)

# --- Example: minimal recomp ---
if(SNESRECOMP_BUILD_EXAMPLES)
    add_executable(snesrecomp_minimal examples/minimal/main.c)
    target_link_libraries(snesrecomp_minimal PRIVATE snesrecomp SDL2::SDL2main)
endif()
