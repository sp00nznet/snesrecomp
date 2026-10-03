# snesrecomp tools

Game-agnostic static-analysis tools for SNES recompilation projects. LoROM and
HiROM are auto-detected (override with a trailing `lorom`/`hirom` arg). Copier
headers (512-byte) are stripped automatically.

## `disasm65816.py` — 65816 disassembler

Tracks M/X flags so immediate operands are sized correctly.

```bash
py tools/disasm65816.py <rom> <bank:addr> [count] [mx_hex] [lorom|hirom]
# mx_hex: M/X flag byte — "00"=16-bit A+index, "30"=8-bit both (default 30)
py tools/disasm65816.py game.sfc 00:8000 40        # disassemble the reset chain
```

## `callgraph.py` — recursive-traversal call-graph crawler

Seeds from the CPU vectors (reset/NMI/IRQ), follows real control flow tracking
the program bank + M/X, and **resolves `JMP/JSR (abs,x)` jump tables** (SNES
dispatch is largely indirect — a direct-only crawl badly undercounts). Reports
distinct reachable functions, per-bank breakdown, jump tables resolved, code
coverage, and the **top functions by caller count (in-degree)** — the
highest-reuse recompilation targets.

```bash
py tools/callgraph.py <rom> [lorom|hirom]
```

It's a defensible static estimate, not a perfect disassembly: jump-table ends
aren't marked (reads may slightly over/under-count), RAM-stored function
pointers are invisible (undercount), and `PLP`-restored M/X flags aren't
tracked. For an exact picture, pair with Ghidra/IDA. For ranking what to
recompile next, pair with a runtime call profiler.

## `autogen.py` — 65816 → C generator

Lifts whole routines into C against the snesrecomp runtime
(`<snesrecomp/gen_ops.h>`: one reviewed helper per instruction family, ADC/SBC
mirroring LakeSnes including decimal mode).

```bash
py tools/autogen.py <rom> <list.txt> gen/game_gen.c
# list.txt: "BBAAAA P JSR|JSL" per routine, as MP_PROFILE / recomp_timed_profile_dump prints
```

Each routine becomes `gen_BBAAAA()`, and `gen_register_all()` registers them
(call it after hand-written registrations so generated bodies win). Routines it
cannot lift are listed at the top of the output with the reason.

It tracks M/X along every path (REP/SEP, PLP back to the matching PHP), and the
stack depth (pushes, pulls, `TSC : SEC : SBC #n : TCS` local frames). It refuses
what it cannot model faithfully: one instruction reached with two different
widths, pulling the return address (jump-table tricks), returning with bytes
still pushed, `TXS`/`XCE`/`BRK`/`WAI`/`STP`. `JSR`/`JSL` push the real return
address around the call; `JMP abs` is followed as a goto; `JML` and indirect
jumps are tail calls. Every instruction calls `recomp_tick()` with its estimated
cost so the timed loop stays in phase.

**The output is derived from the ROM: write it to a gitignored directory and
never commit it.** Check every routine with lockstep validation
(`recomp_timed_add_validate`) before relying on it.
