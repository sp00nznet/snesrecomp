# Architecture

How snesrecomp sits between a game's recompiled C and the LakeSnes hardware.
Code: `src/bus.c` (address routing), `src/snesrecomp.c` (frame loop, capture),
`src/recomp_interp.c` (timed loop, intercepts, validation), `src/platform_sdl.c`.

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

## Key design decisions

- **LakeSnes's CPU is compiled but not driven.** Your recompiled native C code replaces it. LakeSnes's CPU module is included in the build because other components (DMA, etc.) reference it, but `snes_runFrame()` is never called.

- **The `Snes` struct is the hardware backbone.** LakeSnes allocates and manages all hardware state (PPU, APU, DMA, Cart) inside a `Snes` struct. Our adapter layer routes bus reads/writes through it.

- **WRAM lives in LakeSnes.** The 128KB of WRAM is `snes->ram[]`. Stack operations, direct page access, everything goes through the real memory map.

- **Audio is real.** The SPC700 CPU and DSP run alongside your recompiled code. APU port writes (`$2140-$2143`) go to the real SPC700. Audio samples come out the other end.

## Two ways to run a game

- **Native frame** (`snesrecomp_begin_frame` / `snesrecomp_end_frame`): the
  game's recompiled C is the CPU. LakeSnes's CPU is never stepped.
- **Timed recomp** (`snesrecomp_realframe_begin` / `_end` plus
  `recomp_timed_add_intercept`): the genuine ROM runs in LakeSnes's cycle-timed
  frame, and an opcode-fetch hook swaps in recompiled C only at the addresses
  a game registers. This is how a title becomes playable before most of it is
  translated: what isn't recompiled yet still runs, on the emulated CPU.

## Why validation runs inside the timed loop

A recompiled function costs zero emulated cycles unless it calls
`recomp_tick()`, so intercepting it shifts later work onto other frames and a
snapshot diff flags correct functions too. `recomp_timed_add_validate` instead
saves the machine, runs the native body, keeps its result, restores, lets the
genuine routine run, and compares when it returns. Execution stays genuine, so a
mismatch belongs to that one function. See [runtime.md](runtime.md).
