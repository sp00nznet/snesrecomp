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
| **Memory Bus** | 24-bit address routing to all hardware | Adapter over LakeSnes |
| **Function Table** | Hash-table dispatch for recompiled functions by SNES address | Recomp utility |
| **Platform** | SDL2 window, renderer, audio output, frame timing | SDL2 |
| **Input** | Joypad + SNES Mouse + Super Scope, keyboard/mouse mapping, hardware auto-read | SDL2 + LakeSnes |

## Quick Start

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

### 3. Run the minimal example

```bash
./build/Debug/snesrecomp_minimal "path/to/your/game.sfc"
```

## Using snesrecomp in your project

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

/* A recompiled SNES function — this is what the original 65816 code becomes */
void smk_808056(void) {
    /* Original:  LDA #$80 / STA $2100   (force blank on) */
    CPU_SET_A8(0x80);
    bus_write8(0x00, 0x2100, CPU_A8());   /* -> real PPU INIDISP register */

    /* Original:  STZ $4200              (disable NMI) */
    bus_write8(0x00, 0x4200, 0x00);       /* -> real NMI control register */
}

int main(int argc, char *argv[]) {
    snesrecomp_init("Super Mario Kart", 3);
    snesrecomp_load_rom(argv[1]);

    /* Register recompiled functions */
    func_table_register(0x808056, smk_808056);

    while (snesrecomp_begin_frame()) {
        /* Run the game's main loop function */
        func_table_call(0x808056);

        snesrecomp_end_frame();  /* renders PPU + presents */
    }

    snesrecomp_shutdown();
    return 0;
}
```

Every `bus_write8` to a PPU register updates real PPU state. Every `bus_read8` from an APU port reads real SPC700 output. DMA transfers move real data between real hardware components. **You don't write any hardware emulation code — it's all already there.**

## Architecture

```
┌─────────────────────────────────────────────┐
│           Your Recomp Project               │
│  ┌─────────────┐  ┌──────────────────────┐  │
│  │ Recompiled   │  │ Game-specific logic  │  │
│  │ functions    │  │ (main loop, init)    │  │
│  └──────┬───────┘  └──────────┬───────────┘  │
│         │                     │              │
│  ┌──────▼─────────────────────▼───────────┐  │
│  │         snesrecomp library             │  │
│  │  ┌──────────┐  ┌───────────────────┐   │  │
│  │  │ CPU State │  │  Memory Bus       │   │  │
│  │  │ (g_cpu)   │  │  bus_read/write8  │   │  │
│  │  └──────────┘  └────────┬──────────┘   │  │
│  │  ┌──────────┐           │              │  │
│  │  │ Func     │  ┌────────▼──────────┐   │  │
│  │  │ Table    │  │    LakeSnes HW    │   │  │
│  │  └──────────┘  │  PPU APU DMA Cart │   │  │
│  │                └────────┬──────────┘   │  │
│  │  ┌──────────────────────▼──────────┐   │  │
│  │  │  SDL2 Platform (video + audio)  │   │  │
│  │  └────────────────────────────────┘   │  │
│  └────────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

### Key design decisions

- **LakeSnes's CPU is compiled but not driven.** Your recompiled native C code replaces it. LakeSnes's CPU module is included in the build because other components (DMA, etc.) reference it, but `snes_runFrame()` is never called.

- **The `Snes` struct is the hardware backbone.** LakeSnes allocates and manages all hardware state (PPU, APU, DMA, Cart) inside a `Snes` struct. Our adapter layer routes bus reads/writes through it.

- **WRAM lives in LakeSnes.** The 128KB of WRAM is `snes->ram[]`. Stack operations, direct page access, everything goes through the real memory map.

- **Audio is real.** The SPC700 CPU and DSP run alongside your recompiled code. APU port writes (`$2140-$2143`) go to the real SPC700. Audio samples come out the other end.

## Want to recomp a SNES game?

Here's the general workflow:

1. **Get a disassembly** of your target game (or create one with tools like Mesen2's trace logger)
2. **Identify functions** — find subroutine boundaries (JSR/JSL/RTS/RTL patterns)
3. **Recompile functions** to C — each 65816 instruction becomes a line of C using the CPU state macros and bus functions
4. **Register them** in the function table at their original SNES addresses
5. **Wire up the main loop** — call the game's entry point function each frame
6. **Link against snesrecomp** — all hardware just works

The [Super Mario Kart recompilation](https://github.com/sp00nznet/mk) and [Mario Paint recompilation](https://github.com/sp00nznet/mariopaint) are projects using this library. Check them out for real-world examples.

## Credits

- **[LakeSnes](https://github.com/angelo-wf/LakeSnes)** by angelo-wf — the excellent pure-C SNES emulator that provides all hardware emulation. MIT licensed.
- **[SDL2](https://www.libsdl.org/)** — cross-platform multimedia. zlib licensed.
- **[sp00nznet](https://github.com/sp00nznet)** — library design, adapter layer, and the recomp projects that use it.

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, ship games with it.

LakeSnes is also MIT licensed. SDL2 is zlib licensed. You're good.
