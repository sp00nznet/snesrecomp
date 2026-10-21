# snesrecomp

**Drop-in SNES hardware for your static recompilation project.**

You write the recompiled game code. We give you real PPU rendering, SPC700 audio, DMA, Mode 7, and everything else the SNES had — as a linkable C library.

```
Your Recompiled Game Code
         |
         v
    bus_write8(0x00, 0x2118, val)   <-- writes to VRAM data port
         |
         v
    +-----------+
    | snesrecomp |  <-- this library
    +-----------+
         |
         v
    +------------+
    |  LakeSnes   |  <-- real SNES PPU/APU/DMA hardware (MIT licensed)
    +------------+
         |
         v
    SDL2 Window + Audio Output
```

Part of the recomp family (snesrecomp, gbarecomp, lynxrecomp, xboxrecomp,
ps3recomp, pcrecomp, ...) and follows the same house style: toolkit and game repos
kept apart, generated code never committed, `--headless --record` on every build, a
conformance harness against a reference.

## Status

**Alpha. Unversioned.** The hardware layer is LakeSnes and is complete; the
recompilation side (timed loop, lockstep validation, `autogen.py`) is new and
moving. Titles built on it:

| Title | Repo | State |
|---|---|---|
| Super Mario Kart | [mk](https://github.com/sp00nznet/mk) | Playable end to end (genuine ROM in the timed frame); recompiled shells growing |
| Mario Paint | [mariopaint](https://github.com/sp00nznet/mariopaint) | Title, canvas and bottom bar work; 57/103 hand-ports pass conformance |
| Stunt Race FX | [stuntrace](https://github.com/sp00nznet/stuntrace) | Boots with Super FX (GSU-2 in the LakeSnes fork); game logic in progress |

The toolkit has no conformance harness of its own yet; the game repos carry
theirs (see [ROADMAP.md](ROADMAP.md)).

## Screenshots

Mario Paint, running on snesrecomp:

![Mario Paint canvas](https://raw.githubusercontent.com/sp00nznet/mariopaint/main/docs/screenshots/canvas-timed.png)

## What is this?

Static recompilation takes a console game's machine code and converts it into equivalent native C code that runs on modern hardware. The hard part isn't recompiling the CPU instructions — it's providing the **hardware** those instructions talk to.

Every SNES game writes to the same PPU registers, the same APU ports, the same DMA channels. So why reimplement that for every single recomp project?

**snesrecomp** solves this by packaging a real, battle-tested SNES emulator's hardware backend ([LakeSnes](https://github.com/sp00nznet/LakeSnes), forked from angelo-wf) as a linkable static library. Your recompiled code calls `bus_write8(bank, addr, val)` and gets real PPU behavior, real audio processing, real DMA transfers — all for free.

This is the same approach used by [N64Recomp](https://github.com/N64Recomp/N64Recomp) (which uses parallel-rdp for graphics) and other successful recomp projects. **Chop up an emulator, turn the hardware into libraries, let game-specific projects link against them.**

## What you get

| Component | What it does | Status |
|-----------|-------------|--------|
| **PPU** | Full scanline rendering, Mode 0-7, sprites, windows, color math, hi-res | Real (LakeSnes) |
| **APU/SPC700** | Full SPC700 CPU + BRR audio DSP, 8 channels, echo, noise | Real (LakeSnes) |
| **DMA** | All 8 channels, GPDMA + HDMA, proper A-bus/B-bus routing | Real (LakeSnes) |
| **Cartridge** | LoROM, HiROM, ExHiROM auto-detection, SRAM, DSP-1 coprocessor | Real (LakeSnes) |
| **CPU I/O** | NMI/IRQ, multiply/divide ALU, joypad auto-read | Real (LakeSnes) |
| **CPU State** | 65816 register struct (A, X, Y, S, DP, DB, PB, flags) | Recomp adapter |
| **65816 Op Kit** | `cpu_ops.h` — inline helpers for `LDA`, `STA`, `REP/SEP`, `PHP/PLP`, etc. | Recomp utility |
| **Memory Bus** | 24-bit address routing to all hardware | Adapter over LakeSnes |
| **Auto Dispatch** | `RECOMP_PATCH(name, addr)` — recompiled fn auto-registers at SNES address before `main()` | Recomp utility |
| **Function Table** | Hash-table dispatch for `JSR`/`JSL` indirection by SNES address | Recomp utility |
| **Platform** | SDL2 window, renderer, audio output, frame timing | SDL2 |
| **Input** | Joypad + SNES Mouse + Super Scope, keyboard/mouse mapping, hardware auto-read | SDL2 + LakeSnes |

## Getting Started

The toolkit is used from a game repo, and the game repo's README is the quick
start (it fetches this repo as a submodule). To build the toolkit on its own:

### 1. Clone with submodules

```bash
git clone --recursive https://github.com/sp00nznet/snesrecomp.git
cd snesrecomp
```

### 2. Build

```bash
# Windows (MSVC + vcpkg)
cmake -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug

# Linux / macOS
cmake -B build
cmake --build build
```

Prerequisites: CMake 3.16+, a C11/C++17 compiler (Visual Studio 2022, GCC or
Clang), SDL2 (`vcpkg install sdl2:x64-windows` on Windows), and optionally
SDL2_ttf for the menu font. Clone with `--recursive`: LakeSnes is a submodule
and the build fails without it.

### 3. Run the minimal example

```bash
./build/Debug/snesrecomp_minimal "path/to/your/game.sfc"
./build/Debug/snesrecomp_minimal game.sfc --headless --record out.mp4   # no window
```

On Windows, copy `SDL2.dll` next to the exe.

## Usage

### Add as a subdirectory (recommended)

```cmake
# In your game's CMakeLists.txt:
add_subdirectory(ext/snesrecomp)

add_executable(my_recomp src/main.c src/game_functions.c)
target_link_libraries(my_recomp PRIVATE snesrecomp SDL2::SDL2main)
```

### Write your recompiled game code

```c
#include <snesrecomp/snesrecomp.h>

/* A recompiled SNES function — this is what the original 65816 code becomes.
 * RECOMP_PATCH defines the function and auto-registers it in the dispatch
 * table at its original SNES bank:address before main() runs. No central
 * registration list needed. */
RECOMP_PATCH(smk_808056, 0x808056) {
    /* Original:  LDA #$80 / STA $2100   (force blank on) */
    CPU_SET_A8(0x80);
    bus_write8(0x00, 0x2100, CPU_A8());   /* -> real PPU INIDISP register */

    /* Original:  STZ $4200              (disable NMI) */
    bus_write8(0x00, 0x4200, 0x00);       /* -> real NMI control register */
}

int main(int argc, char *argv[]) {
    snesrecomp_init("Super Mario Kart", 3);
    snesrecomp_load_rom(argv[1]);

    while (snesrecomp_begin_frame()) {
        /* Call recompiled functions directly... */
        smk_808056();
        /* ...or dispatch by SNES address (used by recompiled JSR/JSL). */
        func_table_call(0x808056);

        snesrecomp_end_frame();  /* renders PPU + presents */
    }

    snesrecomp_shutdown();
    return 0;
}
```

### 65816 op kit

`<snesrecomp/cpu_ops.h>` (auto-included via `snesrecomp.h`) provides
inline helpers for the common 65816 instructions — `op_lda_imm16`,
`op_sta_dp16`, `op_rep`, `op_sep`, `op_php`/`op_plp`, etc. — that read
and write the global `g_cpu` state and route memory through the bus.
Pull from this kit when translating instruction sequences; extend with
project-specific ops as needed.

Every `bus_write8` to a PPU register updates real PPU state. Every `bus_read8` from an APU port reads real SPC700 output. DMA transfers move real data between real hardware components. **You don't write any hardware emulation code — it's all already there.**

## Documentation

- [docs/architecture.md](docs/architecture.md): the layers, the two run modes, design decisions.
- [docs/runtime.md](docs/runtime.md): command-line flags, environment variables, validation and profiling.
- [tools/README.md](tools/README.md): disassembler, call-graph crawler, `autogen.py`.

## Want to recomp a SNES game?

Here's the general workflow:

1. **Get a disassembly** of your target game (`tools/disasm65816.py`, `tools/callgraph.py`)
2. **Identify functions** — find subroutine boundaries (JSR/JSL/RTS/RTL patterns)
3. **Recompile functions** to C — wrap each in `RECOMP_PATCH(name, snes_addr) { ... }`. Translate 65816 instructions using the `cpu_ops.h` op kit (`op_lda_imm16`, `op_sta_dp16`, …) and the bus functions. Auto-registration handles dispatch — no central list to maintain.
4. **Wire up the main loop** — call the game's entry point function each frame
5. **Link against snesrecomp** — all hardware just works

### Modding / overriding functions

Because `RECOMP_PATCH` registrations run at static-init time and the dispatch table just keeps the last writer, you can override any recompiled function by linking another translation unit that defines a `RECOMP_PATCH` at the same SNES address. Put mod objects after the originals in the link order and they win.

The [Super Mario Kart recompilation](https://github.com/sp00nznet/mk) and [Mario Paint recompilation](https://github.com/sp00nznet/mariopaint) are projects using this library. Check them out for real-world examples.

## Building from source

As in Getting Started. A game repo builds it with `add_subdirectory(ext/snesrecomp)`.
It also cross-builds with mingw-w64, which is how the netlab farm builds it:

```bash
cmake -S . -B build -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_PREFIX_PATH=<SDL2 mingw devel folder>
cmake --build build
```

## Legal

snesrecomp contains no game data. You supply your own ROM, and anything
`tools/autogen.py` generates from it stays on your machine (`gen/` is gitignored).

## Credits

- **[LakeSnes](https://github.com/angelo-wf/LakeSnes)** by angelo-wf (used through the [sp00nznet fork](https://github.com/sp00nznet/LakeSnes)) — the excellent pure-C SNES emulator that provides all hardware emulation. MIT licensed.
- **[Dear ImGui](https://github.com/ocornut/imgui)**: the menu overlay. MIT licensed.
- **[stb_image_write](https://github.com/nothings/stb)**: screenshots. MIT / public domain.
- **[SDL2](https://www.libsdl.org/)** — cross-platform multimedia. zlib licensed.
- **[sp00nznet](https://github.com/sp00nznet)** — library design, adapter layer, and the recomp projects that use it.

## License

MIT, see [LICENSE](LICENSE). Third-party code (LakeSnes, Dear ImGui,
stb_image_write) keeps its own licence; see [NOTICE](NOTICE) and `LICENSES/`.
Contributions: [CONTRIBUTING.md](CONTRIBUTING.md).
