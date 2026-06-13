#!/usr/bin/env python3
"""
callgraph.py — recursive-traversal call-graph crawler for SNES ROMs (game-agnostic).

Unlike a naive byte scan (which counts every $20/$22/$60 byte, including data),
this follows actual control flow from the CPU vectors: it disassembles only
reachable instructions, tracking the program bank (PB) and the M/X flags through
REP/SEP so immediate operands are sized correctly. It records every JSR/JSL
target as a function, follows branches/jumps, and *resolves* JMP/JSR (abs,x)
jump tables heuristically (SNES dispatch is largely indirect — a direct-only
crawl badly undercounts). LoROM/HiROM is auto-detected.

Output: distinct reachable functions, per-bank breakdown, jump tables resolved,
remaining unresolved indirect sites, code coverage, and the top functions by
caller count (in-degree) — the highest-reuse recompilation targets.

Usage: py tools/callgraph.py <rom.smc> [lorom|hirom]

Caveats (it's a static estimate, not a perfect disassembly):
- Jump-table ends aren't marked, so table reads can slightly over/under-count.
- Function pointers stored in RAM then called are invisible -> undercount.
- PLP-restored flags can't be tracked; M/X is best-effort first-reached.
- In-bank JMP is treated as an intra-function continuation; JSR/JSL (and
  cross-bank JML) start new functions.
"""

import sys
import os
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm65816 import OPCODES, rom_read, detect_mapping, set_mapping


def rd(data, pb, pc):
    return rom_read(data, pb, pc)


def rd16(data, pb, pc):
    return rd(data, pb, pc) | (rd(data, pb, (pc + 1) & 0xFFFF) << 8)


def insn_len(name, extra, mode, m8, x8):
    if mode == "immA":
        return 1 + (1 if m8 else 2)
    if mode == "immX":
        return 1 + (1 if x8 else 2)
    return 1 + extra


def resolve_table(data, pb, base, cap=64):
    """Heuristically read a 16-bit jump table at `base` (program bank pb): take
    consecutive entries while they look like in-bank code pointers ($8000-$FFFF
    that decode to a known opcode). Approximate — the table end isn't marked, so
    this can slightly over- or under-read; cap bounds the damage."""
    out = []
    for i in range(cap):
        t = rd16(data, pb, (base + 2 * i) & 0xFFFF)
        if not (0x8000 <= t <= 0xFFFF):
            break
        if rd(data, pb, t) not in OPCODES:
            break
        out.append(t)
    return out


