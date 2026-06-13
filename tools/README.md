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