def crawl(data):
    functions = set()          # 24-bit function entry addresses
    edges = {}                 # callee -> set(caller) for in-degree
    visited = set()            # (pb<<16|pc) instruction starts
    unresolved = set()         # single-indirect JMP/JML sites not followed
    table_sites = set()        # JMP/JSR (abs,x) jump-table dispatch sites
    table_targets = 0          # total handler entries pulled from those tables
    work = []                  # (pb, pc, m8, x8, func24)

    # Seed from the bank-$00 CPU vectors (native + the reset).
    seeds = {}
    for name, voff in [("RESET", 0xFFFC), ("NMI_n", 0xFFEA), ("IRQ_n", 0xFFEE),
                       ("COP_n", 0xFFE4), ("BRK_n", 0xFFE6), ("ABORT_n", 0xFFE8),
                       ("NMI_e", 0xFFFA), ("IRQ_e", 0xFFFE), ("COP_e", 0xFFF4)]:
        tgt = rd16(data, 0x00, voff & 0xFFFF)
        if 0x8000 <= tgt <= 0xFFFF:
            seeds[name] = tgt
            f = (0x00 << 16) | tgt
            functions.add(f)
            edges.setdefault(f, set())
            work.append((0x00, tgt, True, True, f))

    while work:
        pb, pc, m8, x8, func = work.pop()
        while True:
            key = (pb << 16) | pc
            if key in visited:
                break
            visited.add(key)
            op = rd(data, pb, pc)
            if op not in OPCODES:
                break  # data / misalignment -> stop this run
            name, extra, mode = OPCODES[op]
            length = insn_len(name, extra, mode, m8, x8)
            nxt = (pc + length) & 0xFFFF

            if op == 0xC2:      # REP #imm -> clear M/X bits
                imm = rd(data, pb, (pc + 1) & 0xFFFF)
                if imm & 0x20: m8 = False
                if imm & 0x10: x8 = False
            elif op == 0xE2:    # SEP #imm -> set M/X bits
                imm = rd(data, pb, (pc + 1) & 0xFFFF)
                if imm & 0x20: m8 = True
                if imm & 0x10: x8 = True
            elif op == 0x20:    # JSR abs (in-bank call)
                t = (pb << 16) | rd16(data, pb, (pc + 1) & 0xFFFF)
                functions.add(t); edges.setdefault(t, set()).add(func)
                work.append((pb, t & 0xFFFF, m8, x8, t))
            elif op == 0x22:    # JSL long (call)
                lo = rd16(data, pb, (pc + 1) & 0xFFFF)
                t = lo | (rd(data, pb, (pc + 3) & 0xFFFF) << 16)
                functions.add(t); edges.setdefault(t, set()).add(func)
                work.append(((t >> 16) & 0xFF, t & 0xFFFF, m8, x8, t))
            elif op == 0xFC:    # JSR (abs,x) -> indirect call via jump table
                table_sites.add((pb << 16) | pc)
                base = rd16(data, pb, (pc + 1) & 0xFFFF)
                for h in resolve_table(data, pb, base):
                    t = (pb << 16) | h
                    functions.add(t); edges.setdefault(t, set()).add(func)
                    work.append((pb, h, m8, x8, t)); table_targets += 1
            elif op == 0x4C:    # JMP abs (in-bank) -> continuation, no fall-through
                work.append((pb, rd16(data, pb, (pc + 1) & 0xFFFF), m8, x8, func))
                break
            elif op == 0x5C:    # JML long -> treat target as a new function (tail-call)
                lo = rd16(data, pb, (pc + 1) & 0xFFFF)
                t = lo | (rd(data, pb, (pc + 3) & 0xFFFF) << 16)
                functions.add(t); edges.setdefault(t, set()).add(func)
                work.append(((t >> 16) & 0xFF, t & 0xFFFF, m8, x8, t))
                break
            elif op == 0x7C:    # JMP (abs,x) -> jump table of handlers
                table_sites.add((pb << 16) | pc)
                base = rd16(data, pb, (pc + 1) & 0xFFFF)
                for h in resolve_table(data, pb, base):
                    t = (pb << 16) | h
                    functions.add(t); edges.setdefault(t, set()).add(func)
                    work.append((pb, h, m8, x8, t)); table_targets += 1
                break
            elif op in (0x6C, 0xDC):  # JMP (abs) / JML [abs] single indirect -> unresolved
                unresolved.add((pb << 16) | pc)
                break
            elif op in (0x60, 0x6B, 0x40, 0xDB):  # RTS/RTL/RTI/STP -> stop
                break
            elif op == 0x80:    # BRA rel8 (unconditional)
                rel = rd(data, pb, (pc + 1) & 0xFFFF)
                if rel >= 0x80: rel -= 0x100
                work.append((pb, (pc + 2 + rel) & 0xFFFF, m8, x8, func))
                break
            elif op == 0x82:    # BRL rel16 (unconditional)
                rel = rd16(data, pb, (pc + 1) & 0xFFFF)
                if rel >= 0x8000: rel -= 0x10000
                work.append((pb, (pc + 3 + rel) & 0xFFFF, m8, x8, func))
                break
            elif op in (0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0):  # cond branch
                rel = rd(data, pb, (pc + 1) & 0xFFFF)
                if rel >= 0x80: rel -= 0x100
                work.append((pb, (pc + 2 + rel) & 0xFFFF, m8, x8, func))
                # fall through to next instruction

            pc = nxt

    return functions, edges, visited, unresolved, table_sites, table_targets, seeds


def main():
    if len(sys.argv) < 2:
        print("Usage: py tools/callgraph.py <rom.smc>")
        return 1
    data = Path(sys.argv[1]).read_bytes()
    if len(data) % 1024 == 512:
        data = data[512:]   # strip copier header
    if len(sys.argv) > 2 and sys.argv[2] in ("lorom", "hirom"):
        set_mapping(sys.argv[2]); mapping = sys.argv[2]
    else:
        mapping = detect_mapping(data)
    functions, edges, visited, unresolved, table_sites, table_targets, seeds = crawl(data)

    from collections import Counter
    by_bank = Counter((f >> 16) for f in functions)

    print(f"ROM: {len(data)} bytes ({len(data)//1024} KB), {mapping.upper()}")
    print("Seed vectors: " + ", ".join(f"{k}=${v:04X}" for k, v in seeds.items()))
    print()
    print(f"Reachable functions (JSR/JSL/JML targets + vectors): {len(functions)}")
    print(f"Reachable code bytes (instruction starts):           {len(visited)}")
    print(f"  approx code coverage of ROM:                       {100*len(visited)/len(data):.1f}%")
    print(f"Jump-table dispatch sites resolved:                  {len(table_sites)} (-> {table_targets} handler entries)")
    print(f"Single-indirect JMP/JML sites NOT followed:          {len(unresolved)} (more functions hide here)")
    print()
    print("Reachable functions by bank (top 20):")
    for b, n in by_bank.most_common(20):
        print(f"  bank ${b:02X}: {n}")
    print()
    print("Top 20 functions by caller count (in-degree) -- structural hubs:")
    ranked = sorted(functions, key=lambda f: len(edges.get(f, ())), reverse=True)
    for f in ranked[:20]:
        print(f"  ${f:06X}  callers={len(edges.get(f, ()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
